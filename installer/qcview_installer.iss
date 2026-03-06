; QCView Installer Script for Inno Setup 6
; https://jrsoftware.org/isinfo.php

#define MyAppName "QCView"
#define MyAppVersion "1.0.3"
#define MyAppPublisher "cbkow"
#define MyAppURL "https://github.com/cbkow/ump"
#define MyAppExeName "qcview.exe"
#define MyAppAssocName "QCView Media File"
#define MyAppAssocExt ".qcv"
#define MyAppAssocKey StringChange(MyAppAssocName, " ", "") + MyAppAssocExt

[Setup]
; App information
AppId={{8F9A7B3C-4E5D-6F2A-8B1C-9D3E4F5A6B7C}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppVerName={#MyAppName} {#MyAppVersion}
AppPublisher={#MyAppPublisher}
AppPublisherURL={#MyAppURL}
AppSupportURL={#MyAppURL}
AppUpdatesURL={#MyAppURL}
AppCopyright=Copyright (C) 2025 {#MyAppPublisher}

; Installation directories
DefaultDirName={autopf}\QCView
DefaultGroupName={#MyAppName}
DisableProgramGroupPage=yes

; Licensing
LicenseFile=..\LICENSE
InfoBeforeFile=..\LICENSES\THIRD_PARTY_NOTICES.txt

; Output settings
OutputDir=.
OutputBaseFilename=qcview-setup-{#MyAppVersion}
Compression=lzma2/max
SolidCompression=yes

; Visual settings
SetupIconFile=..\assets\icons\qcview.ico
UninstallDisplayIcon={app}\{#MyAppExeName}
WizardStyle=modern

; System requirements
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
PrivilegesRequired=admin
MinVersion=10.0.17763

; Uninstaller settings
UninstallDisplayName={#MyAppName}
UninstallFilesDir={app}\uninstall

; Miscellaneous
AllowNoIcons=yes
DisableWelcomePage=no

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Types]
Name: "full"; Description: "Full installation (recommended)"
Name: "minimal"; Description: "Minimal installation"
Name: "custom"; Description: "Custom installation"; Flags: iscustom

[Components]
Name: "core"; Description: "Core application files"; Types: full minimal custom; Flags: fixed
Name: "video_assoc"; Description: "Associate video files with QCView"; Types: full
Name: "audio_assoc"; Description: "Associate audio files with QCView"; Types: custom
Name: "image_assoc"; Description: "Associate image files with QCView"; Types: full
Name: "image_assoc\essential"; Description: "Essential formats (gif, exr) - Animated GIF and EXR images"; Types: full; Flags: exclusive
Name: "image_assoc\all"; Description: "All supported formats (gif, exr, jpg, png, tiff, hdr)"; Types: custom; Flags: exclusive
Name: "project_assoc"; Description: "Associate project files (.qcv, .qcvproj, .qcvexr)"; Types: full minimal; Flags: fixed
Name: "uri_protocol"; Description: "Register qcview:// URI protocol for project links"; Types: full
Name: "shortcuts"; Description: "Create shortcuts"
Name: "shortcuts\desktop"; Description: "Create desktop shortcut"; Types: full
Name: "shortcuts\startmenu"; Description: "Create Start Menu shortcuts"; Types: full minimal; Flags: fixed

[Tasks]
Name: "cleansettings"; Description: "Delete user settings and cache (%LOCALAPPDATA%\qcview\)"; GroupDescription: "User data:"; Flags: unchecked
Name: "launchafter"; Description: "Launch {#MyAppName} after installation"; GroupDescription: "Post-installation:"; Flags: unchecked

[Files]
; Main executable
Source: "..\build\Release\{#MyAppExeName}"; DestDir: "{app}"; Flags: ignoreversion; Components: core

; Core DLLs (FFmpeg, OpenEXR, OpenColorIO, etc.)
Source: "..\build\Release\*.dll"; DestDir: "{app}"; Flags: ignoreversion; Components: core
Source: "..\build\Release\*.exe"; DestDir: "{app}"; Excludes: "{#MyAppExeName}"; Flags: ignoreversion; Components: core

; Assets directory (recursive)
Source: "..\build\Release\assets\*"; DestDir: "{app}\assets"; Flags: ignoreversion recursesubdirs createallsubdirs; Components: core

; Documentation
Source: "..\LICENSE"; DestDir: "{app}"; DestName: "LICENSE.txt"; Flags: ignoreversion; Components: core
Source: "..\LICENSES\THIRD_PARTY_NOTICES.txt"; DestDir: "{app}"; Flags: ignoreversion; Components: core

[Icons]
; Start Menu shortcuts
Name: "{group}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; Components: shortcuts\startmenu
Name: "{group}\Uninstall {#MyAppName}"; Filename: "{uninstallexe}"; Components: shortcuts\startmenu

; Desktop shortcut
Name: "{autodesktop}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; Components: shortcuts\desktop

[Registry]
; ========================================
; PROJECT FILE ASSOCIATIONS (.qcv, .qcvproj, .qcvexr)
; ========================================

; .qcv extension
Root: HKCR; Subkey: ".qcv"; ValueType: string; ValueName: ""; ValueData: "QCView.MediaFile"; Flags: uninsdeletevalue; Components: project_assoc
Root: HKCR; Subkey: "QCView.MediaFile"; ValueType: string; ValueName: ""; ValueData: "{#MyAppAssocName}"; Flags: uninsdeletekey; Components: project_assoc
Root: HKCR; Subkey: "QCView.MediaFile\DefaultIcon"; ValueType: string; ValueName: ""; ValueData: "{app}\assets\icons\qcview512.ico,0"; Components: project_assoc
Root: HKCR; Subkey: "QCView.MediaFile\shell\open\command"; ValueType: string; ValueName: ""; ValueData: """{app}\{#MyAppExeName}"" ""%1"""; Components: project_assoc

; .qcvexr extension
Root: HKCR; Subkey: ".qcvexr"; ValueType: string; ValueName: ""; ValueData: "QCView.EXRProjectFile"; Flags: uninsdeletevalue; Components: project_assoc
Root: HKCR; Subkey: "QCView.EXRProjectFile"; ValueType: string; ValueName: ""; ValueData: "QCView EXR Project File"; Flags: uninsdeletekey; Components: project_assoc
Root: HKCR; Subkey: "QCView.EXRProjectFile\DefaultIcon"; ValueType: string; ValueName: ""; ValueData: "{app}\assets\icons\qcview512.ico,0"; Components: project_assoc
Root: HKCR; Subkey: "QCView.EXRProjectFile\shell\open\command"; ValueType: string; ValueName: ""; ValueData: """{app}\{#MyAppExeName}"" ""%1"""; Components: project_assoc

; .qcvproj extension
Root: HKCR; Subkey: ".qcvproj"; ValueType: string; ValueName: ""; ValueData: "QCView.ProjectFile"; Flags: uninsdeletevalue; Components: project_assoc
Root: HKCR; Subkey: "QCView.ProjectFile"; ValueType: string; ValueName: ""; ValueData: "QCView Project File"; Flags: uninsdeletekey; Components: project_assoc
Root: HKCR; Subkey: "QCView.ProjectFile\DefaultIcon"; ValueType: string; ValueName: ""; ValueData: "{app}\assets\icons\qcview512.ico,0"; Components: project_assoc
Root: HKCR; Subkey: "QCView.ProjectFile\shell\open\command"; ValueType: string; ValueName: ""; ValueData: """{app}\{#MyAppExeName}"" ""%1"""; Components: project_assoc

; ========================================
; VIDEO FILE ASSOCIATIONS
; ========================================

; MP4
Root: HKCR; Subkey: ".mp4"; ValueType: string; ValueName: ""; ValueData: "QCView.VideoFile"; Flags: uninsdeletevalue; Components: video_assoc
; MKV
Root: HKCR; Subkey: ".mkv"; ValueType: string; ValueName: ""; ValueData: "QCView.VideoFile"; Flags: uninsdeletevalue; Components: video_assoc
; AVI
Root: HKCR; Subkey: ".avi"; ValueType: string; ValueName: ""; ValueData: "QCView.VideoFile"; Flags: uninsdeletevalue; Components: video_assoc
; MOV
Root: HKCR; Subkey: ".mov"; ValueType: string; ValueName: ""; ValueData: "QCView.VideoFile"; Flags: uninsdeletevalue; Components: video_assoc
; WMV
Root: HKCR; Subkey: ".wmv"; ValueType: string; ValueName: ""; ValueData: "QCView.VideoFile"; Flags: uninsdeletevalue; Components: video_assoc
; FLV
Root: HKCR; Subkey: ".flv"; ValueType: string; ValueName: ""; ValueData: "QCView.VideoFile"; Flags: uninsdeletevalue; Components: video_assoc
; WEBM
Root: HKCR; Subkey: ".webm"; ValueType: string; ValueName: ""; ValueData: "QCView.VideoFile"; Flags: uninsdeletevalue; Components: video_assoc
; M4V
Root: HKCR; Subkey: ".m4v"; ValueType: string; ValueName: ""; ValueData: "QCView.VideoFile"; Flags: uninsdeletevalue; Components: video_assoc
; M2TS/MTS/TS
Root: HKCR; Subkey: ".m2ts"; ValueType: string; ValueName: ""; ValueData: "QCView.VideoFile"; Flags: uninsdeletevalue; Components: video_assoc
Root: HKCR; Subkey: ".mts"; ValueType: string; ValueName: ""; ValueData: "QCView.VideoFile"; Flags: uninsdeletevalue; Components: video_assoc
Root: HKCR; Subkey: ".ts"; ValueType: string; ValueName: ""; ValueData: "QCView.VideoFile"; Flags: uninsdeletevalue; Components: video_assoc
; MXF
Root: HKCR; Subkey: ".mxf"; ValueType: string; ValueName: ""; ValueData: "QCView.VideoFile"; Flags: uninsdeletevalue; Components: video_assoc
; OGV
Root: HKCR; Subkey: ".ogv"; ValueType: string; ValueName: ""; ValueData: "QCView.VideoFile"; Flags: uninsdeletevalue; Components: video_assoc
; 3GP
Root: HKCR; Subkey: ".3gp"; ValueType: string; ValueName: ""; ValueData: "QCView.VideoFile"; Flags: uninsdeletevalue; Components: video_assoc

; Video file type definition
Root: HKCR; Subkey: "QCView.VideoFile"; ValueType: string; ValueName: ""; ValueData: "Video File"; Flags: uninsdeletekey; Components: video_assoc
Root: HKCR; Subkey: "QCView.VideoFile\DefaultIcon"; ValueType: string; ValueName: ""; ValueData: "{app}\assets\icons\qcview512.ico,0"; Components: video_assoc
Root: HKCR; Subkey: "QCView.VideoFile\shell\open\command"; ValueType: string; ValueName: ""; ValueData: """{app}\{#MyAppExeName}"" ""%1"""; Components: video_assoc

; ========================================
; IMAGE FILE ASSOCIATIONS (Essential: GIF + EXR)
; ========================================

; GIF
Root: HKCR; Subkey: ".gif"; ValueType: string; ValueName: ""; ValueData: "QCView.ImageFile"; Flags: uninsdeletevalue; Components: image_assoc\essential
; EXR
Root: HKCR; Subkey: ".exr"; ValueType: string; ValueName: ""; ValueData: "QCView.ImageFile"; Flags: uninsdeletevalue; Components: image_assoc\essential

; Image file type definition (Essential)
Root: HKCR; Subkey: "QCView.ImageFile"; ValueType: string; ValueName: ""; ValueData: "Image File"; Flags: uninsdeletekey; Components: image_assoc\essential
Root: HKCR; Subkey: "QCView.ImageFile\DefaultIcon"; ValueType: string; ValueName: ""; ValueData: "{app}\assets\icons\qcview512.ico,0"; Components: image_assoc\essential
Root: HKCR; Subkey: "QCView.ImageFile\shell\open\command"; ValueType: string; ValueName: ""; ValueData: """{app}\{#MyAppExeName}"" ""%1"""; Components: image_assoc\essential

; ========================================
; IMAGE FILE ASSOCIATIONS (All formats)
; ========================================

; GIF
Root: HKCR; Subkey: ".gif"; ValueType: string; ValueName: ""; ValueData: "QCView.ImageFile"; Flags: uninsdeletevalue; Components: image_assoc\all
; EXR
Root: HKCR; Subkey: ".exr"; ValueType: string; ValueName: ""; ValueData: "QCView.ImageFile"; Flags: uninsdeletevalue; Components: image_assoc\all
; HDR
Root: HKCR; Subkey: ".hdr"; ValueType: string; ValueName: ""; ValueData: "QCView.ImageFile"; Flags: uninsdeletevalue; Components: image_assoc\all
; TIFF
Root: HKCR; Subkey: ".tif"; ValueType: string; ValueName: ""; ValueData: "QCView.ImageFile"; Flags: uninsdeletevalue; Components: image_assoc\all
Root: HKCR; Subkey: ".tiff"; ValueType: string; ValueName: ""; ValueData: "QCView.ImageFile"; Flags: uninsdeletevalue; Components: image_assoc\all
; PNG
Root: HKCR; Subkey: ".png"; ValueType: string; ValueName: ""; ValueData: "QCView.ImageFile"; Flags: uninsdeletevalue; Components: image_assoc\all
; JPEG
Root: HKCR; Subkey: ".jpg"; ValueType: string; ValueName: ""; ValueData: "QCView.ImageFile"; Flags: uninsdeletevalue; Components: image_assoc\all
Root: HKCR; Subkey: ".jpeg"; ValueType: string; ValueName: ""; ValueData: "QCView.ImageFile"; Flags: uninsdeletevalue; Components: image_assoc\all

; Image file type definition (All formats)
Root: HKCR; Subkey: "QCView.ImageFile"; ValueType: string; ValueName: ""; ValueData: "Image File"; Flags: uninsdeletekey; Components: image_assoc\all
Root: HKCR; Subkey: "QCView.ImageFile\DefaultIcon"; ValueType: string; ValueName: ""; ValueData: "{app}\assets\icons\qcview512.ico,0"; Components: image_assoc\all
Root: HKCR; Subkey: "QCView.ImageFile\shell\open\command"; ValueType: string; ValueName: ""; ValueData: """{app}\{#MyAppExeName}"" ""%1"""; Components: image_assoc\all

; ========================================
; AUDIO FILE ASSOCIATIONS
; ========================================

; WAV
Root: HKCR; Subkey: ".wav"; ValueType: string; ValueName: ""; ValueData: "QCView.AudioFile"; Flags: uninsdeletevalue; Components: audio_assoc
; MP3
Root: HKCR; Subkey: ".mp3"; ValueType: string; ValueName: ""; ValueData: "QCView.AudioFile"; Flags: uninsdeletevalue; Components: audio_assoc
; AAC
Root: HKCR; Subkey: ".aac"; ValueType: string; ValueName: ""; ValueData: "QCView.AudioFile"; Flags: uninsdeletevalue; Components: audio_assoc
; FLAC
Root: HKCR; Subkey: ".flac"; ValueType: string; ValueName: ""; ValueData: "QCView.AudioFile"; Flags: uninsdeletevalue; Components: audio_assoc
; OGG
Root: HKCR; Subkey: ".ogg"; ValueType: string; ValueName: ""; ValueData: "QCView.AudioFile"; Flags: uninsdeletevalue; Components: audio_assoc
; M4A
Root: HKCR; Subkey: ".m4a"; ValueType: string; ValueName: ""; ValueData: "QCView.AudioFile"; Flags: uninsdeletevalue; Components: audio_assoc
; WMA
Root: HKCR; Subkey: ".wma"; ValueType: string; ValueName: ""; ValueData: "QCView.AudioFile"; Flags: uninsdeletevalue; Components: audio_assoc

; Audio file type definition
Root: HKCR; Subkey: "QCView.AudioFile"; ValueType: string; ValueName: ""; ValueData: "Audio File"; Flags: uninsdeletekey; Components: audio_assoc
Root: HKCR; Subkey: "QCView.AudioFile\DefaultIcon"; ValueType: string; ValueName: ""; ValueData: "{app}\assets\icons\qcview512.ico,0"; Components: audio_assoc
Root: HKCR; Subkey: "QCView.AudioFile\shell\open\command"; ValueType: string; ValueName: ""; ValueData: """{app}\{#MyAppExeName}"" ""%1"""; Components: audio_assoc
; ========================================
; CUSTOM URI PROTOCOL (qcview://)
; ========================================

Root: HKCR; Subkey: "qcview"; ValueType: string; ValueName: ""; ValueData: "URL:QCView Project Protocol"; Flags: uninsdeletekey; Components: uri_protocol
Root: HKCR; Subkey: "qcview"; ValueType: string; ValueName: "URL Protocol"; ValueData: ""; Components: uri_protocol
Root: HKCR; Subkey: "qcview\DefaultIcon"; ValueType: string; ValueName: ""; ValueData: "{app}\assets\icons\qcview256.ico,0"; Components: uri_protocol
Root: HKCR; Subkey: "qcview\shell\open\command"; ValueType: string; ValueName: ""; ValueData: """{app}\{#MyAppExeName}"" ""%1"""; Components: uri_protocol

; ========================================
; APP USER MODEL ID (for Windows 11 taskbar/start menu)
; ========================================

Root: HKLM; Subkey: "Software\Classes\Applications\{#MyAppExeName}"; ValueType: string; ValueName: "AppUserModelID"; ValueData: "cbkow.qcview"; Flags: uninsdeletekey

; ========================================
; CLEAR PROGRAM COMPATIBILITY FLAGS
; Remove any stale PCA entries that could interfere with D3D11/ffmpeg
; ========================================

Root: HKCU; Subkey: "Software\Microsoft\Windows NT\CurrentVersion\AppCompatFlags\Layers"; ValueName: "{app}\{#MyAppExeName}"; Flags: deletevalue
Root: HKLM; Subkey: "Software\Microsoft\Windows NT\CurrentVersion\AppCompatFlags\Layers"; ValueName: "{app}\{#MyAppExeName}"; Flags: deletevalue

; ========================================
; UNINSTALL REGISTRY INFO
; ========================================

Root: HKLM; Subkey: "Software\Microsoft\Windows\CurrentVersion\Uninstall\{{8F9A7B3C-4E5D-6F2A-8B1C-9D3E4F5A6B7C}_is1"; ValueType: string; ValueName: "DisplayName"; ValueData: "{#MyAppName}"; Flags: uninsdeletekey
Root: HKLM; Subkey: "Software\Microsoft\Windows\CurrentVersion\Uninstall\{{8F9A7B3C-4E5D-6F2A-8B1C-9D3E4F5A6B7C}_is1"; ValueType: string; ValueName: "DisplayIcon"; ValueData: "{app}\{#MyAppExeName},0"
Root: HKLM; Subkey: "Software\Microsoft\Windows\CurrentVersion\Uninstall\{{8F9A7B3C-4E5D-6F2A-8B1C-9D3E4F5A6B7C}_is1"; ValueType: string; ValueName: "DisplayVersion"; ValueData: "{#MyAppVersion}"
Root: HKLM; Subkey: "Software\Microsoft\Windows\CurrentVersion\Uninstall\{{8F9A7B3C-4E5D-6F2A-8B1C-9D3E4F5A6B7C}_is1"; ValueType: string; ValueName: "Publisher"; ValueData: "{#MyAppPublisher}"
Root: HKLM; Subkey: "Software\Microsoft\Windows\CurrentVersion\Uninstall\{{8F9A7B3C-4E5D-6F2A-8B1C-9D3E4F5A6B7C}_is1"; ValueType: string; ValueName: "URLInfoAbout"; ValueData: "{#MyAppURL}"

[Code]
// ========================================
// Pascal Script for Advanced Features
// ========================================

// Always perform clean install - remove all files before installing new version
procedure CurStepChanged(CurStep: TSetupStep);
var
  AppDir: String;
begin
  if CurStep = ssInstall then
  begin
    // Always delete old files before installing to ensure clean state
    AppDir := ExpandConstant('{app}');
    if DirExists(AppDir) then
    begin
      Log('Performing clean install - deleting: ' + AppDir);
      DelTree(AppDir, True, True, True);
    end;

    // Clean user settings if selected
    if WizardIsTaskSelected('cleansettings') then
    begin
      AppDir := ExpandConstant('{localappdata}\qcview');
      if DirExists(AppDir) then
      begin
        Log('Cleaning user settings: ' + AppDir);
        DelTree(AppDir, True, True, True);
      end;
    end;
  end;
end;

// Initialize uninstall
function InitializeUninstall(): Boolean;
begin
  Result := True;
end;

// Uninstall - prompt to delete settings
procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
  SettingsDir: String;
  Response: Integer;
begin
  if CurUninstallStep = usPostUninstall then
  begin
    SettingsDir := ExpandConstant('{localappdata}\qcview');
    if DirExists(SettingsDir) then
    begin
      Response := MsgBox('Do you want to delete your user settings and cache files?' + #13#10 +
                         'Location: ' + SettingsDir + #13#10#13#10 +
                         'Choose "Yes" to delete all settings (clean uninstall)' + #13#10 +
                         'Choose "No" to keep settings for future installations',
                         mbConfirmation, MB_YESNO);
      if Response = IDYES then
      begin
        Log('User chose to delete settings during uninstall');
        DelTree(SettingsDir, True, True, True);
      end
      else
      begin
        Log('User chose to keep settings during uninstall');
      end;
    end;
  end;
end;

[Run]
; Clear stale Windows Prefetch data to prevent DLL loading issues from cached paths
Filename: "cmd.exe"; Parameters: "/c del /q ""{win}\Prefetch\QCVIEW*.pf"" 2>nul"; Flags: runhidden waituntilterminated shellexec

; Launch application after installation if task is selected
Filename: "{app}\{#MyAppExeName}"; WorkingDir: "{app}"; Description: "{cm:LaunchProgram,{#StringChange(MyAppName, '&', '&&')}}"; Flags: nowait postinstall skipifsilent; Tasks: launchafter

[UninstallDelete]
; Clean up any generated files not tracked by installer
Type: filesandordirs; Name: "{app}\cache"
Type: filesandordirs; Name: "{app}\temp"
Type: filesandordirs; Name: "{app}\logs"

[Messages]
; Custom messages
WelcomeLabel2=This will install [name/ver] on your computer.%n%nQCView is a media player and review tool designed for visual effects and post-production workflows.%n%nIt is recommended that you close all other applications before continuing.
FinishedLabel=Setup has finished installing [name] on your computer.%n%nThe application may be launched by selecting the installed shortcuts.
