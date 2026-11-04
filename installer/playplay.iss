; PLAY PLAY installer (Inno Setup 6). Built by scripts\package.ps1, which passes AppVersion, SourceDir, OutputDir.
; The package bundles third-party runtimes (Qt, a GPL FFmpeg build, optionally libobs and the NDI runtime). Before distributing a
; setup .exe, add their licence texts and a source offer (see THIRD_PARTY_NOTICES.md) and set LicenseFile= for your own licence.
#ifndef AppVersion
  #define AppVersion "3.0.0"
#endif
#ifndef SourceDir
  #define SourceDir "..\dist\PlayPlay-" + AppVersion
#endif
#ifndef OutputDir
  #define OutputDir "..\dist"
#endif

[Setup]
AppId={{4168D2FC-9F01-496B-AB7C-C0338C429E83}
AppName=PLAY PLAY
AppVersion={#AppVersion}
AppPublisher=PLAY PLAY
DefaultDirName={autopf}\PlayPlay
DefaultGroupName=PLAY PLAY
OutputDir={#OutputDir}
OutputBaseFilename=PlayPlay-{#AppVersion}-setup
Compression=lzma2/max
SolidCompression=yes
ArchitecturesInstallIn64BitMode=x64compatible
PrivilegesRequired=admin
WizardStyle=modern
UninstallDisplayIcon={app}\PlayPlay.exe
SetupIconFile=..\branding\play-play.ico

[Tasks]
Name: "desktopicon"; Description: "Create a desktop shortcut"; GroupDescription: "Shortcuts:"
Name: "service"; Description: "Install the Supervisor as a Windows service (starts with the machine, keeps playout running without the app)"; GroupDescription: "Automation host:"
Name: "firewall"; Description: "Open TCP 7700 (REST, web dashboard) and 7701 (WebSocket) in Windows Firewall"; GroupDescription: "Network:"

[Files]
Source: "{#SourceDir}\*"; DestDir: "{app}"; Flags: recursesubdirs ignoreversion

[Dirs]
Name: "{commonappdata}\PlayPlay\supervisor"; Permissions: users-modify
Name: "{commonappdata}\PlayPlay\supervisor\media\uploads"; Permissions: users-modify

[Icons]
Name: "{group}\PLAY PLAY"; Filename: "{app}\PlayPlay.exe"
Name: "{group}\PLAY PLAY Dashboard"; Filename: "http://localhost:7700/"
Name: "{group}\Uninstall PLAY PLAY"; Filename: "{uninstallexe}"
Name: "{autodesktop}\PLAY PLAY"; Filename: "{app}\PlayPlay.exe"; Tasks: desktopicon

[Run]
Filename: "{app}\PlayPlaySupervisor.exe"; Parameters: "--install-service --data-dir ""{commonappdata}\PlayPlay\supervisor"""; StatusMsg: "Installing the Supervisor service..."; Flags: runhidden; Tasks: service
Filename: "netsh"; Parameters: "advfirewall firewall add rule name=""PLAY PLAY Supervisor"" dir=in action=allow protocol=TCP localport=7700,7701"; Flags: runhidden; Tasks: firewall
Filename: "{app}\PlayPlay.exe"; Description: "Launch PLAY PLAY"; Flags: nowait postinstall skipifsilent

[UninstallRun]
Filename: "{app}\PlayPlaySupervisor.exe"; Parameters: "--uninstall-service"; Flags: runhidden; RunOnceId: "svc"
Filename: "netsh"; Parameters: "advfirewall firewall delete rule name=""PLAY PLAY Supervisor"""; Flags: runhidden; RunOnceId: "fw"
