# Write Redirect Demo

This demo has two modes:

- Case 1: `process_a.exe` writes the target file directly.
- Case 2: `hook_launcher.exe` starts `process_a.exe` suspended, injects `redirect_hook.dll`, then `process_a.exe` performs the same normal file write. The hook intercepts `CreateFileW` / `WriteFile` / `CloseHandle` and sends the write stream to `process_b.exe`, which writes the target file.
- Case 3: `process_b.exe --forward-to-c` receives the hooked write stream from `process_a.exe`, forwards it over TCP to `process_c.exe`, and `process_c.exe` writes the target file.

`process_a.exe` never contains code that talks to `process_b.exe`. This is the point of the demo: the target process still calls normal Windows file APIs.

`process_c.exe` listens on `127.0.0.1:39017`.

Build from an MSVC Developer Command Prompt:

```bat
build_msvc.bat
```

Or from a normal PowerShell after Visual Studio C++ tools are installed:

```powershell
.\build.ps1
```

If PowerShell blocks script execution with an execution policy error, run:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\build.ps1
```

Run case 1:

```bat
out\process_a.exe out\case1_direct.txt case1_no_hook
```

Run case 2:

```bat
start "process_b" out\process_b.exe --once
out\hook_launcher.exe out\case2_hooked.txt
```

Run case 3:

```bat
start "process_c" out\process_c.exe --once
start "process_b" out\process_b.exe --once --forward-to-c
out\hook_launcher.exe --copy testfile\00.dat out\case3_forwarded_to_c\00.dat
```

Or run both cases and validate output files:

```powershell
.\run_demo.ps1
```

If PowerShell blocks script execution for the validation script, run:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\run_demo.ps1
```

Run the automated integration tests:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\test_demo.ps1
```

The integration test also walks every file under:

```text
D:\workspace\write_redirect_demo\testfile
```

For each input file it writes two copies:

```text
out\copied_from_testfile\direct\...
out\copied_from_testfile\hooked\...
```

Then it compares SHA256 hashes between the original file and both outputs.

ProcMon expectation:

- For `case1_direct.txt`, target-file `CreateFile`/`WriteFile` should be under `process_a.exe`.
- For `case2_hooked.txt`, target-file `CreateFile`/`WriteFile` should be under `process_b.exe`.
- In case 2, `process_a.exe` still calls `CreateFileW` / `WriteFile`, but the injected DLL intercepts those calls before they reach the OS file stack. ProcMon should show `process_a.exe` writing to `\Device\NamedPipe\write_redirect_demo_pipe`, not to `case2_hooked.txt`.
- In case 3, `process_b.exe` should connect to `127.0.0.1:39017` and `process_c.exe` should perform the final target-file `CreateFile` / `WriteFile`.

Useful ProcMon filters:

```text
Process Name is process_a.exe OR Process Name is process_b.exe
OR Process Name is process_c.exe
Operation is CreateFile OR Operation is WriteFile OR Operation is CloseFile
Path contains case1_direct.txt OR Path contains case2_hooked.txt OR Path contains case3_forwarded_to_c OR Path contains write_redirect_demo_pipe
```

Implementation notes:

- `redirect_hook.dll` patches the import address table of the started `process_a.exe`.
- This demo intentionally targets the simple case where `process_a.exe` imports `CreateFileW`, `WriteFile`, and `CloseHandle` from `kernel32.dll`.
- A production-grade hook would also handle dynamically resolved APIs, `CreateFile2`, `NtCreateFile`, overlapped I/O, duplicate handles, child processes, and multiple simultaneously redirected files.
