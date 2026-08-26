param([switch]$Register,[switch]$Unregister,[string]$DataRoot)
# Voice tokens for the outSPOKEN SAPI engine: one per voice the serve
# bridge enumerates from the data root, in both registry views, exactly the
# Panthera model.  Run elevated; the installer does.
$ErrorActionPreference = 'Stop'
$stage = $PSScriptRoot
$clsid = '{a1f4055c-b6c2-4c27-ab6a-af54c409a309}'
# The Panthera resolution order, mirrored: an explicit choice, then the
# remembered one, then NVDA's own config (an NVDA-first user's ROMs are
# already there), then the standalone default.
$prefKey = 'HKCU:\Software\outSPOKEN SAPI'
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

function Remove-Tokens {
    foreach ($view in 'Registry32','Registry64') {
        $base = [Microsoft.Win32.RegistryKey]::OpenBaseKey('LocalMachine',$view)
        $root = $base.CreateSubKey('Software\Microsoft\Speech\Voices\Tokens')
        @($root.GetSubKeyNames()) | Where-Object { $_ -like 'Outspoken_*' } |
            ForEach-Object { $root.DeleteSubKeyTree($_,$false) }
        $root.Dispose(); $base.Dispose()
    }
}

if ($Register) {
    & "$env:SystemRoot\SysWOW64\regsvr32.exe" /s (Join-Path $stage 'x86\outspoken_sapi.dll')
    if ($LASTEXITCODE) { exit $LASTEXITCODE }
    & "$env:SystemRoot\System32\regsvr32.exe" /s (Join-Path $stage 'x64\outspoken_sapi.dll')
    if ($LASTEXITCODE) { exit $LASTEXITCODE }
    # The serve bridge is the one authority on which voices the data
    # provides; registering with no data present is a clean no-op.
    $py = Join-Path $stage 'python\python.exe'
    if (-not (Test-Path $py)) { $py = 'python' }
    $listing = & $py (Join-Path $stage 'osp_serve.py') --list $DataRoot 2>$null
    Remove-Tokens
    foreach ($view in 'Registry32','Registry64') {
        $base = [Microsoft.Win32.RegistryKey]::OpenBaseKey('LocalMachine',$view)
        $root = $base.CreateSubKey('Software\Microsoft\Speech\Voices\Tokens')
        foreach ($line in @($listing)) {
            if ($line -notmatch "`t") { continue }
            $id,$name = $line -split "`t",2
            $key = $root.CreateSubKey('Outspoken_' + ($id -replace '[^A-Za-z0-9]','_'))
            $key.SetValue('', "$name (outSPOKEN)")
            $key.SetValue('CLSID',$clsid)
            $key.SetValue('VoiceId',$id)
            $key.SetValue('DataPath',$DataRoot)
            $attributes = $key.CreateSubKey('Attributes')
            $attributes.SetValue('Name',$name)
            $attributes.SetValue('Vendor','outSPOKEN')
            $attributes.SetValue('Language','409')
            $attributes.SetValue('Gender','Neutral')
            $attributes.Dispose(); $key.Dispose()
        }
        $root.Dispose(); $base.Dispose()
    }
    exit 0
}
if ($Unregister) {
    Remove-Tokens
    & "$env:SystemRoot\SysWOW64\regsvr32.exe" /s /u (Join-Path $stage 'x86\outspoken_sapi.dll')
    & "$env:SystemRoot\System32\regsvr32.exe" /s /u (Join-Path $stage 'x64\outspoken_sapi.dll')
    exit 0
}
Write-Host 'Use -Register or -Unregister (optionally -DataRoot <folder>).'
