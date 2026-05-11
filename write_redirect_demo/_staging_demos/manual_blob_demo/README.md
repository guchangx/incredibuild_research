# manual blob demo

This project contains two self-contained cases.

Case 1 starts its own child process suspended, allocates anonymous executable
memory in that process, writes a small x64 binary blob there, and redirects the
child thread to execute it. The blob writes a marker value into shared memory and
returns.

Case 2 starts a child that normally reads one local input file and writes one
output file. The parent leaves the child's `ReadFile` path untouched, but patches
the child's `WriteFile` import table to point at an anonymous two-byte blob
(`int3; ret`). The parent debug loop catches that breakpoint, reads the child's
`WriteFile` arguments and buffer, writes the bytes itself to a redirected output
file, then makes the child see `WriteFile` as successful.

No DLL is loaded into the child. The executable memory is anonymous private
memory, which demonstrates the core idea behind a manually mapped binary blob:
code can run in a process without appearing as a normal loaded module. Case 2
also demonstrates parent-side brokering of a child write operation.

This is intentionally scoped to a self-launched child process. It does not target
an arbitrary PID.

Run:

```powershell
.\build_and_test.ps1
```
