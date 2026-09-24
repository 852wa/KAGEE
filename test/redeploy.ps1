# Rebuild Kagee, install into the portable OBS test copy and restart it.
param([string]$Portable = (Join-Path (Split-Path (Split-Path $PSScriptRoot -Parent) -Parent) "obs-portable"), [switch]$NoStart)
$ErrorActionPreference = "Stop"
$root = Split-Path $PSScriptRoot -Parent

Get-Process obs64 -ErrorAction SilentlyContinue | Where-Object { $_.Path -like "$Portable*" } | ForEach-Object {
    Stop-Process -Id $_.Id -Force
    $_.WaitForExit(8000) | Out-Null
}

$build = cmake --build "$root\build" --config Release 2>&1
if ($LASTEXITCODE -ne 0) { $build | Select-String "error" | ForEach-Object { $_.Line }; throw "build failed" }
$build | Select-String "warning" | ForEach-Object { $_.Line }

Copy-Item "$root\build\Release\kagee.dll" "$Portable\obs-plugins\64bit\" -Force
New-Item -ItemType Directory -Force "$Portable\data\obs-plugins\kagee" | Out-Null
Copy-Item "$root\data\*" "$Portable\data\obs-plugins\kagee\" -Recurse -Force

if (-not $NoStart) {
    # we kill OBS hard above; drop the unclean-shutdown sentinel so no safe-mode prompt appears
    Remove-Item -Recurse -Force "$Portable\config\obs-studio\.sentinel" -ErrorAction SilentlyContinue
    python "$PSScriptRoot\setup_portable.py" $Portable | Out-Null
    $bin = "$Portable\bin\64bit"
    Start-Process -FilePath "$bin\obs64.exe" -WorkingDirectory $bin -ArgumentList "--portable", "--disable-updater", "--disable-shutdown-check", "--minimize-to-tray"
}
Write-Host "deployed"
