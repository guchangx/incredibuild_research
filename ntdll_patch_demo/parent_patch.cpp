#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winternl.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using NtWriteFileType = NTSTATUS(NTAPI*)(
    HANDLE FileHandle,
    HANDLE Event,
    PIO_APC_ROUTINE ApcRoutine,
    PVOID ApcContext,
    PIO_STATUS_BLOCK IoStatusBlock,
    PVOID Buffer,
    ULONG Length,
    PLARGE_INTEGER ByteOffset,
    PULONG Key);

struct RemotePatch {
    void* target = nullptr;
    void* hook = nullptr;
    unsigned char original[16]{};
    SIZE_T originalSize = 0;
};

// Wraps a command-line argument in double quotes.
// 用双引号包裹命令行参数。
static std::wstring quote(const std::wstring& value) {
    return L"\"" + value + L"\"";
}

// Writes a complete null-terminated text buffer to a file handle.
// 将完整的空字符结尾文本缓冲区写入文件句柄。
static bool writeAll(HANDLE file, const char* text) {
    DWORD written = 0;
    return WriteFile(file, text, static_cast<DWORD>(std::strlen(text)), &written, nullptr) &&
           written == std::strlen(text);
}

// Creates a marker file proving the parent can still write normally.
// 创建标记文件，证明父进程仍可正常写入。
static bool createMarkerFile(const std::wstring& path) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }
    bool ok = writeAll(file, "parent can still write normally\n");
    CloseHandle(file);
    return ok;
}

// Reads the main executable image base from the remote process PEB.
// 从远程进程 PEB 中读取主可执行映像的基地址。
static bool getRemoteImageBase(HANDLE process, std::uintptr_t& base) {
    using NtQueryInformationProcessType = NTSTATUS(NTAPI*)(
        HANDLE, PROCESSINFOCLASS, PVOID, ULONG, PULONG);
    auto ntQueryInformationProcess = reinterpret_cast<NtQueryInformationProcessType>(
        GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationProcess"));
    if (!ntQueryInformationProcess) {
        return false;
    }

    PROCESS_BASIC_INFORMATION pbi{};
    NTSTATUS status = ntQueryInformationProcess(process, ProcessBasicInformation,
                                                &pbi, sizeof(pbi), nullptr);
    if (status < 0 || !pbi.PebBaseAddress) {
        return false;
    }

#if defined(_M_X64)
    auto imageBaseSlot = reinterpret_cast<const char*>(pbi.PebBaseAddress) + 0x10;
#else
#error This demo is x64-only.
#endif

    SIZE_T read = 0;
    return ReadProcessMemory(process, imageBaseSlot, &base, sizeof(base), &read) &&
           read == sizeof(base) && base != 0;
}

// Finds the child process address of the exported MyWriteFile function.
// 查找子进程中导出的 MyWriteFile 函数地址。
static bool findRemoteMyWriteFile(HANDLE process, const std::wstring& childExe,
                                  void*& remoteFunction) {
    std::uintptr_t remoteImageBase = 0;
    if (!getRemoteImageBase(process, remoteImageBase)) {
        std::printf("failed to read child image base: %lu\n", GetLastError());
        return false;
    }

    HMODULE localChild = LoadLibraryExW(childExe.c_str(), nullptr, DONT_RESOLVE_DLL_REFERENCES);
    if (!localChild) {
        std::printf("LoadLibraryExW(child) failed: %lu\n", GetLastError());
        return false;
    }

    auto* localFunction = reinterpret_cast<std::uint8_t*>(
        GetProcAddress(localChild, "MyWriteFile"));
    if (!localFunction) {
        std::printf("GetProcAddress(MyWriteFile) failed: %lu\n", GetLastError());
        FreeLibrary(localChild);
        return false;
    }

    auto* localBase = reinterpret_cast<std::uint8_t*>(localChild);
    const std::uintptr_t functionRva =
        static_cast<std::uintptr_t>(localFunction - localBase);
    remoteFunction = reinterpret_cast<void*>(remoteImageBase + functionRva);
    FreeLibrary(localChild);
    return true;
}

// Replaces the child process NtWriteFile entry point with the child MyWriteFile function.
// 将子进程的 NtWriteFile 入口替换为子进程中的 MyWriteFile 函数。
static bool patchRemoteNtWriteFile(HANDLE process, const std::wstring& childExe,
                                   RemotePatch& patch) {
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (!ntdll) {
        std::printf("GetModuleHandleW(ntdll) failed: %lu\n", GetLastError());
        return false;
    }

    auto localNtWriteFile = reinterpret_cast<unsigned char*>(
        GetProcAddress(ntdll, "NtWriteFile"));
    if (!localNtWriteFile) {
        std::printf("GetProcAddress(NtWriteFile) failed: %lu\n", GetLastError());
        return false;
    }

    // System DLLs are mapped at the same base in normal same-architecture child
    // processes on this machine. This keeps the demo focused on patch mechanics.
    patch.target = localNtWriteFile;
    patch.originalSize = 12;
    std::memcpy(patch.original, localNtWriteFile, patch.originalSize);

    if (!findRemoteMyWriteFile(process, childExe, patch.hook)) {
        return false;
    }

    unsigned char jump[12] = {
        0x48, 0xB8,             // mov rax, imm64
        0, 0, 0, 0, 0, 0, 0, 0,
        0xFF, 0xE0              // jmp rax
    };
    std::uint64_t hookAddress = reinterpret_cast<std::uint64_t>(patch.hook);
    std::memcpy(&jump[2], &hookAddress, sizeof(hookAddress));

    DWORD oldProtect = 0;
    if (!VirtualProtectEx(process, patch.target, sizeof(jump), PAGE_EXECUTE_READWRITE, &oldProtect)) {
        std::printf("VirtualProtectEx(target) failed: %lu\n", GetLastError());
        return false;
    }

    SIZE_T written = 0;
    if (!WriteProcessMemory(process, patch.target, jump, sizeof(jump), &written) ||
        written != sizeof(jump)) {
        std::printf("WriteProcessMemory(target) failed: %lu\n", GetLastError());
        return false;
    }

    FlushInstructionCache(process, patch.target, sizeof(jump));

    DWORD ignored = 0;
    VirtualProtectEx(process, patch.target, sizeof(jump), oldProtect, &ignored);
    std::printf("patched child NtWriteFile to child MyWriteFile at %p\n", patch.hook);
    return true;
}

// Returns a file size and reports whether the file exists.
// 返回文件大小，并报告文件是否存在。
static std::uint64_t fileSizeOrMissing(const std::wstring& path, bool& exists) {
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data)) {
        exists = false;
        return 0;
    }
    exists = true;
    ULARGE_INTEGER size{};
    size.HighPart = data.nFileSizeHigh;
    size.LowPart = data.nFileSizeLow;
    return size.QuadPart;
}

// Runs the child with NtWriteFile patched and verifies that payload bytes are swallowed.
// 在修补 NtWriteFile 后运行子进程，并验证载荷字节被吞掉。
int wmain() {
    wchar_t modulePath[MAX_PATH]{};
    GetModuleFileNameW(nullptr, modulePath, MAX_PATH);
    std::wstring exeDir(modulePath);
    exeDir.resize(exeDir.find_last_of(L"\\/"));

    std::wstring childExe = exeDir + L"\\child.exe";
    std::wstring childOutput = exeDir + L"\\child_output.txt";
    std::wstring parentMarker = exeDir + L"\\parent_marker.txt";

    DeleteFileW(childOutput.c_str());
    DeleteFileW(parentMarker.c_str());

    if (!createMarkerFile(parentMarker)) {
        std::printf("failed to create parent marker: %lu\n", GetLastError());
        return 10;
    }

    std::wstring commandLine = quote(childExe) + L" " + quote(childOutput);
    std::vector<wchar_t> mutableCommand(commandLine.begin(), commandLine.end());
    mutableCommand.push_back(L'\0');

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(childExe.c_str(), mutableCommand.data(), nullptr, nullptr, FALSE,
                        CREATE_SUSPENDED, nullptr, exeDir.c_str(), &si, &pi)) {
        std::printf("CreateProcessW failed: %lu\n", GetLastError());
        return 11;
    }

    RemotePatch patch{};
    if (!patchRemoteNtWriteFile(pi.hProcess, childExe, patch)) {
        TerminateProcess(pi.hProcess, 100);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        return 12;
    }

    ResumeThread(pi.hThread);
    WaitForSingleObject(pi.hProcess, 10000);

    DWORD exitCode = 0;
    GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);

    bool childExists = false;
    std::uint64_t childSize = fileSizeOrMissing(childOutput, childExists);
    bool parentExists = false;
    std::uint64_t parentSize = fileSizeOrMissing(parentMarker, parentExists);

    std::printf("child exit code: %lu\n", exitCode);
    std::printf("parent marker exists: %s, size: %llu\n", parentExists ? "yes" : "no",
                static_cast<unsigned long long>(parentSize));
    std::printf("child output exists: %s, size: %llu\n", childExists ? "yes" : "no",
                static_cast<unsigned long long>(childSize));

    if (exitCode != 0 || !parentExists || parentSize == 0) {
        return 20;
    }

    // CreateFileW in the child may still create a zero-byte file before the
    // patched NtWriteFile swallows the payload.
    if (childExists && childSize != 0) {
        return 21;
    }

    std::printf("PASS: child WriteFile returned success, but its payload was not written.\n");
    return 0;
}
