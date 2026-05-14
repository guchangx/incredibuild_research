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

    cmd /c "`"$VcVars`" >nul && cl /nologo /std:c++17 /utf-8 /W4 /EHsc parent.cpp /Fe:parent.exe"
    if ($LASTEXITCODE -ne 0) { throw "parent build failed" }

    cmd /c "`"$VcVars`" >nul && cl /nologo /std:c++17 /utf-8 /W4 /O2 /GS- /c payload.cpp /Fo:payload.obj"
    if ($LASTEXITCODE -ne 0) { throw "payload compile failed" }

    cmd /c "`"$VcVars`" >nul && link /nologo /DLL /OUT:payload.bin /ENTRY:DllMain /NODEFAULTLIB payload.obj kernel32.lib"
    if ($LASTEXITCODE -ne 0) { throw "payload link failed" }

    Remove-Item -Force -ErrorAction SilentlyContinue .\child_output.txt, .\redirected_output.txt
    .\parent.exe
    if ($LASTEXITCODE -ne 0) { throw "test failed with exit code $LASTEXITCODE" }

    $childSize = if (Test-Path .\child_output.txt) { (Get-Item .\child_output.txt).Length } else { -1 }
    $redirectText = if (Test-Path .\redirected_output.txt) { Get-Content -Raw .\redirected_output.txt } else { '' }
    if ($childSize -ne 0) {
        throw "expected child_output.txt to exist and stay empty, got size $childSize"
    }
    if ($redirectText -ne "child payload written through WriteFile`n") {
        throw "redirected_output.txt did not contain the expected child payload"
    }

    Write-Host "manual_map_bin_demo test passed"
}
finally {
    Pop-Location
}
