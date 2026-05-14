$ErrorActionPreference = "Stop"

$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$target = Join-Path $root "baseline_output.txt"

if (-not (Test-Path -LiteralPath $target)) {
    New-Item -ItemType File -Path $target | Out-Null
}

Write-Host "Target file: $target"
Write-Host "Type 'hello word' in Notepad, save, then close Notepad."
Write-Host "Use ProcMon filters: Process Name is notepad.exe, Path contains baseline_output.txt."

$proc = Start-Process -FilePath "$env:WINDIR\System32\notepad.exe" -ArgumentList "`"$target`"" -PassThru
Wait-Process -Id $proc.Id

$item = Get-Item -LiteralPath $target
Write-Host ""
Write-Host "After Notepad exited:"
Write-Host "Length: $($item.Length) bytes"
Write-Host "LastWriteTime: $($item.LastWriteTime.ToString('yyyy-MM-dd HH:mm:ss'))"
Write-Host ""
Write-Host "File contents:"
Get-Content -LiteralPath $target -Raw
