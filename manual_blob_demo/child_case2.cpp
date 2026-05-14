#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdio>

// Copies bytes from the input file to the output file using normal Win32 file APIs.
// 使用常规 Win32 文件 API 将输入文件中的字节复制到输出文件。
int wmain(int argc, wchar_t** argv) {
    if (argc < 3) {
        return 2;
    }

    const wchar_t* inputPath = argv[1];
    const wchar_t* outputPath = argv[2];

    HANDLE input = CreateFileW(inputPath, GENERIC_READ, FILE_SHARE_READ, nullptr,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (input == INVALID_HANDLE_VALUE) {
        return 3;
    }

    char buffer[256]{};
    DWORD bytesRead = 0;
    BOOL readOk = ReadFile(input, buffer, static_cast<DWORD>(sizeof(buffer)), &bytesRead, nullptr);
    CloseHandle(input);
    if (!readOk || bytesRead == 0) {
        return 4;
    }

    HANDLE output = CreateFileW(outputPath, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
    if (output == INVALID_HANDLE_VALUE) {
        return 5;
    }

    DWORD bytesWritten = 0;
    BOOL writeOk = WriteFile(output, buffer, bytesRead, &bytesWritten, nullptr);
    CloseHandle(output);

    if (!writeOk || bytesWritten != bytesRead) {
        return 6;
    }

    return 0;
}
