# ntdll patch demo

This demo starts its own child process suspended, patches that child process'
in-memory `ntdll!NtWriteFile`, then resumes it.

The child tries to write `child_output.txt`. With the patch active, the call
returns success but no bytes are written to disk. The parent writes a small marker
file through normal file APIs so the test can distinguish parent-side activity
from child-side activity.

This is intentionally scoped to a self-launched child process. It does not patch
an arbitrary PID.

Run:

```powershell
.\build_and_test.ps1
```
