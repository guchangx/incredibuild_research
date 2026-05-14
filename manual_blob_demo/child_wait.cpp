#define WIN32_LEAN_AND_MEAN
#include <windows.h>

// Sleeps briefly and returns a fixed code so the parent can replace its start address.
// 短暂休眠并返回固定代码，便于父进程替换它的启动地址。
int wmain() {
    Sleep(1000);
    return 7;
}
