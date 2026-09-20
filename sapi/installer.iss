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
#define AppVer "2.0.1"

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
; The native host that serves speech: the 64-bit one on a 64-bit Windows,
; the 32-bit one everywhere, and the DLL takes whichever is there.
Source: "{#StageDir}\osp_host.exe"; DestDir: "{app}"; Check: Is64BitInstallMode
Source: "{#StageDir}\osp_host_x86.exe"; DestDir: "{app}"
Source: "{#StageDir}\osp_serve.py"; DestDir: "{app}"
Source: "{#StageDir}\register.ps1"; DestDir: "{app}"
Source: "{#StageDir}\settings.ps1"; DestDir: "{app}"
Source: "{#StageDir}\settings_common.ps1"; DestDir: "{app}"
Source: "{#StageDir}\settings.cmd"; DestDir: "{app}"
Source: "{#StageDir}\outspoken_settings.exe"; DestDir: "{app}"
Source: "{#StageDir}\synthDrivers\*"; DestDir: "{app}\synthDrivers"; Flags: recursesubdirs
Source: "{#StageDir}\python\*"; DestDir: "{app}\python"; Flags: recursesubdirs

[Dirs]
; The machine-wide settings file lives here, and every standard account
; writes it: the settings tool keeps this copy current on every save, so the
; sign-in screen speaks with the settings its owner chose last rather than
; whatever an elevated trip mirrored months ago.  Modify and not full
; control: the file is what SYSTEM reads there, and its permissions are not
; something a standard account should be able to change.  The tool's own
; elevated trips grant the same on a machine upgraded from an installer
; without this entry.  Panthera's model, carried over.
Name: "{commonappdata}\outSPOKEN SAPI"; Permissions: users-modify

[Icons]
; The launcher rather than the batch file: a GUI-subsystem program creates
; no console, so nothing flashes or steals focus before the dialog appears.
Name: "{autoprograms}\outSPOKEN SAPI settings"; Filename: "{app}\outspoken_settings.exe"; WorkingDir: "{app}"

[Run]
; On a fresh install the register pass does everything in order: regsvr32
; for both registry views, then one token per voice the host lists from the
; resolved data root.  Registering with no data present is a clean no-op.
; On an upgrade only the COM classes are refreshed: which voices are
; registered, and from which folder, is a choice the person already made --
; a deliberate unregister, a folder they browsed to -- and rebuilding the
; tokens from whatever the elevated account can see is how Panthera's 3.2.0
; lost people's choices until its r2.  Even zero voices is a choice to keep.
Filename: "powershell.exe"; Parameters: "-NoProfile -ExecutionPolicy Bypass -File ""{app}\register.ps1"" -RegisterServer"; StatusMsg: "Updating SAPI components..."; Flags: runhidden; Check: IsUpgrade
Filename: "powershell.exe"; Parameters: "-NoProfile -ExecutionPolicy Bypass -File ""{app}\register.ps1"" -Register"; StatusMsg: "Registering outSPOKEN voices from your speech data..."; Flags: runhidden; Check: not IsUpgrade
Filename: "{app}\outspoken_settings.exe"; Description: "Open outSPOKEN SAPI settings"; Flags: postinstall nowait skipifsilent

[UninstallRun]
Filename: "powershell.exe"; Parameters: "-NoProfile -ExecutionPolicy Bypass -File ""{app}\register.ps1"" -Unregister"; RunOnceId: "UnregisterOutspoken"; Flags: runhidden

[Code]
const
  UninstallKey = 'Software\Microsoft\Windows\CurrentVersion\Uninstall\{4D6071E1-B142-4F49-8C5C-97C661EA748B}_is1';
var
  ExistingInstall: Boolean;

function InitializeSetup: Boolean;
begin
  { Snapshot before Setup writes its own uninstall key.  Checking in [Run]
    would mistake a first install for an upgrade.  Read both registry views. }
  ExistingInstall := RegKeyExists(HKLM32, UninstallKey);
  if IsWin64 then
    ExistingInstall := ExistingInstall or RegKeyExists(HKLM64, UninstallKey);
  Result := True;
end;

function IsUpgrade: Boolean;
begin
  Result := ExistingInstall;
end;
