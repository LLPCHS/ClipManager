
#define MyAppName "ClipManager"
#define MyAppVersion "1.0.0"
#define MyAppExeName "ClipManager.exe"

[Setup]
AppId={{6C89B1D5-3A67-4E6F-9AC7-2B0F16F34B71}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
DefaultDirName={localappdata}\Programs\ClipManager
DefaultGroupName=ClipManager
PrivilegesRequired=lowest
OutputDir=Output
OutputBaseFilename=ClipManager-Setup
SetupIconFile=ClipManager32px.ico
UninstallDisplayIcon={app}\ClipManager.exe
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
WizardStyle=modern

[Files]
Source: "D:\M\ClipManager\x64\Release\ClipManager.exe"; DestDir: "{app}"; Flags: ignoreversion

[Tasks]
Name: "desktopicon"; Description: "Create a desktop shortcut"; GroupDescription: "Additional tasks:"; Flags: unchecked

[Icons]
Name: "{group}\ClipManager"; Filename: "{app}\ClipManager.exe"; WorkingDir: "{app}"
Name: "{autodesktop}\ClipManager"; Filename: "{app}\ClipManager.exe"; WorkingDir: "{app}"; Tasks: desktopicon

[Registry]
Root: HKCU; Subkey: "Software\Microsoft\Windows\CurrentVersion\Run"; ValueType: none; ValueName: "ClipManager"; Flags: uninsdeletevalue dontcreatekey

[Run]
Filename: "{app}\ClipManager.exe"; Description: "Launch ClipManager"; Flags: postinstall nowait skipifsilent
