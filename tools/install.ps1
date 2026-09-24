# Install the built Kagee plugin into the machine-wide OBS plugin folder
# (%ProgramData%\obs-studio\plugins\kagee). Close OBS before running.
# Uninstall: delete that folder.
param([string]$Config = "Release")
$ErrorActionPreference = "Stop"
$root = Split-Path $PSScriptRoot -Parent
$dll = Join-Path $root "build\$Config\kagee.dll"
if (-not (Test-Path $dll)) { throw "not built yet: run  cmake -S . -B build -A x64  and  cmake --build build --config $Config" }
if (Get-Process obs64 -ErrorAction SilentlyContinue) { Write-Warning "OBS is running - restart it after installing." }

$dest = Join-Path $env:ProgramData "obs-studio\plugins\kagee"
New-Item -ItemType Directory -Force "$dest\bin\64bit", "$dest\data" | Out-Null
Copy-Item $dll "$dest\bin\64bit\" -Force
Copy-Item "$root\data\*" "$dest\data\" -Recurse -Force
Write-Host "Installed to $dest"
