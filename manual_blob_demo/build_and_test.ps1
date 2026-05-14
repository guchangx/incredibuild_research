$ErrorActionPreference = 'Stop'

$Root = Split-Path -Parent $MyInvocation.MyCommand.Path
$VsRoot = 'C:\Program Files\Microsoft Visual Studio\18\Enterprise'
$VcVars = Join-Path $VsRoot 'VC\Auxiliary\Build\vcvars64.bat'

if (!(Test-Path $VcVars)) {
    throw "vcvars64.bat not found: $VcVars"
}

Push-Location $Root
try {
    cmd /c "`"$VcVars`" >nul && cl /nologo /std:c++17 /utf-8 /W4 /EHsc child_wait.cpp /Fe:child_wait.exe"
    if ($LASTEXITCODE -ne 0) { throw "child build failed" }

    cmd /c "`"$VcVars`" >nul && cl /nologo /std:c++17 /utf-8 /W4 /EHsc manual_blob_parent.cpp /Fe:manual_blob_parent.exe psapi.lib"
    if ($LASTEXITCODE -ne 0) { throw "parent build failed" }

    cmd /c "`"$VcVars`" >nul && cl /nologo /std:c++17 /utf-8 /W4 /EHsc child_case2.cpp /Fe:child_case2.exe"
    if ($LASTEXITCODE -ne 0) { throw "case2 child build failed" }

    cmd /c "`"$VcVars`" >nul && cl /nologo /std:c++17 /utf-8 /W4 /EHsc parent_case2.cpp /Fe:parent_case2.exe psapi.lib"
    if ($LASTEXITCODE -ne 0) { throw "case2 parent build failed" }

    Write-Host "Running case1: anonymous blob executes in child"
    .\manual_blob_parent.exe
    if ($LASTEXITCODE -ne 0) { throw "case1 test failed with exit code $LASTEXITCODE" }

    Write-Host "Running case2: child reads locally, parent intercepts child WriteFile"
    .\parent_case2.exe
    if ($LASTEXITCODE -ne 0) { throw "case2 test failed with exit code $LASTEXITCODE" }

    Write-Host "manual_blob_demo test passed"
}
finally {
    Pop-Location
}
