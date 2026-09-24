# Kagee zip installer (run via install.bat / uninstall.bat)
param([switch]$Uninstall, [string]$Destination, [switch]$Quiet) # -Destination/-Quiet: automated tests

$ja = (Get-Culture).Name -like 'ja*'
function Say([string]$jp, [string]$en) { if ($ja) { Write-Host $jp } else { Write-Host $en } }
function Finish {
    Write-Host ''
    if (-not $Quiet) {
        if ($ja) { $null = Read-Host 'Enter キーで閉じます' } else { $null = Read-Host 'Press Enter to close' }
    }
    exit
}

$src = Join-Path $PSScriptRoot 'kagee'
$dest = if ($Destination) { $Destination } else { Join-Path $env:ProgramData 'obs-studio\plugins\kagee' }

Write-Host '==== Kagee ===='
while (-not $Quiet -and (Get-Process obs64 -ErrorAction SilentlyContinue)) {
    Say 'OBS Studio が起動しています。OBS を終了してから Enter キーを押してください。' `
        'OBS Studio is running. Close OBS, then press Enter.'
    Read-Host | Out-Null
}

if (-not $Uninstall) {
    try {
        $dir = (Get-ItemProperty 'HKLM:\SOFTWARE\OBS Studio' -ErrorAction Stop).'(default)'
        $v = (Get-Item (Join-Path $dir 'bin\64bit\obs64.exe') -ErrorAction Stop).VersionInfo
        if ($v.FileMajorPart -lt 32 -or ($v.FileMajorPart -eq 32 -and $v.FileMinorPart -lt 2)) {
            Say "注意: OBS Studio $($v.FileMajorPart).$($v.FileMinorPart) では動作しません。OBS Studio 32.2 以降に更新してください。" `
                "Warning: OBS Studio $($v.FileMajorPart).$($v.FileMinorPart) is not supported. Please update to 32.2 or later."
        }
    } catch {
        Say '注意: OBS Studio が見つかりませんでした（OBS Studio 32.2 以降が必要です）。' `
            'Warning: OBS Studio was not found (OBS Studio 32.2 or later is required).'
    }
}

try {
    if ($Uninstall) {
        if (Test-Path $dest) { Remove-Item $dest -Recurse -Force -ErrorAction Stop }
    } else {
        New-Item -ItemType Directory -Force $dest -ErrorAction Stop | Out-Null
        Copy-Item (Join-Path $src '*') $dest -Recurse -Force -ErrorAction Stop
    }
} catch {
    $isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole(
        [Security.Principal.WindowsBuiltInRole]::Administrator)
    if (-not $isAdmin) {
        Say '管理者の許可が必要です。表示される確認画面で「はい」を押してください。' `
            'Administrator permission is needed. Please press "Yes" in the prompt.'
        $argList = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', "`"$PSCommandPath`"")
        if ($Uninstall) { $argList += '-Uninstall' }
        Start-Process powershell -Verb RunAs -ArgumentList $argList
        exit
    }
    Say "エラー: $($_.Exception.Message)" "Error: $($_.Exception.Message)"
    Finish
}

if ($Uninstall) {
    Say 'Kagee をアンインストールしました。' 'Kagee has been uninstalled.'
} else {
    Say "インストールが完了しました（$dest）。" "Installed to $dest."
    Say 'OBS Studio を起動し、メニュー「ドック」→「Kagee」でパネルを表示してください。' `
        'Start OBS Studio and open the panel from: Docks -> Kagee.'
}
Finish
