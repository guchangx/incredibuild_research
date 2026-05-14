#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

// Wraps one command-line argument in quotes.
// 用引号包裹一个命令行参数。
static std::wstring quote(const std::wstring& value) {
    return L"\"" + value + L"\"";
}

// Returns the directory containing the current executable.
// 返回当前可执行文件所在目录。
static std::wstring exeDirectory() {
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring dir(path);
    dir.resize(dir.find_last_of(L"\\/"));
    return dir;
}

// Reads an entire file into memory.
// 将整个文件读取到内存。
static bool readFileBytes(const std::wstring& path, std::vector<unsigned char>& bytes) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }

    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size) || size.QuadPart <= 0 ||
        size.QuadPart > 64 * 1024 * 1024) {
        CloseHandle(file);
        return false;
    }

    bytes.resize(static_cast<std::size_t>(size.QuadPart));
    DWORD read = 0;
    BOOL ok = ReadFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr);
    CloseHandle(file);
    return ok && read == bytes.size();
}

// Returns a file size and reports whether the file exists.
// 返回文件大小并报告文件是否存在。
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

// Applies x64 base relocations to a locally mapped PE image.
// 对本地映射的 PE 映像应用 x64 基址重定位。
static bool applyRelocations(std::vector<unsigned char>& image,
                             IMAGE_NT_HEADERS64* nt,
                             std::uintptr_t remoteBase) {
    const auto preferred = static_cast<std::uintptr_t>(nt->OptionalHeader.ImageBase);
    const auto delta = static_cast<std::intptr_t>(remoteBase - preferred);
    if (delta == 0) {
        return true;
    }

    const auto& relocDir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC];
    if (!relocDir.VirtualAddress || !relocDir.Size) {
        std::printf("payload has no relocation table and could not be mapped at preferred base\n");
        return false;
    }

    auto offset = relocDir.VirtualAddress;
    const auto end = relocDir.VirtualAddress + relocDir.Size;
    while (offset < end) {
        auto* block = reinterpret_cast<IMAGE_BASE_RELOCATION*>(image.data() + offset);
        if (!block->SizeOfBlock) {
            break;
        }

        const auto count = (block->SizeOfBlock - sizeof(IMAGE_BASE_RELOCATION)) / sizeof(WORD);
        auto* entries = reinterpret_cast<WORD*>(block + 1);
        for (std::size_t i = 0; i < count; ++i) {
            const WORD type = entries[i] >> 12;
            const WORD rva = entries[i] & 0x0FFF;
            if (type == IMAGE_REL_BASED_DIR64) {
                auto* slot = reinterpret_cast<std::uint64_t*>(
                    image.data() + block->VirtualAddress + rva);
                *slot = static_cast<std::uint64_t>(*slot + delta);
            } else if (type != IMAGE_REL_BASED_ABSOLUTE) {
                std::printf("unsupported relocation type: %u\n", type);
                return false;
            }
        }
        offset += block->SizeOfBlock;
    }

    return true;
}

// Resolves the payload imports using addresses valid on this same-architecture host.
// 使用同架构主机上的有效地址解析载荷导入表。
static bool resolveImports(std::vector<unsigned char>& image, IMAGE_NT_HEADERS64* nt) {
    const auto& importDir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!importDir.VirtualAddress) {
        return true;
    }

    auto* desc = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(image.data() + importDir.VirtualAddress);
    for (; desc->Name; ++desc) {
        const char* dllName = reinterpret_cast<const char*>(image.data() + desc->Name);
        HMODULE module = LoadLibraryA(dllName);
        if (!module) {
            std::printf("LoadLibraryA(%s) failed: %lu\n", dllName, GetLastError());
            return false;
        }

        auto* originalThunk = desc->OriginalFirstThunk
                                  ? reinterpret_cast<IMAGE_THUNK_DATA64*>(image.data() + desc->OriginalFirstThunk)
                                  : reinterpret_cast<IMAGE_THUNK_DATA64*>(image.data() + desc->FirstThunk);
        auto* firstThunk = reinterpret_cast<IMAGE_THUNK_DATA64*>(image.data() + desc->FirstThunk);
        for (; originalThunk->u1.AddressOfData; ++originalThunk, ++firstThunk) {
            FARPROC proc = nullptr;
            if (IMAGE_SNAP_BY_ORDINAL64(originalThunk->u1.Ordinal)) {
                proc = GetProcAddress(module, reinterpret_cast<LPCSTR>(
                                                 originalThunk->u1.Ordinal & 0xFFFF));
            } else {
                auto* byName = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(
                    image.data() + originalThunk->u1.AddressOfData);
                proc = GetProcAddress(module, reinterpret_cast<LPCSTR>(byName->Name));
            }
            if (!proc) {
                std::printf("GetProcAddress failed in %s\n", dllName);
                return false;
            }
            firstThunk->u1.Function = reinterpret_cast<ULONGLONG>(proc);
        }
    }

    return true;
}

// Manually maps a PE DLL file into the child process and calls its entry point.
// 将 PE DLL 文件手动映射到子进程并调用其入口点。
static bool manualMapPayload(HANDLE process, const std::wstring& payloadPath) {
    std::vector<unsigned char> fileBytes;
    if (!readFileBytes(payloadPath, fileBytes)) {
        std::printf("failed to read payload.bin: %lu\n", GetLastError());
        return false;
    }

    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(fileBytes.data());
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
        return false;
    }
    auto* sourceNt = reinterpret_cast<IMAGE_NT_HEADERS64*>(fileBytes.data() + dos->e_lfanew);
    if (sourceNt->Signature != IMAGE_NT_SIGNATURE ||
        sourceNt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
        return false;
    }

    void* preferredBase = reinterpret_cast<void*>(sourceNt->OptionalHeader.ImageBase);
    void* remoteBase = VirtualAllocEx(
        process, preferredBase, sourceNt->OptionalHeader.SizeOfImage,
        MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!remoteBase) {
        remoteBase = VirtualAllocEx(
            process, nullptr, sourceNt->OptionalHeader.SizeOfImage,
            MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    }
    if (!remoteBase) {
        std::printf("VirtualAllocEx(image) failed: %lu\n", GetLastError());
        return false;
    }
    std::printf("payload preferred base: %p, remote base: %p\n", preferredBase, remoteBase);

    std::vector<unsigned char> image(sourceNt->OptionalHeader.SizeOfImage);
    std::memcpy(image.data(), fileBytes.data(), sourceNt->OptionalHeader.SizeOfHeaders);

    auto* section = IMAGE_FIRST_SECTION(sourceNt);
    for (WORD i = 0; i < sourceNt->FileHeader.NumberOfSections; ++i, ++section) {
        if (section->SizeOfRawData == 0) {
            continue;
        }
        std::memcpy(image.data() + section->VirtualAddress,
                    fileBytes.data() + section->PointerToRawData,
                    section->SizeOfRawData);
    }

    auto* mappedDos = reinterpret_cast<IMAGE_DOS_HEADER*>(image.data());
    auto* mappedNt = reinterpret_cast<IMAGE_NT_HEADERS64*>(image.data() + mappedDos->e_lfanew);
    if (!applyRelocations(image, mappedNt, reinterpret_cast<std::uintptr_t>(remoteBase)) ||
        !resolveImports(image, mappedNt)) {
        std::printf("failed to prepare payload image\n");
        VirtualFreeEx(process, remoteBase, 0, MEM_RELEASE);
        return false;
    }

    SIZE_T written = 0;
    if (!WriteProcessMemory(process, remoteBase, image.data(), image.size(), &written) ||
        written != image.size()) {
        std::printf("WriteProcessMemory(image) failed: %lu\n", GetLastError());
        VirtualFreeEx(process, remoteBase, 0, MEM_RELEASE);
        return false;
    }

    const auto entry = reinterpret_cast<std::uint64_t>(remoteBase) +
                       mappedNt->OptionalHeader.AddressOfEntryPoint;
    unsigned char callEntryStub[] = {
        0x48, 0x83, 0xEC, 0x28,
        0x48, 0xB9, 0, 0, 0, 0, 0, 0, 0, 0,
        0xBA, 0x01, 0x00, 0x00, 0x00,
        0x45, 0x33, 0xC0,
        0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0,
        0xFF, 0xD0,
        0x48, 0x83, 0xC4, 0x28,
        0xC3
    };
    const auto baseValue = reinterpret_cast<std::uint64_t>(remoteBase);
    std::memcpy(&callEntryStub[6], &baseValue, sizeof(baseValue));
    std::memcpy(&callEntryStub[24], &entry, sizeof(entry));

    void* remoteStub = VirtualAllocEx(process, nullptr, sizeof(callEntryStub),
                                      MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!remoteStub ||
        !WriteProcessMemory(process, remoteStub, callEntryStub, sizeof(callEntryStub), &written) ||
        written != sizeof(callEntryStub)) {
        std::printf("failed to write entry stub: %lu\n", GetLastError());
        return false;
    }

    HANDLE thread = CreateRemoteThread(process, nullptr, 0,
                                       reinterpret_cast<LPTHREAD_START_ROUTINE>(remoteStub),
                                       nullptr, 0, nullptr);
    if (!thread) {
        std::printf("CreateRemoteThread(entry) failed: %lu\n", GetLastError());
        return false;
    }

    WaitForSingleObject(thread, 10000);
    DWORD exitCode = 0;
    GetExitCodeThread(thread, &exitCode);
    CloseHandle(thread);

    std::printf("manual mapped payload.bin at %p, DllMain returned %lu\n",
                remoteBase, exitCode);
    return exitCode != 0;
}

// Starts the child, maps payload.bin into it, and verifies redirected WriteFile behavior.
// 启动子进程，映射 payload.bin，并验证 WriteFile 被重定向的行为。
int wmain() {
    const std::wstring dir = exeDirectory();
    const std::wstring childExe = dir + L"\\child.exe";
    const std::wstring payloadBin = dir + L"\\payload.bin";
    const std::wstring childOutput = dir + L"\\child_output.txt";
    const std::wstring redirectedOutput = dir + L"\\redirected_output.txt";

    DeleteFileW(childOutput.c_str());
    DeleteFileW(redirectedOutput.c_str());

    SetEnvironmentVariableW(L"MMB_REDIRECT_PATH", redirectedOutput.c_str());

    std::wstring commandLine = quote(childExe) + L" " + quote(childOutput);
    std::vector<wchar_t> mutableCommand(commandLine.begin(), commandLine.end());
    mutableCommand.push_back(L'\0');

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(childExe.c_str(), mutableCommand.data(), nullptr, nullptr, FALSE,
                        CREATE_SUSPENDED, nullptr, dir.c_str(), &si, &pi)) {
        std::printf("CreateProcessW failed: %lu\n", GetLastError());
        SetEnvironmentVariableW(L"MMB_REDIRECT_PATH", nullptr);
        return 10;
    }

    SetEnvironmentVariableW(L"MMB_REDIRECT_PATH", nullptr);

    if (!manualMapPayload(pi.hProcess, payloadBin)) {
        TerminateProcess(pi.hProcess, 100);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        return 11;
    }

    ResumeThread(pi.hThread);
    WaitForSingleObject(pi.hProcess, 10000);

    DWORD childExit = 0;
    GetExitCodeProcess(pi.hProcess, &childExit);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);

    bool childExists = false;
    bool redirectedExists = false;
    const auto childSize = fileSizeOrMissing(childOutput, childExists);
    const auto redirectedSize = fileSizeOrMissing(redirectedOutput, redirectedExists);

    std::printf("child exit code: %lu\n", childExit);
    std::printf("child output exists: %s, size: %llu\n",
                childExists ? "yes" : "no", static_cast<unsigned long long>(childSize));
    std::printf("redirected output exists: %s, size: %llu\n",
                redirectedExists ? "yes" : "no",
                static_cast<unsigned long long>(redirectedSize));

    if (childExit != 0 || !childExists || childSize != 0 ||
        !redirectedExists || redirectedSize == 0) {
        return 20;
    }

    std::printf("PASS: child WriteFile was handled by the manually mapped payload.bin.\n");
    return 0;
}
