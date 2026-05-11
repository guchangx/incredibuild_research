# Analysis of `D:\06.dat`

## Summary

The updated `D:\06.dat` is the same kind of file shown in the helper-side ProcMon module list:

```text
C:\Program Files (x86)\Incredibuild\FileCache\00\00\00\06.dat
```

It is not a header file. It is a Microsoft PE DLL that Incredibuild cached under an internal `.dat` filename.

The file is:

```text
OriginalFilename: FileTracker.dll
FileDescription:  FileTracker
ProductName:      Microsoft Build Tools
CompanyName:      Microsoft Corporation
FileVersion:      17.0.33905.217 built by: D17.7
ProductVersion:   17.0.33905.217
```

This is a Microsoft Build Tools file-tracking DLL, not an Incredibuild-owned DLL.

## File Metadata

Observed file:

```text
D:\06.dat
```

Metadata:

```text
Length:        296344 bytes
CreationTime:  2026-05-09 15:57:01
LastWriteTime: 2026-05-09 12:16:31
Attributes:    Archive
```

SHA256:

```text
20d91f9dab16e3f3d1307501cb8b714056776e46f653412887b80fb3c124e322
```

## PE Evidence

The file starts with an `MZ` DOS header and has a normal PE header:

```text
00000000  4D 5A ...
00000110  50 45 00 00 ...
```

`dumpbin /headers` reports:

```text
File Type: DLL
Machine:   x64
Image size: 0x4D000
Subsystem: Windows GUI
```

This matches the ProcMon module list, which showed:

```text
Base: 0x7ffd56610000
Size: 0x4d000
```

## Signature

Authenticode signature is valid:

```text
Signer: Microsoft Corporation
Issuer: Microsoft Code Signing PCA 2011
Status: Valid
```

This explains why the module does not look like an unfamiliar third-party DLL in `cl.exe`. It is Microsoft-signed and named as `.dat` only because it is loaded from Incredibuild's cache.

## Exports

`dumpbin /exports D:\06.dat` reports exports for `FileTracker.dll`:

```text
StartTrackingContext
StartTrackingContextWithRoot
EndTrackingContext
StopTrackingAndCleanup
SuspendTracking
ResumeTracking
WriteAllTLogs
WriteContextTLogs
SetThreadCount
```

These names strongly indicate MSBuild/VC file access tracking.

## Detours Evidence

The PE section table contains:

```text
.detourc
.detourd
```

Those section names are strong evidence that this DLL uses Microsoft Detours-style API interception internally.

## File API Imports

`dumpbin /imports D:\06.dat` shows imports including:

```text
CreateFileA
CreateFileW
ReadFile
WriteFile
CloseHandle
DeleteFileA
DeleteFileW
MoveFileA
MoveFileW
MoveFileExA
MoveFileExW
FindFirstFileW
FindFirstFileExA
GetFileAttributesA
GetFileAttributesW
GetFileAttributesExA
GetFileAttributesExW
SetFileInformationByHandle
SetFilePointer
SetFilePointerEx
```

This is exactly the kind of API surface a file tracking component would observe.

## Relationship To The Helper `cl.exe`

The helper-side ProcMon module list showed:

```text
CL.exe          -> C:\Program Files (x86)\Incredibuild\ModuleCache\...\CL.exe
mspdbcore.dll   -> C:\Program Files (x86)\Incredibuild\ModuleCache\...\mspdbcore.dll
c1xx.dll        -> C:\Program Files (x86)\Incredibuild\ModuleCache\...\c1xx.dll
06.dat          -> C:\Program Files (x86)\Incredibuild\FileCache\00\00\00\06.dat
```

The `06.dat` module is actually Microsoft `FileTracker.dll`, loaded from Incredibuild `FileCache` under a cache name.

Therefore, the corrected interpretation is:

```text
cl.exe does have a file-tracking/interception-capable module loaded.
It just is not named FileTracker.dll on disk.
It is not an Incredibuild DLL; it is Microsoft FileTracker.dll cached as 06.dat.
```

## What This Does And Does Not Prove

This proves:

1. A Microsoft file-tracking DLL is loaded into helper-side `cl.exe`.
2. That DLL exports tracking APIs such as `StartTrackingContext` and `WriteAllTLogs`.
3. It imports file APIs such as `CreateFileW`, `ReadFile`, and `WriteFile`.
4. It contains Detours-related sections.
5. Incredibuild stores it under `FileCache` as `06.dat`, so module names in ProcMon can hide the original filename.

This does not by itself prove that `FileTracker.dll` redirects `.obj` output to another process. Microsoft FileTracker is normally used by MSBuild/VC to track file reads/writes and produce `.tlog` dependency logs.

However, it is strong evidence that the helper `cl.exe` process is not a plain uninstrumented compiler process. It has at least Microsoft file-tracking instrumentation loaded.

## Likely Incredibuild Mechanism From Current Evidence

The current evidence suggests this helper-side flow:

```text
Incredibuild BuildService
  -> prepares ModuleCache and FileCache
  -> starts cached CL.exe from ModuleCache
  -> supplies arguments via Incredibuild Temp\cmdfile.tmp
  -> loads Microsoft FileTracker.dll from FileCache as 06.dat
  -> cl.exe runs with file tracking active
```

The absence of an obvious Incredibuild-named DLL does not mean there is no instrumentation. The loaded module:

```text
FileCache\...\06.dat
```

is really:

```text
Microsoft FileTracker.dll
```

and it has Detours/file-API tracking capabilities.

For output files, Incredibuild may still combine several techniques:

1. Response-file rewriting: `/Fo`, `/Fd`, temp, and working paths may point into Incredibuild-controlled paths.
2. Module/input virtualization: compiler binaries and dependent DLLs are loaded from `ModuleCache` and `FileCache`.
3. File tracking: `FileTracker.dll` records file accesses and outputs via tracking APIs/TLogs.
4. Service-side collection: `BuildService.exe` or another Incredibuild process transfers/restores produced outputs.

The most important next artifact to capture is:

```text
C:\Program Files (x86)\Incredibuild\Temp\cmdfile.tmp
```

That response file likely contains the actual `/Fo`, `/Fd`, `/I`, source path, and output path arguments that determine where `cl.exe` writes.

## Why `/Fo` Rewriting Alone Is Not Enough

If Incredibuild only changed `/Fo` to another normal disk path, ProcMon should still show some kind of write I/O from `cl.exe`, just to a different path.

For example:

```text
cl.exe -> WriteFile -> C:\Program Files (x86)\Incredibuild\Temp\...\file.obj
```

or:

```text
cl.exe -> WriteFile -> C:\Program Files (x86)\Incredibuild\FileCache\...\xx.dat
```

Therefore, an observation of:

```text
cl.exe has reads
cl.exe has no visible file writes at all
```

cannot be fully explained by `/Fo` rewriting alone.

The loaded `06.dat` module is the missing evidence. It is Microsoft `FileTracker.dll`, and it contains Detours-related sections:

```text
.detourc
.detourd
```

It also imports:

```text
CreateFileW
ReadFile
WriteFile
CloseHandle
MoveFile*
DeleteFile*
GetFileAttributes*
```

This means the helper-side `cl.exe` is not a plain compiler process. It is running with a file-tracking/interception-capable Microsoft module loaded.

The refined explanation is:

```text
The write path may be intercepted in user mode before the call reaches the kernel file-system stack.
If the intercepted output write is redirected to an IPC channel, memory buffer, tracking context, or another process, ProcMon will not show cl.exe performing a normal WriteFile to the final target file.
```

This is similar in shape to the local demo's hook case, but with an important difference:

```text
Demo:          redirect_hook.dll is our visible hook module.
Incredibuild: 06.dat is Microsoft FileTracker.dll cached under an Incredibuild name.
```

So the correct conclusion is not:

```text
There is no injected/instrumentation module.
```

The correct conclusion is:

```text
There is no obvious Incredibuild-named DLL.
But there is a Microsoft FileTracker.dll module loaded as FileCache\...\06.dat.
That module is capable of Detours-style file API interception/tracking.
```

## Why ProcMon May Show No `cl.exe` WriteFile

Possible explanations consistent with the evidence:

1. `FileTracker.dll` detours file APIs and the output write does not call through to the real kernel `WriteFile` from `cl.exe`.
2. The output stream is handed to another component/process, so ProcMon attributes final disk writes to `BuildService.exe`, `mspdbsrv.exe`, or another helper-side process.
3. Writes may go to a named pipe or IPC path rather than a normal disk file. If the ProcMon filter only watched disk paths or `.obj`, this can be missed.
4. Some outputs may be memory-mapped and flushed by the memory manager, causing visible writes to be attributed differently.
5. The observed `cl.exe` may be running under a tracking context where `FileTracker.dll` records read/write intent and Incredibuild reconstructs/collects output separately.

The strongest immediate hypothesis is:

```text
helper cl.exe output is intercepted/tracked by Microsoft FileTracker.dll loaded as 06.dat,
while Incredibuild controls the process launch, response file, ModuleCache/FileCache, and output collection.
```

This better explains both facts:

```text
No Incredibuild-named DLL appears in cl.exe.
No normal cl.exe WriteFile is visible in ProcMon.
```
