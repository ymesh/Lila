; Lila installer: per-user, no admin. Built by build.ps1, which stages the install and passes the defines below.
; With PatchFrom, PatchDir and PatchDeletes defined it builds a patch instead: only the files that changed since that
; version, and the files it dropped, applied over an existing install of exactly that version.
#ifndef AppVersion
  #error Build with scripts\installer\build.ps1
#endif

[Setup]
AppId={{6B1E7C2A-4F3D-4A8E-9C51-2D7E8B0F4A13}
AppName=Lila
AppVersion={#AppVersion}
AppVerName=Lila {#AppVersion}
AppPublisher=Jared Taylor
AppPublisherURL=https://lila-3d.github.io/
AppSupportURL=https://lila-3d.github.io/docs/#/support
DefaultDirName={autopf}\Lila
DefaultGroupName=Lila
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=commandline dialog
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0
LicenseFile={#SourceDir}\LICENSE
SetupIconFile={#RepoDir}\resources\icons\lila.ico
UninstallDisplayIcon={app}\lila.exe
OutputDir={#OutputDir}
#ifdef PatchFrom
OutputBaseFilename=Lila-{#AppVersion}-Patch-{#PatchFrom}
DisableDirPage=yes
#else
OutputBaseFilename=Lila-{#AppVersion}-Setup
#endif
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
CloseApplications=yes
RestartApplications=no
; Lila checks for this and waits instead of starting on half-replaced files.
SetupMutex=LilaSetup
ChangesAssociations=yes

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[Registry]
; The values Lila's own file type code (src/io/FileTypes.cpp) writes, so the app and the uninstaller agree on what is Lila's.
Root: HKA; Subkey: "Software\Classes\Lila.lila"; ValueType: string; ValueName: ""; ValueData: "Lila scene"; Flags: uninsdeletekey
Root: HKA; Subkey: "Software\Classes\Lila.lila"; ValueType: string; ValueName: "FriendlyTypeName"; ValueData: "Lila scene"
Root: HKA; Subkey: "Software\Classes\Lila.lila\DefaultIcon"; ValueType: string; ValueName: ""; ValueData: "{app}\lila.exe,0"
Root: HKA; Subkey: "Software\Classes\Lila.lila\shell\open\command"; ValueType: string; ValueName: ""; ValueData: """{app}\lila.exe"" ""%1"""
Root: HKA; Subkey: "Software\Classes\.lila"; ValueType: string; ValueName: ""; ValueData: "Lila.lila"; Flags: uninsdeletevalue
Root: HKA; Subkey: "Software\Classes\.lila\OpenWithProgids"; ValueType: string; ValueName: "Lila.lila"; ValueData: ""; Flags: uninsdeletevalue
Root: HKA; Subkey: "Software\Classes\Lila.lilaa"; ValueType: string; ValueName: ""; ValueData: "Lila text scene"; Flags: uninsdeletekey
Root: HKA; Subkey: "Software\Classes\Lila.lilaa"; ValueType: string; ValueName: "FriendlyTypeName"; ValueData: "Lila text scene"
Root: HKA; Subkey: "Software\Classes\Lila.lilaa\DefaultIcon"; ValueType: string; ValueName: ""; ValueData: "{app}\lila.exe,0"
Root: HKA; Subkey: "Software\Classes\Lila.lilaa\shell\open\command"; ValueType: string; ValueName: ""; ValueData: """{app}\lila.exe"" ""%1"""
Root: HKA; Subkey: "Software\Classes\.lilaa"; ValueType: string; ValueName: ""; ValueData: "Lila.lilaa"; Flags: uninsdeletevalue
Root: HKA; Subkey: "Software\Classes\.lilaa\OpenWithProgids"; ValueType: string; ValueName: "Lila.lilaa"; ValueData: ""; Flags: uninsdeletevalue
Root: HKA; Subkey: "Software\Classes\Applications\lila.exe"; ValueType: string; ValueName: "FriendlyAppName"; ValueData: "Lila"; Flags: uninsdeletekey
Root: HKA; Subkey: "Software\Classes\Applications\lila.exe\shell\open\command"; ValueType: string; ValueName: ""; ValueData: """{app}\lila.exe"" ""%1"""

[Files]
#ifdef PatchFrom
Source: "{#PatchDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs skipifsourcedoesntexist
#else
Source: "{#SourceDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs
#endif

[InstallDelete]
#ifdef PatchFrom
#include PatchDeletes
#else
; A new version replaces the bundled Python and resources wholesale, so files removed upstream don't linger.
Type: filesandordirs; Name: "{app}\python-runtime"
Type: filesandordirs; Name: "{app}\python-site"
Type: filesandordirs; Name: "{app}\python"
Type: filesandordirs; Name: "{app}\resources"
#endif

[Icons]
Name: "{autoprograms}\Lila"; Filename: "{app}\lila.exe"
Name: "{autodesktop}\Lila"; Filename: "{app}\lila.exe"; Tasks: desktopicon

[Run]
; Setup still holds its mutex here, which would make Lila think an install is under way and quit.
Filename: "{app}\lila.exe"; Parameters: "--from-setup"; Description: "{cm:LaunchProgram,Lila}"; Flags: nowait postinstall skipifsilent
Filename: "{app}\lila.exe"; Parameters: "--from-setup"; Flags: nowait; Check: RelaunchRequested

[UninstallRun]
; Hands back the import formats ticked in Preferences, which only Lila knows about.
Filename: "{app}\lila.exe"; Parameters: "--batch -c ""unregisterFileTypes()"""; Flags: runhidden waituntilterminated; RunOnceId: "LilaFileTypes"

[UninstallDelete]
Type: filesandordirs; Name: "{app}\python-runtime"
Type: filesandordirs; Name: "{app}\python-site"
Type: filesandordirs; Name: "{app}\python"

[Code]
{ Lila leaves this flag when someone opened it while a silent update was installing. }
function RelaunchRequested: Boolean;
var
  Flag: String;
begin
  Flag := GetEnv('TEMP') + '\LilaUpdate\relaunch';
  Result := WizardSilent and FileExists(Flag);
  if Result then
    DeleteFile(Flag);
end;

function OpenProcess(Access: Cardinal; Inherit: Cardinal; Pid: Cardinal): THandle; external 'OpenProcess@kernel32.dll stdcall';
function WaitForSingleObject(Handle: THandle; Ms: Cardinal): Cardinal; external 'WaitForSingleObject@kernel32.dll stdcall';
function CloseHandle(Handle: THandle): Cardinal; external 'CloseHandle@kernel32.dll stdcall';

#ifdef PatchFrom
{ The version of the install this patch would update, in the scope it runs in: HKA is HKLM for all users, else HKCU. }
function InstalledVersion: String;
begin
  if not RegQueryStringValue(HKA, 'Software\Microsoft\Windows\CurrentVersion\Uninstall\{6B1E7C2A-4F3D-4A8E-9C51-2D7E8B0F4A13}_is1', 'DisplayVersion', Result) then
    Result := '';
end;
#endif

{ An update started by a closing Lila passes its pid, so files aren't replaced while it is still exiting. }
function InitializeSetup: Boolean;
var
  Pid: Cardinal;
  Process: THandle;
begin
#ifdef PatchFrom
  if InstalledVersion <> '{#PatchFrom}' then
  begin
    SuppressibleMsgBox('This patch updates Lila {#PatchFrom} only. Download the full Lila {#AppVersion} installer instead.', mbError, MB_OK, IDOK);
    Result := False;
    exit;
  end;
#endif
  Pid := StrToIntDef(ExpandConstant('{param:WAITPID|0}'), 0);
  if Pid <> 0 then
  begin
    Process := OpenProcess($00100000, 0, Pid);
    if Process <> 0 then
    begin
      WaitForSingleObject(Process, 60000);
      CloseHandle(Process);
    end;
  end;
  Result := True;
end;
