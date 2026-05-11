#include "common.hpp"

namespace {

std::wstring default_output_path(const wchar_t* file_name) {
    wchar_t buffer[MAX_PATH]{};
    const DWORD length = GetCurrentDirectoryW(MAX_PATH, buffer);
    if (length == 0 || length >= MAX_PATH) {
        throw std::runtime_error(last_error_message("GetCurrentDirectoryW"));
    }
    std::wstring path(buffer);
    if (!path.empty() && path.back() != L'\\') {
        path += L'\\';
    }
    path += L"out\\";
    path += file_name;
    return path;
}

std::string build_payload(const char* label) {
    SYSTEMTIME now{};
    GetLocalTime(&now);

    char buffer[512]{};
    const int length = std::snprintf(
        buffer, sizeof(buffer),
        "process=process_a\n"
        "label=%s\n"
        "time=%04u-%02u-%02u %02u:%02u:%02u.%03u\n"
        "operation=normal CreateFileW/WriteFile/CloseHandle\n",
        label, now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute,
        now.wSecond, now.wMilliseconds);
    if (length <= 0) {
        throw std::runtime_error("snprintf failed");
    }
    return std::string(buffer, static_cast<std::size_t>(length));
}

void copy_file_with_normal_apis(const std::wstring& input,
                                const std::wstring& output) {
    HANDLE source = CreateFileW(input.c_str(), GENERIC_READ, FILE_SHARE_READ,
                                nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                                nullptr);
    if (source == INVALID_HANDLE_VALUE) {
        throw std::runtime_error(last_error_message("CreateFileW input"));
    }

    ensure_parent_directory(output);
    HANDLE destination =
        CreateFileW(output.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                    CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (destination == INVALID_HANDLE_VALUE) {
        const DWORD error = GetLastError();
        CloseHandle(source);
        SetLastError(error);
        throw std::runtime_error(last_error_message("CreateFileW output"));
    }

    try {
        std::vector<char> buffer(64 * 1024);
        for (;;) {
            DWORD bytes_read = 0;
            if (!ReadFile(source, buffer.data(), static_cast<DWORD>(buffer.size()),
                          &bytes_read, nullptr)) {
                throw std::runtime_error(last_error_message("ReadFile input"));
            }
            if (bytes_read == 0) {
                break;
            }
            write_all(destination, buffer.data(), bytes_read);
        }
    } catch (...) {
        CloseHandle(destination);
        CloseHandle(source);
        throw;
    }

    CloseHandle(destination);
    CloseHandle(source);
}

void usage() {
    std::cout
        << "Usage:\n"
        << "  process_a.exe [output-file] [label]\n"
        << "  process_a.exe --copy <input-file> <output-file>\n\n"
        << "This program only reads files and writes files through normal Windows file APIs.\n";
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    try {
        if (argc >= 2 && std::wstring(argv[1]) == L"--copy") {
            if (argc < 4) {
                usage();
                return 2;
            }
            const std::wstring input = absolute_path(argv[2]);
            const std::wstring output = absolute_path(argv[3]);
            std::wcout << L"process_a: copying " << input << L" -> " << output
                       << L" through normal CreateFileW/ReadFile/WriteFile\n";
            copy_file_with_normal_apis(input, output);
            std::wcout << L"process_a: copy done\n";
            return 0;
        }

        const std::wstring output = absolute_path(
            argc >= 2 ? argv[1] : default_output_path(L"process_a.txt"));
        const std::string label =
            argc >= 3 ? narrow(argv[2]) : std::string("normal_write");
        const std::string payload = build_payload(label.c_str());

        std::wcout << L"process_a: calling CreateFileW/WriteFile for " << output << L"\n";
        write_file_direct(output, payload);
        std::wcout << L"process_a: done\n";
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "process_a error: " << ex.what() << "\n";
        return 1;
    }
}
