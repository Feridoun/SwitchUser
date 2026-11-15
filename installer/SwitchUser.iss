; SwitchUser installer (Inno Setup 6.2+). Built by tools\release.ps1, which
; passes the version and, for signed builds, the "certum" sign tool:
;   ISCC /DMyAppVersion=1.1.0 [/DSignBuild=1 /Scertum=...] installer\SwitchUser.iss
;
; Installs for all users into Program Files (so a standard user can't replace
; the program the SYSTEM service runs), then has SwitchUser.exe register the
; service and the HKLM Run entry itself. Uninstalling removes all of it.

#ifndef MyAppVersion
#define MyAppVersion "0.0.0"
#endif
#define MyAppName "SwitchUser"
#define MyAppPublisher "Feridoun"
#define MyAppURL "https://github.com/Feridoun/SwitchUser"
#define MyAppExeName "SwitchUser.exe"

[Setup]
AppId={{D2F9388A-5967-47E9-A3FB-5FFBAF6278EC}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppVerName={#MyAppName} {#MyAppVersion}
AppPublisher={#MyAppPublisher}
AppPublisherURL={#MyAppURL}
AppSupportURL={#MyAppURL}/issues
AppUpdatesURL={#MyAppURL}/releases
VersionInfoVersion={#MyAppVersion}
VersionInfoProductVersion={#MyAppVersion}
VersionInfoCompany={#MyAppPublisher}
VersionInfoDescription={#MyAppName} Setup
VersionInfoProductName={#MyAppName}
; The same folder "SwitchUser.exe install" uses, so either way of installing
; upgrades the other.
DefaultDirName={autopf}\{#MyAppName}
DisableDirPage=yes
DisableProgramGroupPage=yes
PrivilegesRequired=admin
OutputDir=..\build\dist
OutputBaseFilename=SwitchUser-Setup-{#MyAppVersion}
Compression=lzma2/ultra64
SolidCompression=yes
MinVersion=10.0
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
WizardStyle=modern
LicenseFile=..\LICENSE
InfoBeforeFile=before-install.txt
UninstallDisplayIcon={app}\{#MyAppExeName}
UninstallDisplayName={#MyAppName}
; The service and every user's tray icon are stopped in [Code] below.
CloseApplications=no
#ifdef SignBuild
SignTool=certum
SignedUninstaller=yes
#endif

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Files]
Source: "..\{#MyAppExeName}"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\LICENSE"; DestDir: "{app}"; DestName: "LICENSE.txt"; Flags: ignoreversion

[Run]
; Creates and starts SwitchUserSvc and adds the HKLM Run entry; the service
; then starts the tray icon in every session that's signed in.
Filename: "{app}\{#MyAppExeName}"; Parameters: "install-elevated"; StatusMsg: "Starting the SwitchUser service..."; \
    Flags: runhidden waituntilterminated

[UninstallRun]
; Removes the service and the Run entry, then closes every user's tray icon so
; the files can be deleted.
Filename: "{app}\{#MyAppExeName}"; Parameters: "unregister"; Flags: runhidden waituntilterminated; RunOnceId: "Unregister"
Filename: "{sys}\taskkill.exe"; Parameters: "/F /IM {#MyAppExeName}"; Flags: runhidden waituntilterminated; RunOnceId: "StopTrayIcons"

[UninstallDelete]
; Left by "SwitchUser.exe install" when it replaced a running copy.
Type: files; Name: "{app}\{#MyAppExeName}.old"
Type: filesandordirs; Name: "{commonappdata}\SwitchUser"

[Code]
// Stops the service and every user's tray icon, so the program can be replaced.
// The new version re-creates the service after copying the files.
function PrepareToInstall(var NeedsRestart: Boolean): String;
var
  Code: Integer;
begin
  Result := '';
  Exec(ExpandConstant('{sys}\sc.exe'), 'stop SwitchUserSvc', '', SW_HIDE, ewWaitUntilTerminated, Code);
  Sleep(1500);
  Exec(ExpandConstant('{sys}\taskkill.exe'), '/F /IM {#MyAppExeName}', '', SW_HIDE, ewWaitUntilTerminated, Code);
  Sleep(500);
end;

// The tray icons closed by [UninstallRun] can hold the folder open for a
// moment after the files are gone, so keep trying to remove it for a while.
procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
  Dir: String;
  Tries: Integer;
begin
  if CurUninstallStep <> usPostUninstall then Exit;
  Dir := ExpandConstant('{app}');
  Tries := 0;
  while DirExists(Dir) and (Tries < 20) do begin
    RemoveDir(Dir);
    if DirExists(Dir) then Sleep(500);
    Tries := Tries + 1;
  end;
end;
