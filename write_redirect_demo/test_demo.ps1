Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$Root = Split-Path -Parent $MyInvocation.MyCommand.Path
$Out = Join-Path $Root 'out'
$Logs = Join-Path $Out 'test_logs'
$A = Join-Path $Out 'process_a.exe'
$B = Join-Path $Out 'process_b.exe'
$C = Join-Path $Out 'process_c.exe'
$Launcher = Join-Path $Out 'hook_launcher.exe'
$Hook = Join-Path $Out 'redirect_hook.dll'

$DirectFile = Join-Path $Out 'test_case1_direct.txt'
$HookedFile = Join-Path $Out 'test_case2_hooked.txt'
$ServerOut = Join-Path $Logs 'process_b.stdout.log'
$ServerErr = Join-Path $Logs 'process_b.stderr.log'
$ForwardBOut = Join-Path $Logs 'process_b.forward.stdout.log'
$ForwardBErr = Join-Path $Logs 'process_b.forward.stderr.log'
$COut = Join-Path $Logs 'process_c.stdout.log'
$CErr = Join-Path $Logs 'process_c.stderr.log'
$TestFileRoot = Join-Path $Root 'testfile'
$CopyOut = Join-Path $Out 'copied_from_testfile'
$Case3Out = Join-Path $Out 'case3_forwarded_to_c'

function Assert-True($Condition, $Message) {
    if (!$Condition) {
        throw $Message
    }
}

function Assert-FileContains($Path, $Needle) {
    Assert-True (Test-Path -LiteralPath $Path) "missing file: $Path"
    $Text = Get-Content -LiteralPath $Path -Raw
    Assert-True ($Text.Contains($Needle)) "file '$Path' does not contain '$Needle'"
}

function Invoke-Checked($Description, $Command) {
    Write-Host "TEST: $Description"
    & $Command
}

function Ensure-TestFiles {
    New-Item -ItemType Directory -Force -Path $TestFileRoot | Out-Null
    $Existing = Get-ChildItem -LiteralPath $TestFileRoot -File -Recurse -ErrorAction SilentlyContinue
    if ($Existing.Count -gt 0) {
        return
    }

    Set-Content -LiteralPath (Join-Path $TestFileRoot 'hello.txt') `
        -Value "hello from testfile`r`nthis content must survive hooked writes`r`n" `
        -NoNewline `
        -Encoding UTF8

    New-Item -ItemType Directory -Force -Path (Join-Path $TestFileRoot 'nested') | Out-Null
    Set-Content -LiteralPath (Join-Path $TestFileRoot 'nested\data.txt') `
        -Value "nested file`r`nline 2`r`nline 3`r`n" `
        -NoNewline `
        -Encoding ASCII

    $Binary = New-Object byte[] 4096
    for ($Index = 0; $Index -lt $Binary.Length; $Index++) {
        $Binary[$Index] = [byte](($Index * 37 + 11) % 256)
    }
    [System.IO.File]::WriteAllBytes((Join-Path $TestFileRoot 'binary.bin'), $Binary)

    [System.IO.File]::WriteAllBytes((Join-Path $TestFileRoot 'empty.bin'), [byte[]]::new(0))
}

function Relative-Path($Base, $Path) {
    $BaseUri = [System.Uri]((Resolve-Path -LiteralPath $Base).Path.TrimEnd('\') + '\')
    $PathUri = [System.Uri]((Resolve-Path -LiteralPath $Path).Path)
    return [System.Uri]::UnescapeDataString($BaseUri.MakeRelativeUri($PathUri).ToString()).Replace('/', '\')
}

function Assert-SameHash($Expected, $Actual) {
    Assert-True (Test-Path -LiteralPath $Actual) "missing copied file: $Actual"
    $ExpectedHash = (Get-FileHash -LiteralPath $Expected -Algorithm SHA256).Hash
    $ActualHash = (Get-FileHash -LiteralPath $Actual -Algorithm SHA256).Hash
    Assert-True ($ExpectedHash -eq $ActualHash) "hash mismatch: '$Expected' != '$Actual'"
}

New-Item -ItemType Directory -Force -Path $Out, $Logs | Out-Null
Remove-Item -LiteralPath $DirectFile,$HookedFile,$ServerOut,$ServerErr,$ForwardBOut,$ForwardBErr,$COut,$CErr -Force -ErrorAction SilentlyContinue
if (Test-Path -LiteralPath $CopyOut) {
    Remove-Item -LiteralPath $CopyOut -Recurse -Force
}
if (Test-Path -LiteralPath $Case3Out) {
    Remove-Item -LiteralPath $Case3Out -Recurse -Force
}
New-Item -ItemType Directory -Force -Path $CopyOut | Out-Null
New-Item -ItemType Directory -Force -Path $Case3Out | Out-Null
Ensure-TestFiles

Write-Host 'TEST: building demo'
& (Join-Path $Root 'build.ps1')
Assert-True ($LASTEXITCODE -eq 0) "build failed with exit code $LASTEXITCODE"

Assert-True (Test-Path -LiteralPath $A) "missing binary: $A"
Assert-True (Test-Path -LiteralPath $B) "missing binary: $B"
Assert-True (Test-Path -LiteralPath $C) "missing binary: $C"
Assert-True (Test-Path -LiteralPath $Launcher) "missing binary: $Launcher"
Assert-True (Test-Path -LiteralPath $Hook) "missing binary: $Hook"

Write-Host 'TEST: process_a source has no IPC awareness'
$ProcessASource = Get-Content -LiteralPath (Join-Path $Root 'src\process_a.cpp') -Raw
foreach ($Forbidden in @('kPipeName', 'CreateNamedPipe', 'process_b', 'PipeMessage', 'send_write_request')) {
    Assert-True (!$ProcessASource.Contains($Forbidden)) "process_a.cpp unexpectedly contains '$Forbidden'"
}

Write-Host 'TEST: case 1 direct write by process_a'
& $A $DirectFile case1_test_no_hook
Assert-True ($LASTEXITCODE -eq 0) "case 1 failed with exit code $LASTEXITCODE"
Assert-FileContains $DirectFile 'process=process_a'
Assert-FileContains $DirectFile 'label=case1_test_no_hook'
Assert-FileContains $DirectFile 'operation=normal CreateFileW/WriteFile/CloseHandle'

Write-Host 'TEST: case 2 hooked write redirected to process_b'
$Server = Start-Process -FilePath $B `
    -ArgumentList '--once' `
    -WorkingDirectory $Root `
    -RedirectStandardOutput $ServerOut `
    -RedirectStandardError $ServerErr `
    -PassThru `
    -WindowStyle Hidden

try {
    Start-Sleep -Milliseconds 300
    & $Launcher $HookedFile
    Assert-True ($LASTEXITCODE -eq 0) "case 2 launcher failed with exit code $LASTEXITCODE"

    Assert-True ($Server.WaitForExit(5000)) 'process_b did not exit after one redirected request'
    $Server.Refresh()
    if ($null -ne $Server.ExitCode) {
        Assert-True ($Server.ExitCode -eq 0) "process_b failed with exit code $($Server.ExitCode)"
    }
}
finally {
    if (!$Server.HasExited) {
        Stop-Process -Id $Server.Id -Force
    }
}

Assert-FileContains $HookedFile 'process=process_a'
Assert-FileContains $HookedFile 'label=case2_hooked_write'
Assert-FileContains $HookedFile 'operation=normal CreateFileW/WriteFile/CloseHandle'

$ServerLog = Get-Content -LiteralPath $ServerOut -Raw
Assert-True ($ServerLog.Contains('opened redirected target')) 'process_b log did not show redirected open'
Assert-True ($ServerLog.Contains('wrote')) 'process_b log did not show redirected write'
Assert-True ($ServerLog.Contains('closed redirected target')) 'process_b log did not show redirected close'
Assert-True ($ServerLog.Contains($HookedFile)) 'process_b log did not mention hooked target path'

Write-Host 'TEST: copy every file under testfile with and without hook, then compare SHA256'
$InputFiles = Get-ChildItem -LiteralPath $TestFileRoot -File -Recurse | Sort-Object FullName
Assert-True ($InputFiles.Count -gt 0) "no files found under $TestFileRoot"

foreach ($Input in $InputFiles) {
    $Relative = Relative-Path $TestFileRoot $Input.FullName
    $DirectCopy = Join-Path (Join-Path $CopyOut 'direct') $Relative
    $HookedCopy = Join-Path (Join-Path $CopyOut 'hooked') $Relative

    Write-Host "TEST: direct copy $Relative"
    & $A --copy $Input.FullName $DirectCopy
    Assert-True ($LASTEXITCODE -eq 0) "direct copy failed for $Relative with exit code $LASTEXITCODE"
    Assert-SameHash $Input.FullName $DirectCopy

    Write-Host "TEST: hooked copy $Relative"
    Remove-Item -LiteralPath $ServerOut,$ServerErr -Force -ErrorAction SilentlyContinue
    $Server = Start-Process -FilePath $B `
        -ArgumentList '--once' `
        -WorkingDirectory $Root `
        -RedirectStandardOutput $ServerOut `
        -RedirectStandardError $ServerErr `
        -PassThru `
        -WindowStyle Hidden

    try {
        Start-Sleep -Milliseconds 300
        & $Launcher --copy $Input.FullName $HookedCopy
        Assert-True ($LASTEXITCODE -eq 0) "hooked copy launcher failed for $Relative with exit code $LASTEXITCODE"

        Assert-True ($Server.WaitForExit(5000)) "process_b did not exit after hooked copy for $Relative"
        $Server.Refresh()
        if ($null -ne $Server.ExitCode) {
            Assert-True ($Server.ExitCode -eq 0) "process_b failed for $Relative with exit code $($Server.ExitCode)"
        }
    }
    finally {
        if (!$Server.HasExited) {
            Stop-Process -Id $Server.Id -Force
        }
    }

    Assert-SameHash $Input.FullName $HookedCopy
    $CopyServerLog = Get-Content -LiteralPath $ServerOut -Raw
    Assert-True ($CopyServerLog.Contains($HookedCopy)) "process_b log did not mention hooked copy target: $HookedCopy"
}

Write-Host 'TEST: case 3 process_b forwards file stream to process_c over TCP'
$Case3Input = $InputFiles[0]
$Case3Target = Join-Path $Case3Out $Case3Input.Name

$ProcessC = Start-Process -FilePath $C `
    -ArgumentList '--once' `
    -WorkingDirectory $Root `
    -RedirectStandardOutput $COut `
    -RedirectStandardError $CErr `
    -PassThru `
    -WindowStyle Hidden

try {
    Start-Sleep -Milliseconds 500
    $ForwardB = Start-Process -FilePath $B `
        -ArgumentList '--once','--forward-to-c' `
        -WorkingDirectory $Root `
        -RedirectStandardOutput $ForwardBOut `
        -RedirectStandardError $ForwardBErr `
        -PassThru `
        -WindowStyle Hidden

    try {
        Start-Sleep -Milliseconds 300
        & $Launcher --copy $Case3Input.FullName $Case3Target
        Assert-True ($LASTEXITCODE -eq 0) "case 3 launcher failed with exit code $LASTEXITCODE"

        Assert-True ($ForwardB.WaitForExit(5000)) 'process_b forwarder did not exit after case 3'
        Assert-True ($ProcessC.WaitForExit(5000)) 'process_c did not exit after case 3'
    }
    finally {
        if (!$ForwardB.HasExited) {
            Stop-Process -Id $ForwardB.Id -Force
        }
    }
}
finally {
    if (!$ProcessC.HasExited) {
        Stop-Process -Id $ProcessC.Id -Force
    }
}

Assert-SameHash $Case3Input.FullName $Case3Target
$ForwardBLog = Get-Content -LiteralPath $ForwardBOut -Raw
$ProcessCLog = Get-Content -LiteralPath $COut -Raw
Assert-True ($ForwardBLog.Contains('connected to process_c')) 'process_b forward log did not show TCP connection'
Assert-True ($ForwardBLog.Contains('forwarded')) 'process_b forward log did not show forwarding'
Assert-True ($ProcessCLog.Contains('opened TCP target')) 'process_c log did not show TCP open'
Assert-True ($ProcessCLog.Contains('wrote')) 'process_c log did not show TCP write'
Assert-True ($ProcessCLog.Contains('closed TCP target')) 'process_c log did not show TCP close'

Write-Host 'PASS: all integration tests passed'
Write-Host "  direct file: $DirectFile"
Write-Host "  hooked file: $HookedFile"
Write-Host "  copied files: $CopyOut"
Write-Host "  case3 forwarded file: $Case3Target"
Write-Host "  process_b log: $ServerOut"
