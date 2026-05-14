#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "protocol.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>

namespace {

using CreateFileWFn = HANDLE(WINAPI*)(LPCWSTR, DWORD, DWORD,
                                      LPSECURITY_ATTRIBUTES, DWORD, DWORD,
                                      HANDLE);
using WriteFileFn = BOOL(WINAPI*)(HANDLE, LPCVOID, DWORD, LPDWORD,
                                  LPOVERLAPPED);
using CloseHandleFn = BOOL(WINAPI*)(HANDLE);

CreateFileWFn g_real_create_file_w = nullptr;
WriteFileFn g_real_write_file = nullptr;
CloseHandleFn g_real_close_handle = nullptr;
CRITICAL_SECTION g_lock;
std::wstring g_target_path;
std::unordered_map<HANDLE, HANDLE> g_redirected_handles;
std::uintptr_t g_next_fake_handle = 0x70000000;

// Resolves a path to an absolute path without throwing on failure.
// 将路径解析为绝对路径，失败时不抛出异常。
std::wstring absolute_path_no_throw(std::wstring_view path) {
    if (path.empty()) {
        return {};
    }

    const DWORD required =
        GetFullPathNameW(std::wstring(path).c_str(), 0, nullptr, nullptr);
    if (required == 0) {
        return std::wstring(path);
    }

    std::wstring result(required, L'\0');
    const DWORD written =
        GetFullPathNameW(std::wstring(path).c_str(), required, result.data(),
                         nullptr);
    if (written == 0 || written >= required) {
        return std::wstring(path);
    }
    result.resize(written);
    return result;
}

// Compares two paths case-insensitively using ordinal string comparison.
// 使用序数字符串比较以大小写不敏感方式比较两个路径。
bool same_path(const std::wstring& left, const std::wstring& right) {
    return CompareStringOrdinal(left.c_str(), -1, right.c_str(), -1, TRUE) ==
           CSTR_EQUAL;
}

// Converts UTF-16 text to UTF-8 without throwing on failure.
// 将 UTF-16 文本转换为 UTF-8，失败时不抛出异常。
std::string narrow_no_throw(std::wstring_view text) {
    if (text.empty()) {
        return {};
    }
    const int required = WideCharToMultiByte(
        CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0,
        nullptr, nullptr);
    if (required <= 0) {
        return {};
    }
    std::string result(static_cast<std::size_t>(required), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                        result.data(), required, nullptr, nullptr);
    return result;
}

// Writes an entire buffer through the original WriteFile function.
// 通过原始 WriteFile 函数写入完整缓冲区。
bool real_write_all(HANDLE handle, const void* data, DWORD bytes) {
    const auto* cursor = static_cast<const std::uint8_t*>(data);
    DWORD remaining = bytes;
    while (remaining > 0) {
        DWORD written = 0;
        if (!g_real_write_file(handle, cursor, remaining, &written, nullptr)) {
            return false;
        }
        if (written == 0) {
            SetLastError(ERROR_WRITE_FAULT);
            return false;
        }
        cursor += written;
        remaining -= written;
    }
    return true;
}

// Reads an exact byte count through the normal ReadFile API.
// 通过常规 ReadFile API 读取精确的字节数。
bool real_read_all(HANDLE handle, void* data, DWORD bytes) {
    auto* cursor = static_cast<std::uint8_t*>(data);
    DWORD remaining = bytes;
    while (remaining > 0) {
        DWORD read = 0;
        if (!ReadFile(handle, cursor, remaining, &read, nullptr)) {
            return false;
        }
        if (read == 0) {
            SetLastError(ERROR_READ_FAULT);
            return false;
        }
        cursor += read;
        remaining -= read;
    }
    return true;
}

// Sends one redirect protocol message over the named pipe and reads its response.
// 通过命名管道发送一条重定向协议消息并读取响应。
bool send_message(HANDLE pipe, PipeCommand command, const std::string& path,
                  const void* payload, DWORD payload_bytes,
                  PipeResponse* response) {
    PipeMessageHeader header{kProtocolMagic,
                             static_cast<std::uint32_t>(command),
                             static_cast<std::uint32_t>(path.size()),
                             payload_bytes};

    if (!real_write_all(pipe, &header, sizeof(header))) {
        return false;
    }
    if (!path.empty() &&
        !real_write_all(pipe, path.data(), static_cast<DWORD>(path.size()))) {
        return false;
    }
    if (payload_bytes > 0 && payload != nullptr &&
        !real_write_all(pipe, payload, payload_bytes)) {
        return false;
    }

    PipeResponse local_response{};
    if (!real_read_all(pipe, &local_response, sizeof(local_response))) {
        return false;
    }
    if (local_response.magic != kProtocolMagic) {
        SetLastError(ERROR_INVALID_DATA);
        return false;
    }
    if (response != nullptr) {
        *response = local_response;
    }
    if (!local_response.ok) {
        SetLastError(local_response.win32Error);
        return false;
    }
    return true;
}

// Connects to the process_b named-pipe server with short retries.
// 通过短暂重试连接到 process_b 命名管道服务器。
HANDLE connect_to_process_b() {
    for (int attempt = 0; attempt < 50; ++attempt) {
        HANDLE pipe = g_real_create_file_w(kPipeName, GENERIC_READ | GENERIC_WRITE,
                                           0, nullptr, OPEN_EXISTING,
                                           FILE_ATTRIBUTE_NORMAL, nullptr);
        if (pipe != INVALID_HANDLE_VALUE) {
            return pipe;
        }

        const DWORD error = GetLastError();
        if (error != ERROR_PIPE_BUSY && error != ERROR_FILE_NOT_FOUND) {
            return INVALID_HANDLE_VALUE;
        }
        Sleep(100);
    }
    SetLastError(ERROR_FILE_NOT_FOUND);
    return INVALID_HANDLE_VALUE;
}

// Creates a fake file handle and associates it with a pipe connection.
// 创建伪文件句柄，并将其关联到管道连接。
HANDLE make_fake_handle(HANDLE pipe) {
    EnterCriticalSection(&g_lock);
    const auto raw = g_next_fake_handle += 4;
    const HANDLE fake = reinterpret_cast<HANDLE>(raw);
    g_redirected_handles.emplace(fake, pipe);
    LeaveCriticalSection(&g_lock);
    return fake;
}

// Removes and returns the pipe associated with a fake file handle.
// 移除并返回与伪文件句柄关联的管道。
bool take_pipe(HANDLE fake, HANDLE* pipe) {
    EnterCriticalSection(&g_lock);
    const auto it = g_redirected_handles.find(fake);
    if (it == g_redirected_handles.end()) {
        LeaveCriticalSection(&g_lock);
        return false;
    }
    *pipe = it->second;
    g_redirected_handles.erase(it);
    LeaveCriticalSection(&g_lock);
    return true;
}

// Looks up the pipe associated with a fake file handle.
// 查找与伪文件句柄关联的管道。
bool get_pipe(HANDLE fake, HANDLE* pipe) {
    EnterCriticalSection(&g_lock);
    const auto it = g_redirected_handles.find(fake);
    if (it == g_redirected_handles.end()) {
        LeaveCriticalSection(&g_lock);
        return false;
    }
    *pipe = it->second;
    LeaveCriticalSection(&g_lock);
    return true;
}

// Intercepts target file opens and replaces them with redirected fake handles.
// 拦截目标文件打开操作，并用重定向伪句柄替代。
extern "C" HANDLE WINAPI HookedCreateFileW(
    LPCWSTR file_name, DWORD desired_access, DWORD share_mode,
    LPSECURITY_ATTRIBUTES security_attributes, DWORD creation_disposition,
    DWORD flags_and_attributes, HANDLE template_file) {
    if (file_name != nullptr && !g_target_path.empty() &&
        (desired_access & (GENERIC_WRITE | FILE_WRITE_DATA | FILE_APPEND_DATA))) {
        const std::wstring requested = absolute_path_no_throw(file_name);
        if (same_path(requested, g_target_path)) {
            HANDLE pipe = connect_to_process_b();
            if (pipe == INVALID_HANDLE_VALUE) {
                return INVALID_HANDLE_VALUE;
            }

            const std::string path_utf8 = narrow_no_throw(requested);
            PipeResponse response{};
            if (!send_message(pipe, PipeCommand::Open, path_utf8, nullptr, 0,
                              &response)) {
                const DWORD error = GetLastError();
                g_real_close_handle(pipe);
                SetLastError(error);
                return INVALID_HANDLE_VALUE;
            }
            return make_fake_handle(pipe);
        }
    }

    return g_real_create_file_w(file_name, desired_access, share_mode,
                                security_attributes, creation_disposition,
                                flags_and_attributes, template_file);
}

// Intercepts writes to redirected fake handles and sends payloads to process_b.
// 拦截对重定向伪句柄的写入，并将载荷发送给 process_b。
extern "C" BOOL WINAPI HookedWriteFile(HANDLE file, LPCVOID buffer,
                                       DWORD bytes_to_write,
                                       LPDWORD bytes_written,
                                       LPOVERLAPPED overlapped) {
    HANDLE pipe = INVALID_HANDLE_VALUE;
    if (get_pipe(file, &pipe)) {
        if (overlapped != nullptr) {
            SetLastError(ERROR_NOT_SUPPORTED);
            return FALSE;
        }
        PipeResponse response{};
        if (!send_message(pipe, PipeCommand::Write, {}, buffer, bytes_to_write,
                          &response)) {
            return FALSE;
        }
        if (bytes_written != nullptr) {
            *bytes_written = bytes_to_write;
        }
        return TRUE;
    }

    return g_real_write_file(file, buffer, bytes_to_write, bytes_written,
                             overlapped);
}

// Intercepts closes of redirected fake handles and closes the pipe protocol stream.
// 拦截重定向伪句柄的关闭操作，并关闭管道协议流。
extern "C" BOOL WINAPI HookedCloseHandle(HANDLE object) {
    HANDLE pipe = INVALID_HANDLE_VALUE;
    if (take_pipe(object, &pipe)) {
        PipeResponse response{};
        const BOOL ok =
            send_message(pipe, PipeCommand::Close, {}, nullptr, 0, &response)
                ? TRUE
                : FALSE;
        const DWORD error = GetLastError();
        g_real_close_handle(pipe);
        if (!ok) {
            SetLastError(error);
        }
        return ok;
    }

    return g_real_close_handle(object);
}

// Patches one import address table entry in the current module.
// 修补当前模块中的一个导入地址表项。
bool patch_import(const char* import_name, void* replacement,
                  void** original_out) {
    HMODULE module = GetModuleHandleW(nullptr);
    if (module == nullptr) {
        return false;
    }

    auto* base = reinterpret_cast<std::uint8_t*>(module);
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
        return false;
    }
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) {
        return false;
    }

    const IMAGE_DATA_DIRECTORY& imports =
        nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (imports.VirtualAddress == 0) {
        return false;
    }

    auto* descriptor =
        reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + imports.VirtualAddress);
    for (; descriptor->Name != 0; ++descriptor) {
        auto* original_thunk = reinterpret_cast<IMAGE_THUNK_DATA*>(
            base + descriptor->OriginalFirstThunk);
        auto* first_thunk =
            reinterpret_cast<IMAGE_THUNK_DATA*>(base + descriptor->FirstThunk);

        if (descriptor->OriginalFirstThunk == 0) {
            original_thunk = first_thunk;
        }

        for (; original_thunk->u1.AddressOfData != 0;
             ++original_thunk, ++first_thunk) {
            if (IMAGE_SNAP_BY_ORDINAL(original_thunk->u1.Ordinal)) {
                continue;
            }

            auto* import_by_name = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(
                base + original_thunk->u1.AddressOfData);
            if (lstrcmpiA(reinterpret_cast<char*>(import_by_name->Name),
                          import_name) != 0) {
                continue;
            }

            DWORD old_protect = 0;
            if (!VirtualProtect(&first_thunk->u1.Function, sizeof(void*),
                                PAGE_READWRITE, &old_protect)) {
                return false;
            }
            if (original_out != nullptr && *original_out == nullptr) {
                *original_out =
                    reinterpret_cast<void*>(first_thunk->u1.Function);
            }
            first_thunk->u1.Function =
                reinterpret_cast<ULONG_PTR>(replacement);
            DWORD unused = 0;
            VirtualProtect(&first_thunk->u1.Function, sizeof(void*),
                           old_protect, &unused);
            FlushInstructionCache(GetCurrentProcess(), &first_thunk->u1.Function,
                                  sizeof(void*));
            return true;
        }
    }

    return false;
}

// Initializes hook state and patches imports when a redirect target is configured.
// 初始化钩子状态，并在配置重定向目标时修补导入表。
void initialize_hook() {
    InitializeCriticalSection(&g_lock);

    HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
    g_real_create_file_w = reinterpret_cast<CreateFileWFn>(
        GetProcAddress(kernel32, "CreateFileW"));
    g_real_write_file =
        reinterpret_cast<WriteFileFn>(GetProcAddress(kernel32, "WriteFile"));
    g_real_close_handle = reinterpret_cast<CloseHandleFn>(
        GetProcAddress(kernel32, "CloseHandle"));

    wchar_t target[MAX_PATH * 4]{};
    const DWORD length =
        GetEnvironmentVariableW(kRedirectTargetEnv, target,
                                static_cast<DWORD>(std::size(target)));
    if (length > 0 && length < std::size(target)) {
        g_target_path = absolute_path_no_throw(target);
    }

    if (!g_target_path.empty()) {
        patch_import("CreateFileW", reinterpret_cast<void*>(HookedCreateFileW),
                     reinterpret_cast<void**>(&g_real_create_file_w));
        patch_import("WriteFile", reinterpret_cast<void*>(HookedWriteFile),
                     reinterpret_cast<void**>(&g_real_write_file));
        patch_import("CloseHandle", reinterpret_cast<void*>(HookedCloseHandle),
                     reinterpret_cast<void**>(&g_real_close_handle));
    }
}

} // namespace

// Initializes and tears down the redirect hook DLL for process attach and detach.
// 在进程附加和分离时初始化并清理重定向钩子 DLL。
BOOL WINAPI DllMain(HINSTANCE, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        initialize_hook();
    } else if (reason == DLL_PROCESS_DETACH) {
        DeleteCriticalSection(&g_lock);
    }
    return TRUE;
}
