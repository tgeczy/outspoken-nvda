; outSPOKEN SAPI -- one installer, twelve-plus voices, zero ROMs.
;
; It ships only our code: the engine DLLs, the serve bridge, the NVDA
; driver modules the bridge serves (MIT, ours), and python.org's embeddable
; interpreter.  The outSPOKEN engine itself -- Berkeley Systems' work, whose
; rights passed through ALVA to Vispero -- is never packaged, looked for, or
; touched; you extract it from your own outSPOKEN disk or disk image with
; the add-on's Tools-menu manager or tools/extract_rom.py, and the voices
; appear the moment the data exists.  A machine with no data registers no
; voices and that is the correct outcome.
;
; Build:  powershell -ExecutionPolicy Bypass -File .\sapi\build.ps1
;         ISCC .\sapi\installer.iss
#ifndef StageDir
#define StageDir "C:\outspoken\sapi"
#endif
#define AppVer "1.2.0"

[Setup]
AppId={{4D6071E1-B142-4F49-8C5C-97C661EA748B}
AppName=outSPOKEN SAPI
AppVersion={#AppVer}
AppPublisher=outSPOKEN NVDA project
AppSupportURL=https://github.com/tgeczy/outspoken-nvda
DefaultDirName={autopf}\outSPOKEN SAPI
ArchitecturesInstallIn64BitMode=x64compatible
PrivilegesRequired=admin
Compression=lzma2
SolidCompression=yes
OutputDir={#StageDir}\out
OutputBaseFilename=outspoken-sapi-{#AppVer}-setup
DisableProgramGroupPage=yes
UninstallDisplayName=outSPOKEN SAPI {#AppVer}

[Files]
Source: "{#StageDir}\x86\outspoken_sapi.dll"; DestDir: "{app}\x86"
Source: "{#StageDir}\x64\outspoken_sapi.dll"; DestDir: "{app}\x64"; Check: Is64BitInstallMode
Source: "{#StageDir}\osp_serve.py"; DestDir: "{app}"
Source: "{#StageDir}\register.ps1"; DestDir: "{app}"
Source: "{#StageDir}\settings.ps1"; DestDir: "{app}"
Source: "{#StageDir}\settings.cmd"; DestDir: "{app}"
Source: "{#StageDir}\outspoken_settings.exe"; DestDir: "{app}"
Source: "{#StageDir}\synthDrivers\*"; DestDir: "{app}\synthDrivers"; Flags: recursesubdirs
Source: "{#StageDir}\python\*"; DestDir: "{app}\python"; Flags: recursesubdirs

[Icons]
; The launcher rather than the batch file: a GUI-subsystem program creates
; no console, so nothing flashes or steals focus before the dialog appears.
Name: "{autoprograms}\outSPOKEN SAPI settings"; Filename: "{app}\outspoken_settings.exe"; WorkingDir: "{app}"

[Run]
; Registering with no data present is a clean no-op: the serve bridge lists
; the voices the data root actually provides, and tokens follow the data.
Filename: "powershell.exe"; Parameters: "-NoProfile -ExecutionPolicy Bypass -File ""{app}\register.ps1"" -Register"; StatusMsg: "Registering outSPOKEN voices from your speech data..."; Flags: runhidden
Filename: "{app}\outspoken_settings.exe"; Description: "Open outSPOKEN SAPI settings"; Flags: postinstall nowait skipifsilent

[UninstallRun]
Filename: "powershell.exe"; Parameters: "-NoProfile -ExecutionPolicy Bypass -File ""{app}\register.ps1"" -Unregister"; RunOnceId: "UnregisterOutspoken"; Flags: runhidden
