#define WIN32_LEAN_AND_MEAN
#include <windows.h>

using WriteFileFn = BOOL(WINAPI*)(HANDLE, LPCVOID, DWORD, LPDWORD, LPOVERLAPPED);

// Runtime state kept by the manually mapped payload.
// 手动映射载荷维护的运行时状态。
static WriteFileFn g_realWriteFile = nullptr;
static wchar_t g_redirectPath[MAX_PATH * 4]{};
static HANDLE g_redirectFile = INVALID_HANDLE_VALUE;
static LONG g_inHook = 0;

// Compares two ASCII strings without depending on the C runtime.
// 不依赖 C 运行库比较两个 ASCII 字符串。
static bool asciiEquals(const char* left, const char* right) {
    while (*left && *right && *left == *right) {
        ++left;
        ++right;
    }
    return *left == '\0' && *right == '\0';
}

// Opens the redirected output file on first use.
// 首次使用时打开重定向输出文件。
static HANDLE redirectedFile() {
    if (g_redirectFile != INVALID_HANDLE_VALUE) {
        return g_redirectFile;
    }

    g_redirectFile = CreateFileW(g_redirectPath, GENERIC_WRITE, FILE_SHARE_READ,
                                 nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                                 nullptr);
    return g_redirectFile;
}

// Handles child WriteFile calls by writing the payload to the redirected file.
// 通过写入重定向文件来处理子进程的 WriteFile 调用。
extern "C" BOOL WINAPI HookedWriteFile(HANDLE file, LPCVOID buffer,
                                       DWORD bytesToWrite, LPDWORD bytesWritten,
                                       LPOVERLAPPED overlapped) {
    // Fallback to the original API if the payload is not fully initialized,
    // no redirect target is configured, or we re-enter from our own write.
    // 若载荷未初始化、未配置重定向目标，或发生重入，则回退到原始 API。
    if (!g_realWriteFile || g_redirectPath[0] == L'\0' ||
        InterlockedCompareExchange(&g_inHook, 1, 0) != 0) {
        return g_realWriteFile(file, buffer, bytesToWrite, bytesWritten, overlapped);
    }

    // Write user payload bytes to the redirected target file instead of the
    // original file handle passed by the child process.
    // 将子进程要写入的数据写到重定向文件，而不是原始文件句柄。
    HANDLE redirected = redirectedFile();
    DWORD redirectedWritten = 0;
    BOOL ok = FALSE;
    if (redirected != INVALID_HANDLE_VALUE && overlapped == nullptr) {
        ok = g_realWriteFile(redirected, buffer, bytesToWrite, &redirectedWritten, nullptr);
    } else {
        SetLastError(ERROR_NOT_SUPPORTED);
    }

    if (ok && redirectedWritten == bytesToWrite) {
        if (bytesWritten) {
            *bytesWritten = bytesToWrite;
        }
    }

    // Clear the guard before returning so future WriteFile calls can be handled.
    // 返回前清理重入保护，确保后续 WriteFile 仍可被处理。
    InterlockedExchange(&g_inHook, 0);
    return ok && redirectedWritten == bytesToWrite;
}

// Patches one import in the child main executable IAT.
// 修补子进程主可执行文件导入地址表中的一个导入项。
static bool patchMainImport(const char* importName, void* replacement) {
    auto* base = reinterpret_cast<unsigned char*>(GetModuleHandleW(nullptr));
    if (!base) {
        return false;
    }

    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
        return false;
    }

    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) {
        return false;
    }

    const IMAGE_DATA_DIRECTORY imports =
        nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!imports.VirtualAddress) {
        return false;
    }

    auto* desc = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + imports.VirtualAddress);
    for (; desc->Name; ++desc) {
        // Some binaries omit OriginalFirstThunk. In that case, use FirstThunk
        // for both name lookup and target patching.
        // 部分二进制没有 OriginalFirstThunk，此时用 FirstThunk 做查找和修补。
        auto* originalThunk = desc->OriginalFirstThunk
                                  ? reinterpret_cast<IMAGE_THUNK_DATA64*>(base + desc->OriginalFirstThunk)
                                  : reinterpret_cast<IMAGE_THUNK_DATA64*>(base + desc->FirstThunk);
        auto* firstThunk = reinterpret_cast<IMAGE_THUNK_DATA64*>(base + desc->FirstThunk);

        for (; originalThunk->u1.AddressOfData; ++originalThunk, ++firstThunk) {
            if (IMAGE_SNAP_BY_ORDINAL64(originalThunk->u1.Ordinal)) {
                continue;
            }

            auto* byName = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(
                base + originalThunk->u1.AddressOfData);
            if (!asciiEquals(reinterpret_cast<const char*>(byName->Name), importName)) {
                continue;
            }

            // Temporarily make the IAT slot writable, swap the function
            // pointer, and restore original page protection.
            // 临时改写 IAT 槽页权限，替换函数指针后恢复原权限。
            DWORD oldProtect = 0;
            if (!VirtualProtect(&firstThunk->u1.Function, sizeof(void*), PAGE_READWRITE,
                                &oldProtect)) {
                return false;
            }
            firstThunk->u1.Function = reinterpret_cast<ULONGLONG>(replacement);
            DWORD ignored = 0;
            VirtualProtect(&firstThunk->u1.Function, sizeof(void*), oldProtect, &ignored);
            FlushInstructionCache(GetCurrentProcess(), &firstThunk->u1.Function,
                                  sizeof(void*));
            return true;
        }
    }

    return false;
}

// Initializes the manually mapped payload and installs the WriteFile hook.
// 初始化手动映射的载荷并安装 WriteFile 钩子。
extern "C" BOOL WINAPI DllMain(HINSTANCE, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        // Resolve WriteFile from kernel32 and read redirect target from env.
        // 从 kernel32 解析 WriteFile，并从环境变量读取重定向路径。
        HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
        g_realWriteFile = reinterpret_cast<WriteFileFn>(
            GetProcAddress(kernel32, "WriteFile"));
        GetEnvironmentVariableW(L"MMB_REDIRECT_PATH", g_redirectPath,
                                static_cast<DWORD>(sizeof(g_redirectPath) / sizeof(wchar_t)));
        // Patch the main module IAT only when initialization is complete.
        // 仅在初始化完成时修补主模块 IAT。
        if (g_realWriteFile && g_redirectPath[0] != L'\0') {
            patchMainImport("WriteFile", reinterpret_cast<void*>(HookedWriteFile));
        }
    } else if (reason == DLL_PROCESS_DETACH) {
        if (g_redirectFile != INVALID_HANDLE_VALUE) {
            CloseHandle(g_redirectFile);
        }
    }
    return TRUE;
}
