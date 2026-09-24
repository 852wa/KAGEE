# Fetches build dependencies into .deps/ (ignored by git):
#   .deps/obs-studio : libobs + frontend API headers of the OBS version we build against
#   .deps/qt6        : the prebuilt Qt package OBS itself ships with (hash-verified)
param(
    [string]$ObsVersion = "32.2.2",
    [string]$DepsVersion = "2026-07-15",
    [string]$QtHash = "7c7f985711d80467bdc1795b6592275a27d5b0e5a2c7a61db1f2c1d08d6a5579"
)
$ErrorActionPreference = "Stop"
$root = Split-Path $PSScriptRoot -Parent
$deps = Join-Path $root ".deps"
New-Item -ItemType Directory -Force $deps | Out-Null

$obs = Join-Path $deps "obs-studio"
if (-not (Test-Path "$obs\libobs\obs.h")) {
    Write-Host "Fetching OBS $ObsVersion headers..."
    git clone --depth 1 --branch $ObsVersion --filter=blob:none --sparse https://github.com/obsproject/obs-studio.git $obs
    git -C $obs sparse-checkout set libobs deps/w32-pthreads frontend/api
}

$qt = Join-Path $deps "qt6"
if (-not (Test-Path "$qt\lib\cmake\Qt6\Qt6Config.cmake")) {
    $zip = Join-Path $deps "qt6.zip"
    $url = "https://github.com/obsproject/obs-deps/releases/download/$DepsVersion/windows-deps-qt6-$DepsVersion-x64.zip"
    Write-Host "Downloading Qt package $DepsVersion..."
    Invoke-WebRequest -Uri $url -OutFile $zip -UseBasicParsing
    $hash = (Get-FileHash $zip -Algorithm SHA256).Hash.ToLower()
    if ($hash -ne $QtHash) { Remove-Item $zip; throw "Qt package hash mismatch: $hash" }
    Expand-Archive $zip -DestinationPath $qt -Force
    Remove-Item $zip
}
Write-Host "Dependencies ready in $deps"
