#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

// Wraps a command-line argument in double quotes.
// 用双引号包裹命令行参数。
static std::wstring quote(const std::wstring& value) {
    return L"\"" + value + L"\"";
}

struct SharedBlock {
    volatile std::uint32_t marker;
    volatile std::uint32_t value;
};

// Prints the number of normally loaded modules in the child process.
// 打印子进程中正常加载的模块数量。
static bool listChildModules(HANDLE process) {
    HMODULE modules[1024]{};
    DWORD needed = 0;
    using EnumProcessModulesType = BOOL(WINAPI*)(HANDLE, HMODULE*, DWORD, LPDWORD);
    HMODULE psapi = LoadLibraryW(L"psapi.dll");
    if (!psapi) {
        return false;
    }
    auto enumProcessModules = reinterpret_cast<EnumProcessModulesType>(
        GetProcAddress(psapi, "EnumProcessModules"));
    if (!enumProcessModules || !enumProcessModules(process, modules, sizeof(modules), &needed)) {
        FreeLibrary(psapi);
        return false;
    }

    std::printf("child normal module count: %lu\n", needed / static_cast<DWORD>(sizeof(HMODULE)));
    FreeLibrary(psapi);
    return true;
}

// Starts a suspended child, injects an anonymous code blob, and verifies its result.
// 启动挂起的子进程，注入匿名代码块，并验证执行结果。
int wmain() {
    wchar_t modulePath[MAX_PATH]{};
    GetModuleFileNameW(nullptr, modulePath, MAX_PATH);
    std::wstring exeDir(modulePath);
    exeDir.resize(exeDir.find_last_of(L"\\/"));

    std::wstring childExe = exeDir + L"\\child_wait.exe";
    std::wstring commandLine = quote(childExe);
    std::vector<wchar_t> mutableCommand(commandLine.begin(), commandLine.end());
    mutableCommand.push_back(L'\0');

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(childExe.c_str(), mutableCommand.data(), nullptr, nullptr, FALSE,
                        CREATE_SUSPENDED, nullptr, exeDir.c_str(), &si, &pi)) {
        std::printf("CreateProcessW failed: %lu\n", GetLastError());
        return 10;
    }

    auto* shared = static_cast<SharedBlock*>(VirtualAllocEx(
        pi.hProcess, nullptr, sizeof(SharedBlock), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (!shared) {
        std::printf("VirtualAllocEx(shared) failed: %lu\n", GetLastError());
        TerminateProcess(pi.hProcess, 100);
        return 11;
    }

    SharedBlock zero{};
    SIZE_T written = 0;
    WriteProcessMemory(pi.hProcess, shared, &zero, sizeof(zero), &written);

    // x64 blob:
    //   mov rax, <shared block address>
    //   mov dword ptr [rax], 0xC0DEF00D
    //   mov dword ptr [rax+4], 0x12345678
    //   xor ecx, ecx
    //   mov rax, <ExitThread>
    //   call rax
    unsigned char blob[] = {
        0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0,
        0xC7, 0x00, 0x0D, 0xF0, 0xDE, 0xC0,
        0xC7, 0x40, 0x04, 0x78, 0x56, 0x34, 0x12,
        0x31, 0xC9,
        0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0,
        0xFF, 0xD0
    };

    std::uint64_t sharedAddress = reinterpret_cast<std::uint64_t>(shared);
    std::memcpy(&blob[2], &sharedAddress, sizeof(sharedAddress));

    auto exitThread = reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "ExitThread"));
    std::uint64_t exitThreadAddress = reinterpret_cast<std::uint64_t>(exitThread);
    std::memcpy(&blob[27], &exitThreadAddress, sizeof(exitThreadAddress));

    void* remoteBlob = VirtualAllocEx(pi.hProcess, nullptr, sizeof(blob),
                                      MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!remoteBlob) {
        std::printf("VirtualAllocEx(blob) failed: %lu\n", GetLastError());
        TerminateProcess(pi.hProcess, 101);
        return 12;
    }

    written = 0;
    if (!WriteProcessMemory(pi.hProcess, remoteBlob, blob, sizeof(blob), &written) ||
        written != sizeof(blob)) {
        std::printf("WriteProcessMemory(blob) failed: %lu\n", GetLastError());
        TerminateProcess(pi.hProcess, 102);
        return 13;
    }

    CONTEXT context{};
    context.ContextFlags = CONTEXT_CONTROL;
    if (!GetThreadContext(pi.hThread, &context)) {
        std::printf("GetThreadContext failed: %lu\n", GetLastError());
        TerminateProcess(pi.hProcess, 103);
        return 14;
    }

#if defined(_M_X64)
    context.Rip = reinterpret_cast<DWORD64>(remoteBlob);
#else
#error This demo is x64-only.
#endif

    if (!SetThreadContext(pi.hThread, &context)) {
        std::printf("SetThreadContext failed: %lu\n", GetLastError());
        TerminateProcess(pi.hProcess, 104);
        return 15;
    }

    listChildModules(pi.hProcess);

    ResumeThread(pi.hThread);
    WaitForSingleObject(pi.hProcess, 10000);

    SharedBlock observed{};
    SIZE_T read = 0;
    ReadProcessMemory(pi.hProcess, shared, &observed, sizeof(observed), &read);

    DWORD exitCode = 0;
    GetExitCodeProcess(pi.hProcess, &exitCode);

    std::printf("remote blob address: %p\n", remoteBlob);
    std::printf("shared marker: 0x%08X\n", observed.marker);
    std::printf("shared value: 0x%08X\n", observed.value);
    std::printf("child exit code: %lu\n", exitCode);

    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);

    if (observed.marker != 0xC0DEF00D || observed.value != 0x12345678 || exitCode != 0) {
        return 20;
    }

    std::printf("PASS: anonymous binary blob executed inside the child without LoadLibrary.\n");
    return 0;
}
