#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winternl.h>

constexpr NTSTATUS kStatusSuccess = 0;

// Handles redirected NtWriteFile calls inside the child process.
// 在子进程内部处理被重定向的 NtWriteFile 调用。
extern "C" __declspec(dllexport) NTSTATUS NTAPI MyWriteFile(
    HANDLE FileHandle,
    HANDLE Event,
    PIO_APC_ROUTINE ApcRoutine,
    PVOID ApcContext,
    PIO_STATUS_BLOCK IoStatusBlock,
    PVOID Buffer,
    ULONG Length,
    PLARGE_INTEGER ByteOffset,
    PULONG Key) {
    UNREFERENCED_PARAMETER(FileHandle);
    UNREFERENCED_PARAMETER(Event);
    UNREFERENCED_PARAMETER(ApcRoutine);
    UNREFERENCED_PARAMETER(ApcContext);
    UNREFERENCED_PARAMETER(Buffer);
    UNREFERENCED_PARAMETER(Length);
    UNREFERENCED_PARAMETER(ByteOffset);
    UNREFERENCED_PARAMETER(Key);

    if (IoStatusBlock) {
        IoStatusBlock->Status = kStatusSuccess;
        IoStatusBlock->Information = 0;
    }
    return kStatusSuccess;
}

// Writes a payload file so the parent can demonstrate patching NtWriteFile.
// 写入载荷文件，以便父进程演示修补 NtWriteFile 的效果。
int wmain(int argc, wchar_t** argv) {
    if (argc < 2) {
        return 2;
    }

    const wchar_t* outputPath = argv[1];
    HANDLE file = CreateFileW(
        outputPath,
        GENERIC_WRITE,
        0,
        nullptr,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);

    if (file == INVALID_HANDLE_VALUE) {
        return 3;
    }

    const char payload[] = "this data should be swallowed by the patched NtWriteFile\n";
    DWORD written = 0;
    BOOL ok = WriteFile(file, payload, static_cast<DWORD>(sizeof(payload) - 1), &written, nullptr);
    CloseHandle(file);

    return ok ? 0 : 4;
}
