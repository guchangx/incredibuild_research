#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "protocol.hpp"

#include <cstdio>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

// Converts UTF-8 text to a UTF-16 wide string.
// 将 UTF-8 文本转换为 UTF-16 宽字符串。
inline std::wstring widen(std::string_view text) {
    if (text.empty()) {
        return {};
    }

    const int required = MultiByteToWideChar(CP_UTF8, 0, text.data(),
                                             static_cast<int>(text.size()),
                                             nullptr, 0);
    if (required <= 0) {
        throw std::runtime_error("MultiByteToWideChar failed");
    }

    std::wstring result(static_cast<std::size_t>(required), L'\0');
    const int written = MultiByteToWideChar(CP_UTF8, 0, text.data(),
                                            static_cast<int>(text.size()),
                                            result.data(), required);
    if (written != required) {
        throw std::runtime_error("MultiByteToWideChar wrote unexpected length");
    }

    return result;
}

// Converts UTF-16 wide text to a UTF-8 string.
// 将 UTF-16 宽文本转换为 UTF-8 字符串。
inline std::string narrow(std::wstring_view text) {
    if (text.empty()) {
        return {};
    }

    const int required = WideCharToMultiByte(CP_UTF8, 0, text.data(),
                                             static_cast<int>(text.size()),
                                             nullptr, 0, nullptr, nullptr);
    if (required <= 0) {
        throw std::runtime_error("WideCharToMultiByte failed");
    }

    std::string result(static_cast<std::size_t>(required), '\0');
    const int written = WideCharToMultiByte(CP_UTF8, 0, text.data(),
                                            static_cast<int>(text.size()),
                                            result.data(), required,
                                            nullptr, nullptr);
    if (written != required) {
        throw std::runtime_error("WideCharToMultiByte wrote unexpected length");
    }

    return result;
}

// Builds a readable message from the current Win32 last-error value.
// 根据当前 Win32 last-error 值构造可读错误消息。
inline std::string last_error_message(const char* prefix) {
    const DWORD error = GetLastError();
    LPSTR buffer = nullptr;
    const DWORD size = FormatMessageA(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
            FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, error, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        reinterpret_cast<LPSTR>(&buffer), 0, nullptr);

    std::string message = prefix;
    message += " failed with error ";
    message += std::to_string(error);
    if (size != 0 && buffer != nullptr) {
        message += ": ";
        message += buffer;
        LocalFree(buffer);
    }
    return message;
}

// Resolves a path to its absolute Win32 path form.
// 将路径解析为 Win32 绝对路径形式。
inline std::wstring absolute_path(const std::wstring& path) {
    const DWORD required = GetFullPathNameW(path.c_str(), 0, nullptr, nullptr);
    if (required == 0) {
        throw std::runtime_error(last_error_message("GetFullPathNameW"));
    }

    std::wstring result(required, L'\0');
    const DWORD written =
        GetFullPathNameW(path.c_str(), required, result.data(), nullptr);
    if (written == 0 || written >= required) {
        throw std::runtime_error(last_error_message("GetFullPathNameW"));
    }

    result.resize(written);
    return result;
}

// Writes the entire buffer to a Win32 handle.
// 将整个缓冲区写入 Win32 句柄。
inline void write_all(HANDLE handle, const void* data, std::size_t size) {
    const auto* cursor = static_cast<const std::uint8_t*>(data);
    std::size_t remaining = size;
    while (remaining > 0) {
        const DWORD chunk = remaining > 64 * 1024
                                ? 64 * 1024
                                : static_cast<DWORD>(remaining);
        DWORD written = 0;
        if (!WriteFile(handle, cursor, chunk, &written, nullptr)) {
            throw std::runtime_error(last_error_message("WriteFile"));
        }
        if (written == 0) {
            throw std::runtime_error("WriteFile wrote zero bytes");
        }
        cursor += written;
        remaining -= written;
    }
}

// Reads exactly the requested number of bytes from a Win32 handle.
// 从 Win32 句柄读取指定数量的字节。
inline void read_all(HANDLE handle, void* data, std::size_t size) {
    auto* cursor = static_cast<std::uint8_t*>(data);
    std::size_t remaining = size;
    while (remaining > 0) {
        const DWORD chunk = remaining > 64 * 1024
                                ? 64 * 1024
                                : static_cast<DWORD>(remaining);
        DWORD read = 0;
        if (!ReadFile(handle, cursor, chunk, &read, nullptr)) {
            throw std::runtime_error(last_error_message("ReadFile"));
        }
        if (read == 0) {
            throw std::runtime_error("ReadFile read zero bytes");
        }
        cursor += read;
        remaining -= read;
    }
}

// Creates the parent directory chain for a target path if needed.
// 按需为目标路径创建父目录链。
inline void ensure_parent_directory(const std::wstring& path) {
    const std::size_t slash = path.find_last_of(L"\\/");
    if (slash == std::wstring::npos) {
        return;
    }

    const std::wstring directory = path.substr(0, slash);
    if (directory.empty()) {
        return;
    }

    std::wstring current;
    std::size_t index = 0;

    if (directory.size() >= 3 && directory[1] == L':' &&
        (directory[2] == L'\\' || directory[2] == L'/')) {
        current = directory.substr(0, 3);
        index = 3;
    }

    while (index <= directory.size()) {
        const std::size_t next = directory.find_first_of(L"\\/", index);
        const std::wstring part =
            directory.substr(index, next == std::wstring::npos
                                        ? std::wstring::npos
                                        : next - index);
        if (!part.empty()) {
            if (!current.empty() && current.back() != L'\\' &&
                current.back() != L'/') {
                current += L'\\';
            }
            current += part;
            if (!CreateDirectoryW(current.c_str(), nullptr)) {
                const DWORD error = GetLastError();
                if (error != ERROR_ALREADY_EXISTS) {
                    throw std::runtime_error(last_error_message("CreateDirectoryW"));
                }
            }
        }
        if (next == std::wstring::npos) {
            break;
        }
        index = next + 1;
    }
}

// Writes a payload directly to a file through normal Win32 file APIs.
// 通过常规 Win32 文件 API 将载荷直接写入文件。
inline void write_file_direct(const std::wstring& path,
                              std::string_view payload) {
    ensure_parent_directory(path);
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ,
                              nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                              nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        throw std::runtime_error(last_error_message("CreateFileW"));
    }

    try {
        write_all(file, payload.data(), payload.size());
    } catch (...) {
        CloseHandle(file);
        throw;
    }
    CloseHandle(file);
}
