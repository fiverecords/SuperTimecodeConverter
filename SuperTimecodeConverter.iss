; Super Timecode Converter -- Inno Setup Script
; Copyright (c) 2026 Fiverecords -- MIT License
; https://github.com/fiverecords/SuperTimecodeConverter
;
; Build with: ISCC.exe SuperTimecodeConverter.iss
;
; Before building, place the compiled exe in the same directory as this script,
; or adjust SourceDir below to point to your build output folder.

#define MyAppName      "Super Timecode Converter"
#define MyAppVersion   "1.8.0"
#define MyAppPublisher "Fiverecords"
#define MyAppURL       "https://github.com/fiverecords/SuperTimecodeConverter"
#define MyAppExeName   "Super Timecode Converter.exe"

[Setup]
AppId={{8F4E2A6B-3C71-4D9F-A852-1B7E0F3D5C49}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppVerName={#MyAppName} {#MyAppVersion}
AppPublisher={#MyAppPublisher}
AppPublisherURL={#MyAppURL}
AppSupportURL={#MyAppURL}
AppUpdatesURL={#MyAppURL}/releases
DefaultDirName={autopf}\{#MyAppName}
DefaultGroupName={#MyAppName}
AllowNoIcons=yes
OutputBaseFilename=SuperTimecodeConverter_{#MyAppVersion}_Setup
Compression=lzma2/ultra64
SolidCompression=yes
WizardStyle=modern
PrivilegesRequired=admin
; Minimum Windows 10
MinVersion=10.0
ArchitecturesInstallIn64BitModeOnly=x64compatible
ArchitecturesAllowed=x64compatible
; Uninstall info
UninstallDisplayIcon={app}\{#MyAppExeName}
UninstallDisplayName={#MyAppName}

; Windows firewall: request inbound rule so Defender doesn't block sockets.
; This avoids the "Windows Firewall has blocked some features" popup on first run.
; Uncomment the next two lines if you want the installer to add a firewall rule:
; [Run]
; Filename: "netsh"; Parameters: "advfirewall firewall add rule name=""Super Timecode Converter"" dir=in action=allow program=""{app}\{#MyAppExeName}"" enable=yes profile=any"; Flags: runhidden nowait; StatusMsg: "Configuring firewall..."

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[Files]
Source: "{#MyAppExeName}"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{group}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"
Name: "{group}\{cm:UninstallProgram,{#MyAppName}}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; Tasks: desktopicon

[Run]
Filename: "{app}\{#MyAppExeName}"; Description: "{cm:LaunchProgram,{#StringChange(MyAppName, '&', '&&')}}"; Flags: nowait postinstall skipifsilent
