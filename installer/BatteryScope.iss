; BatteryScope installer (Inno Setup 6.5 or later).
;
; Installs per user, without administrator rights, always into the same folder
; (%LOCALAPPDATA%\Programs\BatteryScope). Because the path never changes between
; versions, shortcuts, taskbar and Start pins, and launch at sign-in keep
; pointing at the latest version after every update.
;
; Before copying files it closes any running BatteryScope (installed or portable,
; any version): first cleanly with --quit-running, which lets it undo its
; efficiency mode and power mode changes, then by force. After copying it runs
; --migrate-shortcuts, which repoints shortcuts, pins and the sign-in entry that
; still open an older BatteryScope.exe elsewhere (for example a portable copy).
;
; Build: iscc /DAppVersion=0.12.0 /DSourceExe=..\build\Release\BatteryScope.exe installer\BatteryScope.iss

#ifndef AppVersion
  #error Pass /DAppVersion=x.y.z
#endif
#ifndef SourceExe
  #define SourceExe "..\build\Release\BatteryScope.exe"
#endif
#ifndef OutputDir
  #define OutputDir "..\dist"
#endif

[Setup]
AppId={{42626793-DF4C-45CC-9FB8-5085C3BE2A76}
AppName=BatteryScope
AppVersion={#AppVersion}
AppVerName=BatteryScope {#AppVersion}
AppPublisher=rio10255254
AppPublisherURL=https://github.com/rio10255254/BatteryScope
AppSupportURL=https://github.com/rio10255254/BatteryScope/issues
AppUpdatesURL=https://github.com/rio10255254/BatteryScope/releases/latest
VersionInfoVersion={#AppVersion}.0
DefaultDirName={autopf}\BatteryScope
; Per user only, so the folder is the same on every install and update.
PrivilegesRequired=lowest
DisableDirPage=yes
DisableProgramGroupPage=yes
UsePreviousAppDir=yes
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
; Windows 10 1703, the same minimum as the program.
MinVersion=10.0.15063
OutputDir={#OutputDir}
OutputBaseFilename=BatteryScope-{#AppVersion}-setup
SetupIconFile=..\src\assets\batteryscope.ico
UninstallDisplayIcon={app}\BatteryScope.exe
UninstallDisplayName=BatteryScope
WizardStyle=modern
Compression=lzma2
SolidCompression=yes
CloseApplications=yes
RestartApplications=no
ShowLanguageDialog=auto

[Languages]
Name: "chinesetraditional"; MessagesFile: "ChineseTraditional.isl"
Name: "english"; MessagesFile: "compiler:Default.isl"

[CustomMessages]
chinesetraditional.DesktopIcon=建立桌面捷徑
english.DesktopIcon=Create a desktop shortcut
chinesetraditional.StartAtSignIn=登入 Windows 時自動啟動（在系統匣執行）
english.StartAtSignIn=Start when I sign in to Windows (runs in the notification area)
chinesetraditional.LaunchNow=開啟 BatteryScope
english.LaunchNow=Open BatteryScope

[Tasks]
Name: "desktopicon"; Description: "{cm:DesktopIcon}"; Flags: unchecked
Name: "startatsignin"; Description: "{cm:StartAtSignIn}"; Flags: unchecked

[Files]
Source: "{#SourceExe}"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\src\assets\pixel-ui-OFL.txt"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{autoprograms}\BatteryScope"; Filename: "{app}\BatteryScope.exe"
Name: "{autodesktop}\BatteryScope"; Filename: "{app}\BatteryScope.exe"; Tasks: desktopicon

[Registry]
; The program reads this to know it is the installed copy.
Root: HKCU; Subkey: "Software\BatteryScope"; ValueType: string; ValueName: "InstallPath"; ValueData: "{app}"; Flags: uninsdeletevalue
Root: HKCU; Subkey: "Software\Microsoft\Windows\CurrentVersion\Run"; ValueType: string; ValueName: "BatteryScope"; ValueData: """{app}\BatteryScope.exe"" --tray"; Tasks: startatsignin

[Run]
Filename: "{app}\BatteryScope.exe"; Parameters: "--migrate-shortcuts"; Flags: runhidden waituntilterminated
; Also runs after a silent update started from the program, so it reopens itself.
Filename: "{app}\BatteryScope.exe"; Description: "{cm:LaunchNow}"; Flags: nowait postinstall

[UninstallRun]
Filename: "{app}\BatteryScope.exe"; Parameters: "--quit-running"; Flags: runhidden waituntilterminated; RunOnceId: "QuitBatteryScope"

[Code]
// Close every running BatteryScope before files are replaced: cleanly first,
// then by force for older versions or copies that did not respond.
function PrepareToInstall(var NeedsRestart: Boolean): String;
var
  ResultCode: Integer;
begin
  ExtractTemporaryFile('BatteryScope.exe');
  Exec(ExpandConstant('{tmp}\BatteryScope.exe'), '--quit-running', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
  Exec(ExpandConstant('{sys}\taskkill.exe'), '/F /IM BatteryScope.exe', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
  Sleep(500);
  Result := '';
end;

// Remove launch at sign-in only if it points at this installation.
procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
  Command: String;
begin
  if CurUninstallStep = usUninstall then
    if RegQueryStringValue(HKCU, 'Software\Microsoft\Windows\CurrentVersion\Run', 'BatteryScope', Command) then
      if Pos(Lowercase(ExpandConstant('{app}')), Lowercase(Command)) > 0 then
        RegDeleteValue(HKCU, 'Software\Microsoft\Windows\CurrentVersion\Run', 'BatteryScope');
end;
