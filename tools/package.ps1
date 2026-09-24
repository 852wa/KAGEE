# Builds Kagee and creates the release packages in dist/:
#   Kagee-<ver>-windows-x64-setup.exe  (installer)
#   Kagee-<ver>-windows-x64.zip        (install.bat / uninstall.bat)
#   SHA256SUMS.txt
param([switch]$SkipBuild)
$ErrorActionPreference = "Stop"
$root = Split-Path $PSScriptRoot -Parent
$ver = [regex]::Match((Get-Content "$root\CMakeLists.txt" -Raw), 'project\(kagee VERSION ([0-9.]+)').Groups[1].Value
Write-Host "Kagee $ver"

if (-not $SkipBuild) {
    & "$PSScriptRoot\fetch-deps.ps1"
    cmake -S $root -B "$root\build" -G "Visual Studio 17 2022" -A x64
    if ($LASTEXITCODE) { throw "configure failed" }
    cmake --build "$root\build" --config Release
    if ($LASTEXITCODE) { throw "build failed" }
}

$dist = Join-Path $root "dist"
if (Test-Path $dist) { Remove-Item $dist -Recurse -Force }
$stage = Join-Path $dist "stage\kagee"
New-Item -ItemType Directory -Force "$stage\bin\64bit", "$stage\data" | Out-Null
Copy-Item "$root\build\Release\kagee.dll" "$stage\bin\64bit\"
Copy-Item "$root\data\*" "$stage\data\" -Recurse

# zip package
$name = "Kagee-$ver-windows-x64"
$zipDir = Join-Path $dist "zip\$name"
New-Item -ItemType Directory -Force "$zipDir\files" | Out-Null
Copy-Item "$root\installer\zip\*.bat", "$root\installer\zip\README.txt" $zipDir
Copy-Item "$root\installer\zip\files\install.ps1" "$zipDir\files\"
Copy-Item $stage "$zipDir\files\kagee" -Recurse
Copy-Item "$root\LICENSE" "$zipDir\LICENSE.txt"
Compress-Archive -Path $zipDir -DestinationPath "$dist\$name.zip"

# installer
$iscc = @("$env:LOCALAPPDATA\Programs\Inno Setup 6\ISCC.exe", "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe",
          "$env:ProgramFiles\Inno Setup 6\ISCC.exe") | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $iscc) { $iscc = (Get-Command ISCC -ErrorAction SilentlyContinue).Source }
if (-not $iscc) { throw "Inno Setup 6 (ISCC.exe) not found" }
& $iscc /Q "/DAppVersion=$ver" "/DStageDir=$stage" "$root\installer\kagee.iss"
if ($LASTEXITCODE) { throw "installer build failed" }

Get-ChildItem $dist -File | ForEach-Object {
    "{0}  {1}" -f (Get-FileHash $_.FullName -Algorithm SHA256).Hash.ToLower(), $_.Name
} | Set-Content "$dist\SHA256SUMS.txt" -Encoding ascii
Get-ChildItem $dist -File | Select-Object Name, Length
