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

// Replaces the child process NtWriteFile entry point with a success-only stub.
// 将子进程的 NtWriteFile 入口替换为只返回成功的桩代码。
static bool patchRemoteNtWriteFile(HANDLE process, RemotePatch& patch) {
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

    // x64 code:
    //   mov dword ptr [r9+0], 0      ; IO_STATUS_BLOCK.Status = STATUS_SUCCESS
    //   mov qword ptr [r9+8], r9     ; temporary nonzero value
    //   mov [r9+8], rax              ; Information = 0
    //   xor eax, eax                 ; return STATUS_SUCCESS
    //   ret
    //
    // NtWriteFile's fifth parameter is IoStatusBlock. Under the Windows x64 ABI,
    // parameters 1-4 are rcx/rdx/r8/r9 and parameter 5 is on the stack. However,
    // ntdll syscall stubs enter before stack parameters are rearranged for kernel
    // mode. To avoid relying on that layout, this demo uses a smaller hook that
    // simply returns STATUS_SUCCESS. The child only checks WriteFile's BOOL result.
    unsigned char hookCode[] = {
        0x31, 0xC0, // xor eax,eax
        0xC3        // ret
    };

    patch.hook = VirtualAllocEx(process, nullptr, sizeof(hookCode),
                                MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!patch.hook) {
        std::printf("VirtualAllocEx(hook) failed: %lu\n", GetLastError());
        return false;
    }

    SIZE_T written = 0;
    if (!WriteProcessMemory(process, patch.hook, hookCode, sizeof(hookCode), &written) ||
        written != sizeof(hookCode)) {
        std::printf("WriteProcessMemory(hook) failed: %lu\n", GetLastError());
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

    written = 0;
    if (!WriteProcessMemory(process, patch.target, jump, sizeof(jump), &written) ||
        written != sizeof(jump)) {
        std::printf("WriteProcessMemory(target) failed: %lu\n", GetLastError());
        return false;
    }

    FlushInstructionCache(process, patch.target, sizeof(jump));

    DWORD ignored = 0;
    VirtualProtectEx(process, patch.target, sizeof(jump), oldProtect, &ignored);
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
    if (!patchRemoteNtWriteFile(pi.hProcess, patch)) {
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
