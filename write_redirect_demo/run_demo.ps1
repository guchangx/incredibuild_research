Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$Root = Split-Path -Parent $MyInvocation.MyCommand.Path
$Out = Join-Path $Root 'out'
$A = Join-Path $Out 'process_a.exe'
$B = Join-Path $Out 'process_b.exe'
$C = Join-Path $Out 'process_c.exe'
$Launcher = Join-Path $Out 'hook_launcher.exe'
$Hook = Join-Path $Out 'redirect_hook.dll'
$Case1 = Join-Path $Out 'case1_direct.txt'
$Case2 = Join-Path $Out 'case2_hooked.txt'
$OldCase2 = Join-Path $Out 'case2_delegated.txt'
$SampleInput = Join-Path $Root 'testfile\hello.txt'
$SampleCopy = Join-Path $Out 'sample_hooked_copy.txt'
$SampleForwarded = Join-Path $Out 'sample_forwarded_to_c.txt'

if (!(Test-Path $A) -or !(Test-Path $B) -or !(Test-Path $C) -or !(Test-Path $Launcher) -or !(Test-Path $Hook)) {
    & (Join-Path $Root 'build.ps1')
}

Remove-Item -LiteralPath $Case1,$Case2,$OldCase2 -Force -ErrorAction SilentlyContinue
Remove-Item -LiteralPath $SampleCopy,$SampleForwarded -Force -ErrorAction SilentlyContinue
if (!(Test-Path -LiteralPath $SampleInput)) {
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $SampleInput) | Out-Null
    Set-Content -LiteralPath $SampleInput -Value "sample input for hooked copy`r`n" -NoNewline
}

Push-Location $Root
try {
    & $A $Case1 case1_no_hook
    if ($LASTEXITCODE -ne 0) {
        throw "direct case failed with exit code $LASTEXITCODE"
    }

    $Server = Start-Process -FilePath $B -ArgumentList '--once' -WorkingDirectory $Root -PassThru -WindowStyle Hidden
    Start-Sleep -Milliseconds 300

    & $Launcher $Case2
    if ($LASTEXITCODE -ne 0) {
        throw "hooked case failed with exit code $LASTEXITCODE"
    }

    if (!$Server.WaitForExit(5000)) {
        Stop-Process -Id $Server.Id -Force
        throw 'process_b.exe did not exit after delegated write'
    }
    if ($Server.ExitCode -ne 0) {
        throw "process_b.exe failed with exit code $($Server.ExitCode)"
    }
}
finally {
    Pop-Location
}

if (!(Test-Path $Case1)) {
    throw "missing $Case1"
}
    if (!(Test-Path $Case2)) {
    throw "missing $Case2"
}

$Server = Start-Process -FilePath $B -ArgumentList '--once' -WorkingDirectory $Root -PassThru -WindowStyle Hidden
Start-Sleep -Milliseconds 300
& $Launcher --copy $SampleInput $SampleCopy
if ($LASTEXITCODE -ne 0) {
    throw "hooked copy failed with exit code $LASTEXITCODE"
}
if (!$Server.WaitForExit(5000)) {
    Stop-Process -Id $Server.Id -Force
    throw 'process_b.exe did not exit after hooked copy'
}
if (!(Test-Path $SampleCopy)) {
    throw "missing $SampleCopy"
}
$InputHash = (Get-FileHash -LiteralPath $SampleInput -Algorithm SHA256).Hash
$CopyHash = (Get-FileHash -LiteralPath $SampleCopy -Algorithm SHA256).Hash
if ($InputHash -ne $CopyHash) {
    throw 'hooked copy hash mismatch'
}

Write-Host "OK: case1 no hook     $Case1"
Write-Host "OK: case2 hooked I/O  $Case2"
Write-Host "OK: hooked copy       $SampleCopy"

$ProcessC = Start-Process -FilePath $C -ArgumentList '--once' -WorkingDirectory $Root -PassThru -WindowStyle Hidden
Start-Sleep -Milliseconds 500
$Server = Start-Process -FilePath $B -ArgumentList '--once','--forward-to-c' -WorkingDirectory $Root -PassThru -WindowStyle Hidden
Start-Sleep -Milliseconds 300
& $Launcher --copy $SampleInput $SampleForwarded
if ($LASTEXITCODE -ne 0) {
    throw "case3 forward-to-c failed with exit code $LASTEXITCODE"
}
if (!$Server.WaitForExit(5000)) {
    Stop-Process -Id $Server.Id -Force
    throw 'process_b.exe did not exit after forwarding to process_c'
}
if (!$ProcessC.WaitForExit(5000)) {
    Stop-Process -Id $ProcessC.Id -Force
    throw 'process_c.exe did not exit after receiving forwarded file'
}
$ForwardHash = (Get-FileHash -LiteralPath $SampleForwarded -Algorithm SHA256).Hash
if ($InputHash -ne $ForwardHash) {
    throw 'case3 forwarded hash mismatch'
}
Write-Host "OK: forwarded to C    $SampleForwarded"
