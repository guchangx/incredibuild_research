#include "common.hpp"

#include <cwchar>
#include <sstream>

namespace {

std::wstring quote_arg(const std::wstring& arg) {
    std::wstring result = L"\"";
    for (wchar_t ch : arg) {
        if (ch == L'"') {
            result += L'\\';
        }
        result += ch;
    }
    result += L"\"";
    return result;
}

std::wstring current_exe_directory() {
    wchar_t buffer[MAX_PATH]{};
    const DWORD length = GetModuleFileNameW(nullptr, buffer, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) {
        throw std::runtime_error(last_error_message("GetModuleFileNameW"));
    }
    std::wstring path(buffer);
    const std::size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? L"." : path.substr(0, slash);
}

void inject_dll(HANDLE process, const std::wstring& dll_path) {
    const auto bytes = static_cast<SIZE_T>((dll_path.size() + 1) * sizeof(wchar_t));
    void* remote_memory =
        VirtualAllocEx(process, nullptr, bytes, MEM_COMMIT | MEM_RESERVE,
                       PAGE_READWRITE);
    if (remote_memory == nullptr) {
        throw std::runtime_error(last_error_message("VirtualAllocEx"));
    }

    if (!WriteProcessMemory(process, remote_memory, dll_path.c_str(), bytes,
                            nullptr)) {
        VirtualFreeEx(process, remote_memory, 0, MEM_RELEASE);
        throw std::runtime_error(last_error_message("WriteProcessMemory"));
    }

    HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
    auto* load_library = reinterpret_cast<LPTHREAD_START_ROUTINE>(
        GetProcAddress(kernel32, "LoadLibraryW"));
    if (load_library == nullptr) {
        VirtualFreeEx(process, remote_memory, 0, MEM_RELEASE);
        throw std::runtime_error(last_error_message("GetProcAddress"));
    }

    HANDLE thread = CreateRemoteThread(process, nullptr, 0, load_library,
                                       remote_memory, 0, nullptr);
    if (thread == nullptr) {
        VirtualFreeEx(process, remote_memory, 0, MEM_RELEASE);
        throw std::runtime_error(last_error_message("CreateRemoteThread"));
    }

    WaitForSingleObject(thread, INFINITE);
    DWORD remote_module = 0;
    GetExitCodeThread(thread, &remote_module);
    CloseHandle(thread);
    VirtualFreeEx(process, remote_memory, 0, MEM_RELEASE);

    if (remote_module == 0) {
        throw std::runtime_error("remote LoadLibraryW failed");
    }
}

void usage() {
    std::cout
        << "Usage:\n"
        << "  hook_launcher.exe <output-file> [process-a-exe]\n\n"
        << "  hook_launcher.exe --copy <input-file> <output-file> [process-a-exe]\n\n"
        << "The launcher starts process_a suspended, injects redirect_hook.dll,\n"
        << "then lets process_a perform a normal file write that gets redirected.\n";
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    PROCESS_INFORMATION pi{};
    try {
        if (argc < 2) {
            usage();
            return 2;
        }

        bool copy_mode = false;
        std::wstring input_path;
        std::wstring output_path;
        int process_a_arg_index = 2;

        if (std::wstring(argv[1]) == L"--copy") {
            if (argc < 4) {
                usage();
                return 2;
            }
            copy_mode = true;
            input_path = absolute_path(argv[2]);
            output_path = absolute_path(argv[3]);
            process_a_arg_index = 4;
        } else {
            output_path = absolute_path(argv[1]);
        }

        const std::wstring exe_dir = current_exe_directory();
        const std::wstring process_a =
            argc > process_a_arg_index ? absolute_path(argv[process_a_arg_index])
                                       : absolute_path(exe_dir + L"\\process_a.exe");
        const std::wstring hook_dll = absolute_path(exe_dir + L"\\redirect_hook.dll");

        if (!SetEnvironmentVariableW(kRedirectTargetEnv, output_path.c_str())) {
            throw std::runtime_error(last_error_message("SetEnvironmentVariableW"));
        }

        std::wstring command_line = quote_arg(process_a);
        if (copy_mode) {
            command_line += L" --copy " + quote_arg(input_path) + L" " +
                            quote_arg(output_path);
        } else {
            command_line += L" " + quote_arg(output_path) + L" " +
                            quote_arg(L"case2_hooked_write");
        }

        STARTUPINFOW si{};
        si.cb = sizeof(si);
        if (!CreateProcessW(process_a.c_str(), command_line.data(), nullptr,
                            nullptr, TRUE, CREATE_SUSPENDED, nullptr, nullptr,
                            &si, &pi)) {
            throw std::runtime_error(last_error_message("CreateProcessW"));
        }

        std::wcout << L"hook_launcher: injecting " << hook_dll << L"\n";
        inject_dll(pi.hProcess, hook_dll);
        std::wcout << L"hook_launcher: resuming process_a\n";

        ResumeThread(pi.hThread);
        WaitForSingleObject(pi.hProcess, INFINITE);

        DWORD exit_code = 1;
        GetExitCodeProcess(pi.hProcess, &exit_code);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        SetEnvironmentVariableW(kRedirectTargetEnv, nullptr);
        return static_cast<int>(exit_code);
    } catch (const std::exception& ex) {
        if (pi.hThread != nullptr) {
            CloseHandle(pi.hThread);
        }
        if (pi.hProcess != nullptr) {
            TerminateProcess(pi.hProcess, 1);
            CloseHandle(pi.hProcess);
        }
        SetEnvironmentVariableW(kRedirectTargetEnv, nullptr);
        std::cerr << "hook_launcher error: " << ex.what() << "\n";
        return 1;
    }
}
