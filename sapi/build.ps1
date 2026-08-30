param([string]$OutputRoot = "C:\outspoken")
# Stage the outSPOKEN SAPI engine: both DLL bitnesses, the serve bridge, the
# driver package it serves (our own code, MIT -- the ROMs are never here),
# and the embeddable Python that runs it.  Template: panthera-speech's
# sapi/build.ps1.
$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent $PSScriptRoot
$msvc = Get-ChildItem "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Tools\MSVC" -Directory | Sort-Object Name | Select-Object -Last 1
$sdk = Get-ChildItem "C:\Program Files (x86)\Windows Kits\10\Include" -Directory | Sort-Object Name | Select-Object -Last 1
if (!$msvc -or !$sdk) { throw "MSVC Build Tools and the Windows SDK are required" }
$stage = Join-Path $OutputRoot "sapi"
New-Item -ItemType Directory -Force $stage,(Join-Path $stage "x86"),(Join-Path $stage "x64") | Out-Null
foreach ($arch in "x86","x64") {
  $cl = Join-Path $msvc.FullName "bin\Hostx64\$arch\cl.exe"
  $out = Join-Path $stage $arch
  & $cl /nologo /EHsc /O2 /MT /LD /DUNICODE /D_UNICODE "/I$($msvc.FullName)\include" "/I$($sdk.FullName)\um" "/I$($sdk.FullName)\shared" "/I$($sdk.FullName)\ucrt" (Join-Path $PSScriptRoot "outspoken_sapi.cpp") "/Fe$out\outspoken_sapi.dll" "/Fo$out\" /link "/DEF:$PSScriptRoot\outspoken_sapi.def" "/LIBPATH:$($msvc.FullName)\lib\$arch" "/LIBPATH:$($sdk.Parent.Parent.FullName)\Lib\$($sdk.Name)\um\$arch" "/LIBPATH:$($sdk.Parent.Parent.FullName)\Lib\$($sdk.Name)\ucrt\$arch" sapi.lib ole32.lib advapi32.lib
  if ($LASTEXITCODE) { throw "$arch SAPI DLL build failed ($LASTEXITCODE)" }
}
# The console-free way into the settings dialog: a GUI-subsystem launcher,
# so no console ever flashes and steals focus.  settings.cmd stays for
# anyone at a command line.
$launcherCl = Join-Path $msvc.FullName "bin\Hostx64\x64\cl.exe"
& $launcherCl /nologo /O2 /MT /W3 "/I$($msvc.FullName)\include" "/I$($sdk.FullName)\ucrt" "/I$($sdk.FullName)\um" "/I$($sdk.FullName)\shared" (Join-Path $PSScriptRoot "settings_launcher.c") "/Fe$stage\outspoken_settings.exe" "/Fo$stage\" /link /SUBSYSTEM:WINDOWS "/LIBPATH:$($msvc.FullName)\lib\x64" "/LIBPATH:$($sdk.Parent.Parent.FullName)\Lib\$($sdk.Name)\ucrt\x64" "/LIBPATH:$($sdk.Parent.Parent.FullName)\Lib\$($sdk.Name)\um\x64" user32.lib kernel32.lib
if ($LASTEXITCODE) { throw "settings launcher build failed ($LASTEXITCODE)" }
Set-Content -Encoding ASCII (Join-Path $stage "settings.cmd") '@echo off
powershell.exe -NoProfile -ExecutionPolicy Bypass -STA -File "%~dp0settings.ps1"'

# The serve bridge and the driver it serves.  This is the whole point: the
# same modules NVDA runs, not a port -- see tests/test_sapi_serve.py.
Copy-Item (Join-Path $PSScriptRoot "osp_serve.py") $stage
Copy-Item (Join-Path $PSScriptRoot "register.ps1") $stage
Copy-Item (Join-Path $PSScriptRoot "settings.ps1") $stage
# The command-line extractor, staged at the root where its fallback import
# path finds the driver tree at synthDrivers\_outspoken -- the settings
# window's Extract button runs it with the bundled Python, so a standalone
# SAPI user can go from disc image (or .smi.bin floppy set) to speaking
# without NVDA, a Python install, or an execution-policy change.
Copy-Item (Join-Path $repo "tools\extract_rom.py") $stage
$drv = Join-Path $stage "synthDrivers"
New-Item -ItemType Directory -Force $drv,(Join-Path $drv "_outspoken") | Out-Null
Copy-Item (Join-Path $repo "addon\synthDrivers\outspoken.py") $drv
Copy-Item (Join-Path $repo "addon\synthDrivers\_outspoken\*.py") (Join-Path $drv "_outspoken")
Copy-Item (Join-Path $repo "addon\synthDrivers\_outspoken\*.dll") (Join-Path $drv "_outspoken")
Copy-Item -Recurse -Force (Join-Path $repo "addon\synthDrivers\_outspoken\_machfs") (Join-Path $drv "_outspoken\_machfs")
Get-ChildItem -Recurse (Join-Path $drv "_outspoken") -Directory -Filter "__pycache__" | Remove-Item -Recurse -Force
# Embeddable Python, the portable lesson from day one.  The ._pth must name
# the parent folder or "import outspoken" from the serve script fails --
# the embeddable build locks sys.path to that file's entries.
#
# **3.8.10, deliberately, not current**: it is the last Windows build of
# CPython that runs on Windows 7, and this community runs Windows 7.  The
# driver code is 3.7-compatible by construction (minimumNVDAVersion 2023.1
# shipped Python 3.7), an offline pipe bridge has no security surface that
# an interpreter version changes, and one identical bundle everywhere beats
# a newer interpreter that quietly excludes the machines these voices were
# revived for.
$py = Join-Path $stage "python"
if (!(Test-Path (Join-Path $py "python38.dll"))) {
  if (Test-Path $py) { Remove-Item -Recurse -Force $py }
  $pyzip = Join-Path $env:TEMP "python-3.8.10-embed-amd64.zip"
  if (!(Test-Path $pyzip)) {
    Invoke-WebRequest -Uri "https://www.python.org/ftp/python/3.8.10/python-3.8.10-embed-amd64.zip" -OutFile $pyzip
  }
  Expand-Archive $pyzip -DestinationPath $py -Force
}
Set-Content -Encoding ASCII (Join-Path $py "python38._pth") @'
python38.zip
.
..
..\synthDrivers
..\synthDrivers\_outspoken
#import site
'@
Write-Host "outSPOKEN SAPI development build: $stage"
