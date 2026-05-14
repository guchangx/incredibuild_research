ntdll_patch_demo 逻辑说明

这个 demo 的重点是：父进程没有让子进程跳到父进程里的 WriteFile 或 NtWriteFile 执行。
父进程会找到子进程 exe 中导出的 MyWriteFile 函数，并修改子进程自己的 ntdll!NtWriteFile 函数入口，让它跳到子进程内部的 MyWriteFile。

1. 先区分两个地址空间

父进程地址空间里有：
- parent_patch.exe 自己的代码和数据。
- 父进程自己的 ntdll.dll。
- patch、hookCode、jump 等局部变量。

子进程地址空间里有：
- child.exe。
- child.exe 导出的 MyWriteFile 函数。
- 子进程自己的 ntdll.dll。

父进程和子进程是不同的虚拟地址空间。即使某个地址数值看起来一样，也分别指向各自进程里的内存。

2. 父进程启动挂起的子进程

父进程使用 CreateProcessW 创建 child.exe，并带上 CREATE_SUSPENDED。
这样子进程已经创建完成，但主线程还没有继续执行。
父进程可以在子进程真正写文件之前先修改它的内存。

3. 父进程找到 NtWriteFile 的地址

父进程调用：

GetModuleHandleW(L"ntdll.dll")
GetProcAddress(ntdll, "NtWriteFile")

这得到的是父进程里 ntdll!NtWriteFile 的地址。

当前 demo 做了一个简化假设：同架构、普通情况下，系统 DLL 在父子进程中的加载基址相同。
所以父进程拿到的 NtWriteFile 地址数值，在子进程里也对应子进程自己的 ntdll!NtWriteFile。

注意：这里不是要让子进程执行父进程的 NtWriteFile。
这个地址只是被当作定位子进程 ntdll!NtWriteFile 的参考。

4. 父进程找到子进程中的 MyWriteFile

child.cpp 中导出了一个函数：

MyWriteFile

这个函数和 NtWriteFile 使用相同的调用约定和参数形式。
它会把 IoStatusBlock 设置为成功，并返回成功状态，但不会真正写入文件数据。

父进程为了找到子进程中的 MyWriteFile 地址，会做两件事：

- 从子进程 PEB 中读取 child.exe 在子进程中的映像基地址。
- 在父进程中用 LoadLibraryExW 加载 child.exe，并用 GetProcAddress 找到 MyWriteFile 的 RVA。

然后计算：

子进程 MyWriteFile 地址 = 子进程 child.exe 映像基地址 + MyWriteFile RVA

注意：父进程不是让子进程调用父进程里的 MyWriteFile。
它只是用本地加载的 child.exe 来计算 MyWriteFile 在子进程里的相对位置。

5. 被修改的子进程内存：子进程的 ntdll!NtWriteFile 开头

父进程构造了一段跳转代码 jump：

mov rax, <子进程中的 MyWriteFile 地址>
jmp rax

然后父进程调用 VirtualProtectEx，把子进程 ntdll!NtWriteFile 开头那段内存临时改成可写。

接着父进程调用 WriteProcessMemory，把 jump 写到子进程 ntdll!NtWriteFile 的函数入口。

所以真正被改写的是：

子进程地址空间里的 ntdll!NtWriteFile 函数开头。

父进程自己的 ntdll!NtWriteFile 没有被修改。

6. 子进程恢复运行后的调用链

子进程恢复运行后，child.cpp 里调用 WriteFile。

调用链大致变成：

child.exe 调用 WriteFile
-> Windows 库内部调用 NtWriteFile
-> 子进程自己的 ntdll!NtWriteFile 已经被改写
-> 入口处跳转到子进程里的 MyWriteFile
-> MyWriteFile 设置成功状态并返回
-> 返回成功，但没有写入 payload 数据

所以最后看到的效果是：

- child.exe 认为 WriteFile 成功了。
- 文件可能被 CreateFileW 创建出来。
- 但是 NtWriteFile 的真正写入动作被 hook 吞掉了。
- 因此 child_output.txt 是 0 字节。

7. 一句话总结

父进程拿自己的 NtWriteFile 地址作为参考，推测子进程中对应的 NtWriteFile 地址。
但实际修改的是子进程内存：

- 找到子进程 child.exe 中的 MyWriteFile 地址。
- 把子进程 ntdll!NtWriteFile 的入口改成跳到这个 MyWriteFile。

子进程执行的所有代码仍然发生在子进程地址空间里，并不会跨进程执行父进程里的函数。
