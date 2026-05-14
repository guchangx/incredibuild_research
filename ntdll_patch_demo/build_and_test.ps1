$ErrorActionPreference = 'Stop'

$Root = Split-Path -Parent $MyInvocation.MyCommand.Path
$VsRoot = 'C:\Program Files\Microsoft Visual Studio\18\Enterprise'
$VcVars = Join-Path $VsRoot 'VC\Auxiliary\Build\vcvars64.bat'

if (!(Test-Path $VcVars)) {
    throw "vcvars64.bat not found: $VcVars"
}

Push-Location $Root
try {
    cmd /c "`"$VcVars`" >nul && cl /nologo /std:c++17 /utf-8 /W4 /EHsc child.cpp /Fe:child.exe"
    if ($LASTEXITCODE -ne 0) { throw "child build failed" }

    cmd /c "`"$VcVars`" >nul && cl /nologo /std:c++17 /utf-8 /W4 /EHsc parent_patch.cpp /Fe:parent_patch.exe"
    if ($LASTEXITCODE -ne 0) { throw "parent build failed" }

    Remove-Item -Force -ErrorAction SilentlyContinue .\child_output.txt, .\parent_marker.txt
    .\parent_patch.exe
    if ($LASTEXITCODE -ne 0) { throw "test failed with exit code $LASTEXITCODE" }

    Write-Host "ntdll_patch_demo test passed"
}
finally {
    Pop-Location
}
