Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$Root = Split-Path -Parent $MyInvocation.MyCommand.Path
$VsWhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"

function Invoke-BuildWithEnvironment($EnvCommand) {
    Push-Location $Root
    try {
        cmd.exe /c "$EnvCommand && build_msvc.bat"
        if ($LASTEXITCODE -ne 0) {
            throw "build failed with exit code $LASTEXITCODE"
        }
    }
    finally {
        Pop-Location
    }
}

if (Get-Command cl.exe -ErrorAction SilentlyContinue) {
    Push-Location $Root
    try {
        & .\build_msvc.bat
        if ($LASTEXITCODE -ne 0) {
            throw "build failed with exit code $LASTEXITCODE"
        }
    }
    finally {
        Pop-Location
    }
    exit 0
}

if (Test-Path $VsWhere) {
    $InstallPath = & $VsWhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if ($InstallPath) {
        $VsDevCmd = Join-Path $InstallPath 'Common7\Tools\VsDevCmd.bat'
        if (Test-Path $VsDevCmd) {
            Invoke-BuildWithEnvironment "`"$VsDevCmd`" -arch=x64"
            exit 0
        }

        $VcVarsAll = Join-Path $InstallPath 'VC\Auxiliary\Build\vcvarsall.bat'
        if (Test-Path $VcVarsAll) {
            Invoke-BuildWithEnvironment "`"$VcVarsAll`" x64"
            exit 0
        }
    }
}

throw 'MSVC cl.exe was not found. Install Visual Studio C++ build tools, or run this from an x64 Native Tools Command Prompt.'

