# Kagee: commit all changes and push to GitHub (852wa/KAGEE); optionally tag a release.
# Normally started by push.bat. Parameters allow non-interactive use:
#   -Message "text"  commit message    -Release  also tag v<version>    -NoPause
param([string]$Message, [switch]$Release, [switch]$NoRelease, [switch]$NoPause)

$PushUser = '852wa'
$Repo = 'https://github.com/852wa/KAGEE'
$root = Split-Path $PSScriptRoot -Parent
Set-Location $root

function Stop-Script {
    Write-Host ''
    if (-not $NoPause) { $null = Read-Host 'Enter キーで閉じます' }
    exit
}
function Git-Push([string[]]$refs) {
    & git -c credential.helper= -c 'credential.helper=!gh auth git-credential' push @refs
    return $LASTEXITCODE -eq 0
}

foreach ($tool in 'git', 'gh') {
    if (-not (Get-Command $tool -ErrorAction SilentlyContinue)) {
        Write-Host "[エラー] $tool が見つかりません。" -ForegroundColor Red
        Stop-Script
    }
}

Write-Host '=== 変更されたファイル ===' -ForegroundColor Cyan
$status = git status --porcelain
if ($status) { git status --short } else { Write-Host '（変更はありません）' }
Write-Host ''

# --- commit ---
if ($status) {
    if (-not $Message) { $Message = Read-Host 'コミットメッセージを入力してください（空欄なら "update"）' }
    if (-not $Message) { $Message = 'update' }
    git add -A
    git commit -q -m $Message
    if ($LASTEXITCODE) { Write-Host '[エラー] コミットに失敗しました。' -ForegroundColor Red; Stop-Script }
    Write-Host "コミットしました: $Message"
}

# --- push as $PushUser (switch the gh account temporarily if needed) ---
$prevUser = (gh api user --jq .login 2>$null)
$switched = $false
if ($prevUser -ne $PushUser) {
    gh auth switch -h github.com -u $PushUser *> $null
    if ($LASTEXITCODE) {
        Write-Host "[エラー] GitHub アカウント $PushUser でログインしていません。" -ForegroundColor Red
        Write-Host "        ターミナルで  gh auth login  を実行し、$PushUser でログインしてください。"
        Stop-Script
    }
    $switched = $true
}

try {
    Write-Host ''
    Write-Host '=== GitHub へ送信中 ===' -ForegroundColor Cyan
    if (-not (Git-Push @('-u', 'origin', 'HEAD'))) {
        Write-Host '[エラー] 送信に失敗しました。' -ForegroundColor Red
        return
    }
    Write-Host "送信しました: $Repo" -ForegroundColor Green

    # --- optional release ---
    $ver = [regex]::Match((Get-Content CMakeLists.txt -Raw), 'project\(kagee VERSION ([0-9.]+)').Groups[1].Value
    if (-not $Release -and -not $NoRelease) {
        Write-Host ''
        $answer = Read-Host "バージョン $ver のリリースも作りますか？ (y/N)"
        $Release = $answer -match '^[yYｙＹ]'
    }
    if ($Release) {
        git rev-parse -q --verify "refs/tags/v$ver" *> $null
        if ($LASTEXITCODE -eq 0) {
            Write-Host "[注意] v$ver は既に作成済みです。" -ForegroundColor Yellow
            Write-Host "       CMakeLists.txt の 'project(kagee VERSION $ver ...' の番号を上げてから、もう一度実行してください。"
            return
        }
        git tag -a "v$ver" -m "Kagee $ver"
        if (-not (Git-Push @('origin', "v$ver"))) {
            Write-Host '[エラー] タグの送信に失敗しました。' -ForegroundColor Red
            return
        }
        Write-Host ''
        Write-Host "v$ver を送信しました。数分後に Releases に下書きが作られます:" -ForegroundColor Green
        Write-Host "  $Repo/releases"
        Write-Host '内容を確認して「Publish release」を押すと公開されます。'
    }
} finally {
    if ($switched -and $prevUser) { gh auth switch -h github.com -u $prevUser *> $null }
}
Stop-Script
