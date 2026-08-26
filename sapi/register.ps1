param([switch]$Register,[switch]$Unregister,[string]$DataRoot)
# Voice tokens for the outSPOKEN SAPI engine: one per voice the serve
# bridge enumerates from the data root, in both registry views.
#
# Written in PowerShell 2.0's dialect on purpose: stock Windows 7 has no
# newer engine, and this script runs from the installer, where "update
# PowerShell first" is not an answer.  Hence no $PSScriptRoot (3.0+), no
# OpenBaseKey (needs the .NET 4 runtime 2.0 may not host), and the 32-bit
# view reached the old way: on a 64-bit OS the Wow6432Node path IS the
# 32-bit view, written directly.
$ErrorActionPreference = 'Stop'
$stage = Split-Path -Parent $MyInvocation.MyCommand.Path
$clsid = '{a1f4055c-b6c2-4c27-ab6a-af54c409a309}'
$prefKey = 'HKCU:\Software\outSPOKEN SAPI'

# The Panthera resolution order, mirrored: an explicit choice, then the
# remembered one, then NVDA's own config (an NVDA-first user's ROMs are
# already there), then the standalone default.
if (-not $DataRoot) {
    try { $DataRoot = (Get-ItemProperty -Path $prefKey -Name DataPath -ErrorAction Stop).DataPath } catch {}
}
if (-not $DataRoot) {
    $nvda = Join-Path $env:APPDATA 'nvda'
    if ((Test-Path (Join-Path $nvda 'macintalk\outspoken')) -or
        (Test-Path (Join-Path $nvda 'outspoken-roms'))) { $DataRoot = $nvda }
    else { $DataRoot = Join-Path $env:APPDATA 'outspoken-data' }
}
# Remembered, so the NVDA add-on's own lookup can pay the courtesy back.
New-Item -Path $prefKey -Force | Out-Null
Set-ItemProperty -Path $prefKey -Name DataPath -Value $DataRoot

#: Both registry views, as plain paths.  A 64-bit OS shows the 32-bit view
#: at Wow6432Node; a 32-bit OS has one view and the second path is absent.
$tokenRoots = @('HKLM:\SOFTWARE\Microsoft\Speech\Voices\Tokens')
if (Test-Path 'HKLM:\SOFTWARE\Wow6432Node\Microsoft\Speech') {
    $tokenRoots += 'HKLM:\SOFTWARE\Wow6432Node\Microsoft\Speech\Voices\Tokens'
}

function Remove-Tokens {
    foreach ($root in $tokenRoots) {
        if (-not (Test-Path $root)) { continue }
        foreach ($key in @(Get-ChildItem $root -ErrorAction SilentlyContinue)) {
            if ($key.PSChildName -like 'Outspoken_*') {
                Remove-Item -Path $key.PSPath -Recurse -Force
            }
        }
    }
}

#: On a 64-bit OS the 32-bit regsvr32 lives in SysWOW64 and the 64-bit one
#: in System32; a 32-bit OS has one System32 and it is the 32-bit world.
$wow = Test-Path "$env:SystemRoot\SysWOW64"
$reg32 = "$env:SystemRoot\System32\regsvr32.exe"
if ($wow) { $reg32 = "$env:SystemRoot\SysWOW64\regsvr32.exe" }

if ($Register) {
    & $reg32 /s (Join-Path $stage 'x86\outspoken_sapi.dll')
    if ($LASTEXITCODE) { exit $LASTEXITCODE }
    if ($wow) {
        & "$env:SystemRoot\System32\regsvr32.exe" /s (Join-Path $stage 'x64\outspoken_sapi.dll')
        if ($LASTEXITCODE) { exit $LASTEXITCODE }
    }
    # The serve bridge is the one authority on which voices the data
    # provides; registering with no data present is a clean no-op.
    $py = Join-Path $stage 'python\python.exe'
    if (-not (Test-Path $py)) { $py = 'python' }
    $listing = & $py (Join-Path $stage 'osp_serve.py') --list $DataRoot 2>$null
    Remove-Tokens
    foreach ($root in $tokenRoots) {
        foreach ($line in @($listing)) {
            if ($line -notmatch "`t") { continue }
            $parts = $line -split "`t",2
            $id = $parts[0]; $name = $parts[1]
            $keyPath = Join-Path $root ('Outspoken_' + ($id -replace '[^A-Za-z0-9]','_'))
            New-Item -Path $keyPath -Force | Out-Null
            Set-ItemProperty -Path $keyPath -Name '(default)' -Value "$name (outSPOKEN)"
            Set-ItemProperty -Path $keyPath -Name 'CLSID' -Value $clsid
            Set-ItemProperty -Path $keyPath -Name 'VoiceId' -Value $id
            Set-ItemProperty -Path $keyPath -Name 'DataPath' -Value $DataRoot
            $attrPath = Join-Path $keyPath 'Attributes'
            New-Item -Path $attrPath -Force | Out-Null
            Set-ItemProperty -Path $attrPath -Name 'Name' -Value $name
            Set-ItemProperty -Path $attrPath -Name 'Vendor' -Value 'outSPOKEN'
            Set-ItemProperty -Path $attrPath -Name 'Language' -Value '409'
            Set-ItemProperty -Path $attrPath -Name 'Gender' -Value 'Neutral'
        }
    }
    exit 0
}
if ($Unregister) {
    Remove-Tokens
    & $reg32 /s /u (Join-Path $stage 'x86\outspoken_sapi.dll')
    if ($wow) {
        & "$env:SystemRoot\System32\regsvr32.exe" /s /u (Join-Path $stage 'x64\outspoken_sapi.dll')
    }
    exit 0
}
Write-Host 'Use -Register or -Unregister (optionally -DataRoot <folder>).'
