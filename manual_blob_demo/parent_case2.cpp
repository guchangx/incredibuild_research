#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <psapi.h>
#include <winternl.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static std::wstring quote(const std::wstring& value) {
    return L"\"" + value + L"\"";
}

static std::wstring exeDirectory() {
    wchar_t modulePath[MAX_PATH]{};
    GetModuleFileNameW(nullptr, modulePath, MAX_PATH);
    std::wstring dir(modulePath);
    dir.resize(dir.find_last_of(L"\\/"));
    return dir;
}

static bool writeTextFile(const std::wstring& path, const char* text) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }
    DWORD written = 0;
    BOOL ok = WriteFile(file, text, static_cast<DWORD>(std::strlen(text)), &written, nullptr);
    CloseHandle(file);
    return ok && written == std::strlen(text);
}

static std::string readSmallFile(const std::wstring& path, bool& exists) {
    exists = false;
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return {};
    }

    exists = true;
    char buffer[1024]{};
    DWORD read = 0;
    ReadFile(file, buffer, static_cast<DWORD>(sizeof(buffer)), &read, nullptr);
    CloseHandle(file);
    return std::string(buffer, buffer + read);
}

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

static bool readRemoteString(HANDLE process, std::uintptr_t remoteAddress, std::string& out) {
    out.clear();
    for (std::size_t i = 0; i < 260; ++i) {
        char ch = 0;
        SIZE_T read = 0;
        if (!ReadProcessMemory(process, reinterpret_cast<void*>(remoteAddress + i), &ch, 1, &read) ||
            read != 1) {
            return false;
        }
        if (ch == '\0') {
            return true;
        }
        out.push_back(ch);
    }
    return false;
}

static bool getMainModuleBase(HANDLE process, std::uintptr_t& base) {
    HMODULE modules[16]{};
    DWORD needed = 0;
    if (EnumProcessModules(process, modules, sizeof(modules), &needed) && needed >= sizeof(HMODULE)) {
        base = reinterpret_cast<std::uintptr_t>(modules[0]);
        return true;
    }

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

    // In a native x64 PEB, ImageBaseAddress is at offset 0x10. This demo is
    // intentionally x64-only and is built with vcvars64.
    SIZE_T read = 0;
    auto imageBaseSlot = reinterpret_cast<const char*>(pbi.PebBaseAddress) + 0x10;
    return ReadProcessMemory(process, imageBaseSlot, &base, sizeof(base), &read) &&
           read == sizeof(base) && base != 0;
}

static bool findRemoteIatEntry(HANDLE process, const char* importName, std::uintptr_t& iatEntry) {
    std::uintptr_t base = 0;
    if (!getMainModuleBase(process, base)) {
        std::printf("failed to find child main module base: %lu\n", GetLastError());
        return false;
    }

    IMAGE_DOS_HEADER dos{};
    SIZE_T read = 0;
    if (!ReadProcessMemory(process, reinterpret_cast<void*>(base), &dos, sizeof(dos), &read) ||
        dos.e_magic != IMAGE_DOS_SIGNATURE) {
        return false;
    }

    IMAGE_NT_HEADERS64 nt{};
    if (!ReadProcessMemory(process, reinterpret_cast<void*>(base + dos.e_lfanew), &nt, sizeof(nt), &read) ||
        nt.Signature != IMAGE_NT_SIGNATURE) {
        return false;
    }

    const auto& imports = nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (imports.VirtualAddress == 0) {
        return false;
    }

    for (std::uint32_t descRva = imports.VirtualAddress;; descRva += sizeof(IMAGE_IMPORT_DESCRIPTOR)) {
        IMAGE_IMPORT_DESCRIPTOR desc{};
        if (!ReadProcessMemory(process, reinterpret_cast<void*>(base + descRva), &desc, sizeof(desc), &read)) {
            return false;
        }
        if (desc.Name == 0) {
            break;
        }

        std::uintptr_t originalThunk = desc.OriginalFirstThunk ? desc.OriginalFirstThunk : desc.FirstThunk;
        for (std::uint32_t index = 0;; ++index) {
            IMAGE_THUNK_DATA64 thunk{};
            auto thunkAddress = base + originalThunk + index * sizeof(thunk);
            if (!ReadProcessMemory(process, reinterpret_cast<void*>(thunkAddress), &thunk, sizeof(thunk), &read)) {
                return false;
            }
            if (thunk.u1.AddressOfData == 0) {
                break;
            }
            if (IMAGE_SNAP_BY_ORDINAL64(thunk.u1.Ordinal)) {
                continue;
            }

            std::string name;
            if (!readRemoteString(process, base + thunk.u1.AddressOfData + sizeof(WORD), name)) {
                return false;
            }
            if (name == importName) {
                iatEntry = base + desc.FirstThunk + index * sizeof(std::uintptr_t);
                return true;
            }
        }
    }

    return false;
}

static bool patchWriteFileIat(HANDLE process, void*& remoteStub) {
    // The stub is "int3; ret". The parent debug loop handles the breakpoint,
    // copies the child's WriteFile buffer, sets the return value, and returns
    // directly to the child's caller.
    unsigned char stub[] = {0xCC, 0xC3};
    remoteStub = VirtualAllocEx(process, nullptr, sizeof(stub), MEM_COMMIT | MEM_RESERVE,
                                PAGE_EXECUTE_READWRITE);
    if (!remoteStub) {
        std::printf("VirtualAllocEx(stub) failed: %lu\n", GetLastError());
        return false;
    }

    SIZE_T written = 0;
    if (!WriteProcessMemory(process, remoteStub, stub, sizeof(stub), &written) ||
        written != sizeof(stub)) {
        std::printf("WriteProcessMemory(stub) failed: %lu\n", GetLastError());
        return false;
    }

    std::uintptr_t iatEntry = 0;
    if (!findRemoteIatEntry(process, "WriteFile", iatEntry)) {
        std::printf("WriteFile IAT entry not found\n");
        return false;
    }

    DWORD oldProtect = 0;
    if (!VirtualProtectEx(process, reinterpret_cast<void*>(iatEntry), sizeof(remoteStub),
                          PAGE_READWRITE, &oldProtect)) {
        std::printf("VirtualProtectEx(IAT) failed: %lu\n", GetLastError());
        return false;
    }

    if (!WriteProcessMemory(process, reinterpret_cast<void*>(iatEntry), &remoteStub,
                            sizeof(remoteStub), &written) ||
        written != sizeof(remoteStub)) {
        std::printf("WriteProcessMemory(IAT) failed: %lu\n", GetLastError());
        return false;
    }

    DWORD ignored = 0;
    VirtualProtectEx(process, reinterpret_cast<void*>(iatEntry), sizeof(remoteStub),
                     oldProtect, &ignored);
    FlushInstructionCache(process, reinterpret_cast<void*>(iatEntry), sizeof(remoteStub));
    std::printf("patched child WriteFile IAT entry to stub %p\n", remoteStub);
    return true;
}

static bool handleIntercept(HANDLE process, DWORD threadId, HANDLE parentOutput) {
    HANDLE thread = OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_SUSPEND_RESUME,
                               FALSE, threadId);
    if (!thread) {
        return false;
    }

    CONTEXT ctx{};
    ctx.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
    if (!GetThreadContext(thread, &ctx)) {
        CloseHandle(thread);
        return false;
    }

    std::uint32_t byteCount = static_cast<std::uint32_t>(ctx.R8);
    std::uint32_t capped = std::min<std::uint32_t>(byteCount, 4096);
    std::vector<char> buffer(capped);

    SIZE_T read = 0;
    if (capped != 0 &&
        !ReadProcessMemory(process, reinterpret_cast<void*>(ctx.Rdx), buffer.data(), capped, &read)) {
        CloseHandle(thread);
        return false;
    }

    DWORD parentWritten = 0;
    BOOL parentWriteOk = WriteFile(parentOutput, buffer.data(), static_cast<DWORD>(read),
                                   &parentWritten, nullptr);

    if (ctx.R9 != 0) {
        DWORD childVisibleWritten = parentWriteOk ? static_cast<DWORD>(read) : 0;
        SIZE_T ignored = 0;
        WriteProcessMemory(process, reinterpret_cast<void*>(ctx.R9), &childVisibleWritten,
                           sizeof(childVisibleWritten), &ignored);
    }

    std::uint64_t returnAddress = 0;
    ReadProcessMemory(process, reinterpret_cast<void*>(ctx.Rsp), &returnAddress,
                      sizeof(returnAddress), &read);

    ctx.Rax = parentWriteOk ? TRUE : FALSE;
    ctx.Rip = returnAddress;
    ctx.Rsp += sizeof(std::uint64_t);
    bool ok = SetThreadContext(thread, &ctx) != FALSE;
    CloseHandle(thread);

    std::printf("parent intercepted child WriteFile: %lu bytes\n", parentWritten);
    return ok && parentWriteOk;
}

static bool runDirectChild(const std::wstring& childExe, const std::wstring& input,
                           const std::wstring& output) {
    std::wstring cmd = quote(childExe) + L" " + quote(input) + L" " + quote(output);
    std::vector<wchar_t> mutableCmd(cmd.begin(), cmd.end());
    mutableCmd.push_back(L'\0');

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(childExe.c_str(), mutableCmd.data(), nullptr, nullptr, FALSE,
                        0, nullptr, nullptr, &si, &pi)) {
        return false;
    }
    WaitForSingleObject(pi.hProcess, 10000);
    DWORD exitCode = 0;
    GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return exitCode == 0;
}

int wmain() {
    const std::wstring dir = exeDirectory();
    const std::wstring childExe = dir + L"\\child_case2.exe";
    const std::wstring inputPath = dir + L"\\case2_input.txt";
    const std::wstring directOutput = dir + L"\\case2_direct_child_output.txt";
    const std::wstring childHookedOutput = dir + L"\\case2_hooked_child_output.txt";
    const std::wstring parentOutput = dir + L"\\case2_parent_redirected_output.txt";

    DeleteFileW(inputPath.c_str());
    DeleteFileW(directOutput.c_str());
    DeleteFileW(childHookedOutput.c_str());
    DeleteFileW(parentOutput.c_str());

    const char* inputText = "case2 input read locally by the child, but written by the parent\n";
    if (!writeTextFile(inputPath, inputText)) {
        std::printf("failed to create input\n");
        return 10;
    }

    if (!runDirectChild(childExe, inputPath, directOutput)) {
        std::printf("direct child run failed\n");
        return 11;
    }

    HANDLE redirected = CreateFileW(parentOutput.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
    if (redirected == INVALID_HANDLE_VALUE) {
        return 12;
    }

    std::wstring cmd = quote(childExe) + L" " + quote(inputPath) + L" " + quote(childHookedOutput);
    std::vector<wchar_t> mutableCmd(cmd.begin(), cmd.end());
    mutableCmd.push_back(L'\0');

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    DWORD flags = CREATE_SUSPENDED | DEBUG_ONLY_THIS_PROCESS;
    if (!CreateProcessW(childExe.c_str(), mutableCmd.data(), nullptr, nullptr, FALSE,
                        flags, nullptr, dir.c_str(), &si, &pi)) {
        std::printf("CreateProcessW hooked child failed: %lu\n", GetLastError());
        CloseHandle(redirected);
        return 13;
    }

    ResumeThread(pi.hThread);

    void* remoteStub = nullptr;
    bool hookInstalled = false;
    bool sawIntercept = false;
    DWORD childExitCode = STILL_ACTIVE;
    bool running = true;
    while (running) {
        DEBUG_EVENT event{};
        if (!WaitForDebugEvent(&event, 10000)) {
            std::printf("WaitForDebugEvent timed out or failed: %lu\n", GetLastError());
            TerminateProcess(pi.hProcess, 101);
            CloseHandle(redirected);
            return 15;
        }

        DWORD continueStatus = DBG_CONTINUE;
        if (event.dwDebugEventCode == EXCEPTION_DEBUG_EVENT) {
            auto code = event.u.Exception.ExceptionRecord.ExceptionCode;
            auto address = event.u.Exception.ExceptionRecord.ExceptionAddress;
            if (code == EXCEPTION_BREAKPOINT && !hookInstalled) {
                if (!patchWriteFileIat(pi.hProcess, remoteStub)) {
                    TerminateProcess(pi.hProcess, 100);
                    CloseHandle(redirected);
                    return 14;
                }
                hookInstalled = true;
            } else if (code == EXCEPTION_BREAKPOINT &&
                (address == remoteStub ||
                 reinterpret_cast<std::uintptr_t>(address) ==
                     reinterpret_cast<std::uintptr_t>(remoteStub) + 1)) {
                if (!handleIntercept(pi.hProcess, event.dwThreadId, redirected)) {
                    continueStatus = DBG_EXCEPTION_NOT_HANDLED;
                } else {
                    sawIntercept = true;
                }
            }
        } else if (event.dwDebugEventCode == EXIT_PROCESS_DEBUG_EVENT) {
            childExitCode = event.u.ExitProcess.dwExitCode;
            running = false;
        } else if (event.dwDebugEventCode == CREATE_PROCESS_DEBUG_EVENT) {
            if (event.u.CreateProcessInfo.hFile) {
                CloseHandle(event.u.CreateProcessInfo.hFile);
            }
        } else if (event.dwDebugEventCode == LOAD_DLL_DEBUG_EVENT) {
            if (event.u.LoadDll.hFile) {
                CloseHandle(event.u.LoadDll.hFile);
            }
        }

        ContinueDebugEvent(event.dwProcessId, event.dwThreadId, continueStatus);
    }

    CloseHandle(redirected);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);

    bool directExists = false;
    bool hookedExists = false;
    bool parentExists = false;
    std::string directText = readSmallFile(directOutput, directExists);
    std::string parentText = readSmallFile(parentOutput, parentExists);
    std::uint64_t hookedSize = fileSizeOrMissing(childHookedOutput, hookedExists);

    std::printf("direct child output exists: %s, bytes: %zu\n",
                directExists ? "yes" : "no", directText.size());
    std::printf("hooked child output exists: %s, size: %llu\n",
                hookedExists ? "yes" : "no", static_cast<unsigned long long>(hookedSize));
    std::printf("parent redirected output exists: %s, bytes: %zu\n",
                parentExists ? "yes" : "no", parentText.size());
    std::printf("hooked child exit code: %lu\n", childExitCode);

    if (!sawIntercept || childExitCode != 0 || !directExists || !parentExists ||
        directText != inputText || parentText != inputText || (hookedExists && hookedSize != 0)) {
        return 20;
    }

    std::printf("PASS: child read locally; child WriteFile was intercepted and parent wrote the bytes.\n");
    return 0;
}
