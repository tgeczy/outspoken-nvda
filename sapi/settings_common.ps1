# outSPOKEN SAPI -- the settings files, shared by settings.ps1 and the
# elevated register.ps1.  Dot-sourced by both.
#
# **The engine's settings live in two files, and the registry is what they
# fall back to** -- Panthera's model from its 3.1.0, carried over.  The DLL
# reads, one typed value at a time: this user's file, the machine's, HKCU,
# HKLM, then its default.  This user's file is under %APPDATA%, where a
# choice belongs to the person who made it.  The machine's is under
# %ProgramData%, which every account can read and -- once the installer or
# an elevated trip has granted it -- any standard account can write, so the
# copy the Windows sign-in screen speaks with is the one its owner saved
# last.  Every save writes both.
#
# Flat TOML, so the DLL's reader fits in one file and the writer fits here:
# `Name = 1`, `Name = "word"`, `# comments`; a number is what the DLL reads
# as a DWORD and a quoted word is what it reads as a string.  Keys are the
# registry's own names, so nothing maps them.
#
# Written in PowerShell 2.0's dialect like the scripts that source it: no
# [ordered], no -in, no [pscustomobject], no [NullString].  The two paths
# honour the same environment overrides the DLL does, so a test can point
# both at scratch files.

#: The settings the engine DLL reads and the type it reads each as, as two
#: parallel arrays (PowerShell 2.0 has no ordered hashtable).  Tool state --
#: the data folder, the declined offer -- is deliberately not here: it is
#: this person's business and stays in HKCU.
$SettingNames = @('Inflection', 'NumberStyle', 'Diagnostics', 'ReadTimeoutMs')
$SettingKinds = @('DWord', 'String', 'DWord', 'DWord')
function Get-SettingKind([string]$name) {
    for ($i = 0; $i -lt $SettingNames.Length; $i++) {
        if ($SettingNames[$i] -eq $name) { return $SettingKinds[$i] }
    }
    return $null
}

function Get-SettingsFilePath([string]$override, [string]$base) {
    if ($override) { return $override }
    if ($base) { return (Join-Path $base 'outSPOKEN SAPI\settings.toml') }
    return $null
}
$commonBase = $env:ProgramData
if (-not $commonBase) { $commonBase = $env:ALLUSERSPROFILE }
$userSettingsFile = Get-SettingsFilePath $env:OUTSPOKEN_SAPI_SETTINGS_USER $env:APPDATA
$machineSettingsFile = Get-SettingsFilePath $env:OUTSPOKEN_SAPI_SETTINGS_MACHINE $commonBase

# One file as a list of @{Name;Value} in the file's own order, a Value being
# [int] or [string].  A line that does not parse is left out, which is the
# DLL's rule too: a hand edit that breaks one line cannot take a setting
# with it.  `true`/`false` read as 1/0 as a courtesy; nothing here writes them.
function Read-SettingsFile([string]$path) {
    $table = New-Object Collections.ArrayList
    if ((-not $path) -or (-not (Test-Path -LiteralPath $path))) { return ,$table }
    try { $lines = [IO.File]::ReadAllLines($path, [Text.Encoding]::UTF8) } catch { return ,$table }
    foreach ($raw in $lines) {
        $line = $raw.Trim()
        if ((-not $line) -or ($line[0] -eq '#')) { continue }
        $eq = $line.IndexOf('=')
        if ($eq -lt 0) { continue }
        $name = $line.Substring(0, $eq).Trim()
        $rest = $line.Substring($eq + 1).Trim()
        if (($name -notmatch '^[A-Za-z0-9_-]+$') -or (-not $rest)) { continue }
        if ($rest[0] -eq '"') {
            $text = New-Object Text.StringBuilder; $closed = $false; $i = 1
            while ($i -lt $rest.Length) {
                $c = $rest[$i]
                if (($c -eq '\') -and ($i + 1 -lt $rest.Length)) { [void]$text.Append($rest[$i + 1]); $i += 2; continue }
                if ($c -eq '"') { $closed = $true; break }
                [void]$text.Append($c); $i++
            }
            if (-not $closed) { continue }
            $value = $text.ToString()
            if ($value.Length -gt 63) { $value = $value.Substring(0, 63) }
            [void]$table.Add(@{ Name = $name; Value = [string]$value })
        } else {
            $hash = $rest.IndexOf('#')
            if ($hash -ge 0) { $rest = $rest.Substring(0, $hash).Trim() }
            if ($rest -eq 'true') { [void]$table.Add(@{ Name = $name; Value = [int]1 }) }
            elseif ($rest -eq 'false') { [void]$table.Add(@{ Name = $name; Value = [int]0 }) }
            elseif ($rest -match '^[+-]?\d{1,10}$') {
                $n = [long]$rest
                if (($n -ge -2147483648) -and ($n -le 2147483647)) { [void]$table.Add(@{ Name = $name; Value = [int]$n }) }
            }
        }
    }
    return ,$table
}
# Whole, then swapped in over the old one, because the engine may be reading
# it in another process at that very moment.  -> $true when it is on disk.
function Write-SettingsFile([string]$path, $table) {
    if (-not $path) { return $false }
    $text = New-Object Text.StringBuilder
    [void]$text.Append("# outSPOKEN SAPI settings.  Written by the settings program; safe to edit by hand.`r`n")
    [void]$text.Append("# Numbers stay numbers and words stay in quotes; a line that does not parse is ignored.`r`n")
    foreach ($entry in $table) {
        if ($entry.Value -is [string]) { $shown = '"' + ($entry.Value -replace '(["\\])', '\$1') + '"' }
        else { $shown = [string][int]$entry.Value }
        [void]$text.Append(('{0} = {1}' -f $entry.Name, $shown)).Append("`r`n")
    }
    try {
        $folder = Split-Path -Parent $path
        if ($folder -and (-not (Test-Path -LiteralPath $folder))) { New-Item -ItemType Directory -Path $folder -Force | Out-Null }
        $tmp = $path + '.tmp'
        [IO.File]::WriteAllText($tmp, $text.ToString(), (New-Object Text.UTF8Encoding($false)))
        # Copy over and delete rather than [IO.File]::Replace: PowerShell 2.0
        # hands a [string] parameter "" for $null and Replace refuses it.
        [IO.File]::Copy($tmp, $path, $true)
        [IO.File]::Delete($tmp)
        return $true
    } catch {
        Remove-Item -LiteralPath ($path + '.tmp') -Force -ErrorAction SilentlyContinue
        return $false
    }
}
# $table with $name set to $value: in place where the name already stands,
# any repeat of it dropped, at the end when it is new.
function Set-TableValue($table, [string]$name, $value) {
    $kept = New-Object Collections.ArrayList; $placed = $false
    foreach ($entry in $table) {
        if ($entry.Name -eq $name) {
            if (-not $placed) { [void]$kept.Add(@{ Name = $name; Value = $value }); $placed = $true }
        } else { [void]$kept.Add($entry) }
    }
    if (-not $placed) { [void]$kept.Add(@{ Name = $name; Value = $value }) }
    return ,$kept
}
# One key in one file, everything else in it left as it was.
function Set-SettingsFileValue([string]$path, [string]$name, $value) {
    if (-not $path) { return $false }
    return (Write-SettingsFile $path (Set-TableValue (Read-SettingsFile $path) $name $value))
}
# The last line naming $name in $path when it is of $kind ('DWord' or
# 'String', or $null for either), else $null -- the DLL's typed lookup.
function Get-SettingsFileValue([string]$path, [string]$name, [string]$kind) {
    $found = $null
    foreach ($entry in (Read-SettingsFile $path)) {
        if ($entry.Name -ne $name) { continue }
        $isString = $entry.Value -is [string]
        if (($kind -eq 'String') -and (-not $isString)) { continue }
        if (($kind -eq 'DWord') -and $isString) { continue }
        $found = $entry.Value
    }
    return $found
}

# The machine-wide folder, writable by every standard account.  Done on the
# elevated trips register.ps1 makes, which is what repairs a machine
# upgraded from an installer that never created the folder; a fresh install
# grants the same from installer.iss.  Users get modify and not full
# control: the file is what SYSTEM reads at the sign-in screen, and its
# permissions are not something a standard account should be able to change.
function Grant-SettingsFolder {
    if (-not $machineSettingsFile) { return }
    $folder = Split-Path -Parent $machineSettingsFile
    try {
        if (-not (Test-Path -LiteralPath $folder)) { New-Item -ItemType Directory -Path $folder -Force | Out-Null }
        & "$env:SystemRoot\System32\icacls.exe" $folder /grant '*S-1-5-32-545:(OI)(CI)M' /Q | Out-Null
    } catch {}
}
# This person's settings, arrived as an argument ("Name=Value;..."), into
# the machine's file -- and whatever HKLM still holds from before the files,
# seeded into it first and then removed, so a deleted file means the
# defaults rather than a mirror from months ago coming back.  The values
# travel as an argument rather than being re-read on the other side: the
# elevated process's own files belong to whichever account answered the
# prompt, which need not be this one.  Both registry views, the PowerShell
# 2.0 way: the Wow6432Node path is the 32-bit view.
function Set-MachineSettings([string]$pairs) {
    if (-not $machineSettingsFile) { return }
    $table = Read-SettingsFile $machineSettingsFile
    $present = @{}; foreach ($entry in $table) { $present[$entry.Name] = $true }
    $hklmPaths = @('HKLM:\SOFTWARE\outSPOKEN SAPI', 'HKLM:\SOFTWARE\Wow6432Node\outSPOKEN SAPI')
    foreach ($path in $hklmPaths) {
        if (-not (Test-Path -LiteralPath $path)) { continue }
        $key = $null
        try { $key = Get-Item -LiteralPath $path -ErrorAction Stop } catch { continue }
        try {
            foreach ($name in $SettingNames) {
                if ($present[$name] -or (-not ($key.GetValueNames() -contains $name))) { continue }
                if ($key.GetValueKind($name).ToString() -ne (Get-SettingKind $name)) { continue }
                if ((Get-SettingKind $name) -eq 'DWord') { $value = [int]$key.GetValue($name) } else { $value = [string]$key.GetValue($name) }
                [void]$table.Add(@{ Name = $name; Value = $value }); $present[$name] = $true
            }
        } finally { $key.Close() }
    }
    foreach ($pair in @($pairs -split ';')) {
        if (-not $pair) { continue }
        $split = $pair -split '=', 2
        if ($split.Length -ne 2) { continue }
        $name = $split[0]; $value = $split[1]
        $kind = Get-SettingKind $name
        if (-not $kind) { continue }
        if ($kind -eq 'DWord') {
            if ($value -notmatch '^\d{1,9}$') { continue }
            $typed = [int]$value
        } else { $typed = [string]$value }
        $table = Set-TableValue $table $name $typed
    }
    if (-not (Write-SettingsFile $machineSettingsFile $table)) { return }
    foreach ($path in $hklmPaths) {
        if (-not (Test-Path -LiteralPath $path)) { continue }
        foreach ($name in $SettingNames) {
            Remove-ItemProperty -LiteralPath $path -Name $name -ErrorAction SilentlyContinue
        }
    }
}
