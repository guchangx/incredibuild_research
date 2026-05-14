#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdio>

// Writes a fixed payload through normal CreateFileW and WriteFile calls.
// 通过常规 CreateFileW 和 WriteFile 调用写入固定载荷。
int wmain(int argc, wchar_t** argv) {
    if (argc < 2) {
        return 2;
    }

    HANDLE file = CreateFileW(argv[1], GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        std::printf("child CreateFileW failed: %lu\n", GetLastError());
        return 3;
    }

    const char payload[] = "child payload written through WriteFile\n";
    DWORD written = 0;
    BOOL ok = WriteFile(file, payload, static_cast<DWORD>(sizeof(payload) - 1),
                        &written, nullptr);
    CloseHandle(file);

    if (!ok || written != sizeof(payload) - 1) {
        std::printf("child WriteFile failed: %lu\n", GetLastError());
        return 4;
    }

    return 0;
}
