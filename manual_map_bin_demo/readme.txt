manual_map_bin_demo

This demo models the Incredibuild-like hypothesis we investigated:

- The parent launches a child process suspended.
- The parent manually maps a PE image named payload.bin into the child.
- payload.bin is not loaded with LoadLibrary in the child.
- payload.bin patches the child main executable IAT entry for WriteFile.
- The child calls normal CreateFileW and WriteFile.
- The WriteFile call is handled by payload.bin logic inside the child process.

Files:

- child.cpp: normal child program that writes "child payload written through WriteFile\n".
- payload.cpp: PE DLL source linked as payload.bin; installs the WriteFile hook.
- parent.cpp: manual mapper and test driver.
- build_and_test.ps1: builds all binaries and validates the effect.

Expected test result:

- child.exe exits with code 0.
- child_output.txt exists but has size 0.
- redirected_output.txt exists and contains the child payload.

Why this resembles the Incredibuild hypothesis:

- The mapped code is a .bin PE image, not a visible LoadLibrary DLL.
- The parent uses remote memory APIs to place executable code in the child.
- File API behavior is changed in user mode by code running inside the child.
- The child process still calls WriteFile normally from its own code.

Important differences from a real Incredibuild implementation:

- This demo only patches WriteFile in the child main module IAT.
- It does not virtualize CreateFileW, ReadFile, NtCreateFile, NtWriteFile, PDB/PCH paths, response files, or compiler temp directories.
- It does not implement a cache service, remote agent, network transport, or process-wide module tracking.
- It maps at the payload preferred base because this minimal payload is linked without relocations.

Run:

powershell -ExecutionPolicy Bypass -File .\build_and_test.ps1
