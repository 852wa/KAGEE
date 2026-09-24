; Kagee installer (Inno Setup 6). Built by tools/package.ps1:
;   ISCC /DAppVersion=x.y.z /DStageDir=<dir with bin/ and data/> installer\kagee.iss
#ifndef AppVersion
  #define AppVersion "0.0.0"
#endif
#ifndef StageDir
  #define StageDir "..\dist\stage\kagee"
#endif

[Setup]
AppId={{6C1F5E0A-4B7D-4E3A-9C58-0B6E1D2A7F41}
AppName=Kagee
AppVersion={#AppVersion}
AppVerName=Kagee {#AppVersion}
AppPublisher=hakoniwa
AppPublisherURL=https://github.com/852wa/KAGEE
AppSupportURL=https://github.com/852wa/KAGEE/issues
DefaultDirName={commonappdata}\obs-studio\plugins\kagee
DisableDirPage=yes
DisableProgramGroupPage=yes
PrivilegesRequired=admin
PrivilegesRequiredOverridesAllowed=commandline
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
OutputDir=..\dist
OutputBaseFilename=Kagee-{#AppVersion}-windows-x64-setup
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
LicenseFile=..\LICENSE
UninstallDisplayName=Kagee {#AppVersion} (OBS Studio plugin)
VersionInfoVersion={#AppVersion}
VersionInfoDescription=Kagee OBS Studio plugin installer

[Languages]
Name: "japanese"; MessagesFile: "compiler:Languages\Japanese.isl"
Name: "english"; MessagesFile: "compiler:Default.isl"

[Files]
Source: "{#StageDir}\*"; DestDir: "{app}"; Flags: recursesubdirs createallsubdirs ignoreversion

[UninstallDelete]
Type: filesandordirs; Name: "{app}"

[Messages]
japanese.FinishedLabel=インストールが完了しました。%n%nOBS Studio を起動し、メニュー「ドック」→「Kagee」でパネルを表示してください。
english.FinishedLabel=Installation complete.%n%nStart OBS Studio and open the panel from the menu: Docks -> Kagee.

[CustomMessages]
japanese.ObsRunning=OBS Studio が起動しています。%nOBS を終了してから「OK」を押してください。
english.ObsRunning=OBS Studio is running.%nPlease close OBS, then press OK.
japanese.ObsTooOld=インストールされている OBS Studio のバージョン (%1) では Kagee は動作しません。%nOBS Studio 32.2 以降に更新してください。%n%nこのままインストールを続けますか？
english.ObsTooOld=Kagee does not work with the installed OBS Studio version (%1).%nPlease update OBS Studio to 32.2 or later.%n%nContinue installing anyway?
japanese.ObsNotFound=OBS Studio が見つかりませんでした。%n先に OBS Studio 32.2 以降をインストールしてください。%n%nこのまま続けますか？
english.ObsNotFound=OBS Studio was not found.%nPlease install OBS Studio 32.2 or later first.%n%nContinue anyway?

[Code]
function IsObsRunning(): Boolean;
var
  Locator, Service, Found: Variant;
begin
  Result := False;
  try
    Locator := CreateOleObject('WbemScripting.SWbemLocator');
    Service := Locator.ConnectServer('.', 'root\CIMV2');
    Found := Service.ExecQuery('SELECT ProcessId FROM Win32_Process WHERE Name = ''obs64.exe''');
    Result := Found.Count > 0;
  except
    Result := False;
  end;
end;

{ /SKIPOBSCHECK=1 on the command line skips the OBS checks (automated tests / CI) }
function SkipObsCheck(): Boolean;
begin
  Result := ExpandConstant('{param:SKIPOBSCHECK|0}') = '1';
end;

function WaitForObsClosed(): Boolean;
begin
  Result := True;
  if SkipObsCheck() then
    exit;
  while IsObsRunning() do
  begin
    if WizardSilent() or UninstallSilent() then
    begin
      { a suppressed message box would answer OK forever: abort instead }
      Log('OBS Studio is running; aborting silent run.');
      Result := False;
      exit;
    end;
    if SuppressibleMsgBox(CustomMessage('ObsRunning'), mbInformation, MB_OKCANCEL, IDOK) = IDCANCEL then
    begin
      Result := False;
      exit;
    end;
  end;
end;

function CheckObsVersion(): Boolean;
var
  Dir, Exe: String;
  MS, LS: Cardinal;
  Major, Minor: Integer;
begin
  Result := True;
  if SkipObsCheck() then
    exit;
  if not RegQueryStringValue(HKLM64, 'SOFTWARE\OBS Studio', '', Dir) then
  begin
    Result := SuppressibleMsgBox(CustomMessage('ObsNotFound'), mbConfirmation, MB_YESNO, IDYES) = IDYES;
    exit;
  end;
  Exe := AddBackslash(Dir) + 'bin\64bit\obs64.exe';
  if not GetVersionNumbers(Exe, MS, LS) then
    exit;
  Major := MS shr 16;
  Minor := MS and $FFFF;
  if (Major < 32) or ((Major = 32) and (Minor < 2)) then
    Result := SuppressibleMsgBox(FmtMessage(CustomMessage('ObsTooOld'), [IntToStr(Major) + '.' + IntToStr(Minor)]),
                                 mbConfirmation, MB_YESNO, IDNO) = IDYES;
end;

function InitializeSetup(): Boolean;
begin
  Result := WaitForObsClosed() and CheckObsVersion();
end;

function InitializeUninstall(): Boolean;
begin
  Result := WaitForObsClosed();
end;
