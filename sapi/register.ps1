param([switch]$Register,[switch]$Unregister,[string]$DataRoot,
      [switch]$Move,[string]$MoveFrom,[string]$MoveTo)
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
# Remembered machine-wide, in **both registry views**, because HKLM\Software
# is redirected under WOW64 and a value in one view alone is invisible to
# half the programs on the machine.  Machine-wide rather than HKCU on
# purpose: this script runs elevated, and on a machine where the person at
# the keyboard is not the administrator who answered the prompt, an HKCU
# write here lands in the *administrator's* hive -- remembered for the wrong
# person, invisible to the right one.  The settings window writes the
# per-user preference itself, unelevated, where HKCU means what it says.
foreach ($prefPath in @('HKLM:\SOFTWARE\outSPOKEN SAPI',
                        'HKLM:\SOFTWARE\Wow6432Node\outSPOKEN SAPI')) {
    if (($prefPath -like '*Wow6432Node*') -and
        -not (Test-Path 'HKLM:\SOFTWARE\Wow6432Node')) { continue }
    New-Item -Path $prefPath -Force | Out-Null
    Set-ItemProperty -Path $prefPath -Name DataPath -Value $DataRoot
}

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

if ($Move) {
    # Move the outSPOKEN tree -- and only the outSPOKEN tree -- somewhere
    # every account can read.  Child by child, so a run that stopped halfway
    # (a locked ROM, a full disk) is finished by running it again rather
    # than refused.  The caller decided *what* moves; this half only carries
    # it, locks it, and re-registers.
    if ((-not $MoveFrom) -or (-not $MoveTo)) { exit 2 }
    if (-not (Test-Path -LiteralPath $MoveFrom)) { exit 3 }
    # Belt and suspenders on the caller's own rule: never a folder that also
    # holds Panthera's generations -- that data belongs to another add-on.
    foreach ($name in @('tiger','leopard','snowleopard','lion')) {
        if (Test-Path -LiteralPath (Join-Path $MoveFrom $name)) { exit 6 }
    }
    try {
        New-Item -ItemType Directory -Force -Path $MoveTo | Out-Null
        foreach ($child in @(Get-ChildItem -Path $MoveFrom -Force)) {
            $target = Join-Path $MoveTo $child.Name
            if (Test-Path -LiteralPath $target) { continue }
            Move-Item -Path $child.FullName -Destination $target -ErrorAction Stop
        }
    } catch { exit 4 }
    # **Readable by everybody, writable by nobody but an administrator.**
    # What ProgramData grants by inheritance is Users:(CI)(WD,AD,...) --
    # every account may create files anywhere beneath it, and this tree is
    # executed, not read: the engine ROMs run under the emulator inside
    # NVDA, which is SYSTEM on the sign-in screen.  Locked at the shared
    # `macintalk` root with the same ACL Panthera's mover sets, so whichever
    # add-on moves first locks the folder for both.  SIDs, not names,
    # because BUILTIN\Users is localised.
    $sharedRoot = Split-Path -Parent $MoveTo
    & "$env:SystemRoot\System32\icacls.exe" $sharedRoot /inheritance:r `
        /grant '*S-1-5-18:(OI)(CI)F' `
        /grant '*S-1-5-32-544:(OI)(CI)F' `
        /grant '*S-1-5-32-545:(OI)(CI)RX' /T /C /Q | Out-Null
    # Past this line the data has moved: a failure now is "moved but the
    # registry did not follow", which needs different words said to the
    # person than "nothing changed", so it has its own exit code.
    $newRoot = Split-Path -Parent $sharedRoot
    & powershell.exe -NoProfile -WindowStyle Hidden -ExecutionPolicy Bypass -File $MyInvocation.MyCommand.Path -Register -DataRoot $newRoot
    if ($LASTEXITCODE) { exit 5 }
    $leftovers = @(Get-ChildItem -Path $MoveFrom -Force -ErrorAction SilentlyContinue)
    if (-not $leftovers.Length) {
        Remove-Item -Path $MoveFrom -Force -ErrorAction SilentlyContinue
    }
    exit 0
}

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
            # Spanish voices say so: SAPI clients filter and group by this,
            # and a Carlos advertised as US English is a voice Spanish
            # speakers' tooling never offers them.  80A is Mexican Spanish,
            # which is what the cami engine is -- sold on Mexican floppies,
            # named for it.  Everything else stays US English as before.
            $language = '409'
            if ($id -like 'cami:*') { $language = '80A' }
            Set-ItemProperty -Path $attrPath -Name 'Language' -Value $language
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
