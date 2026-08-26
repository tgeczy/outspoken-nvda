# outSPOKEN SAPI settings -- registration and data management.
#
# A trimmed sibling of panthera-speech's settings app: the voice list comes
# from the serve bridge itself (the one authority on what the data provides),
# registration goes through register.ps1 elevated, and new data offers
# itself once with a remembered "no".  No per-engine settings rows on
# purpose -- rate, pitch and volume are SAPI's own, and everything else is
# the NVDA driver's decision, byte-identical through the bridge.
#
# Written in PowerShell 2.0's dialect, like register.ps1: stock Windows 7
# has no newer engine.  No $PSScriptRoot, no [ordered], no [pscustomobject],
# no simplified Where-Object, no OpenBaseKey -- the 32-bit registry view is
# the Wow6432Node path, read directly.
Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing
[Windows.Forms.Application]::EnableVisualStyles()

$stage = Split-Path -Parent $MyInvocation.MyCommand.Path
$settingsScript = Join-Path $stage 'register.ps1'
$prefKey = 'HKCU:\Software\outSPOKEN SAPI'

function Load-Pref([string]$name, $default) {
    try { (Get-ItemProperty -Path $prefKey -Name $name -ErrorAction Stop).$name }
    catch { $default }
}
function Save-Pref([string]$name, $value) {
    New-Item -Path $prefKey -Force | Out-Null
    Set-ItemProperty -Path $prefKey -Name $name -Value $value
}

function Resolve-DataRoot {
    $remembered = Load-Pref 'DataPath' $null
    if ($remembered) { return $remembered }
    $nvda = Join-Path $env:APPDATA 'nvda'
    if ((Test-Path (Join-Path $nvda 'macintalk\outspoken')) -or
        (Test-Path (Join-Path $nvda 'outspoken-roms'))) { return $nvda }
    Join-Path $env:APPDATA 'outspoken-data'
}
$script:data = Resolve-DataRoot

#: Engine families in display order, as parallel arrays: 2.0 has no
#: [ordered] and a plain hashtable shuffles.
$FamilyKeys = @('mt1','mtk2','mtk3','gala')
$FamilyNames = @('MacinTalk 1 (1984)','MacinTalk 2','MacinTalk 3','MacinTalk Pro')

function Get-BridgeVoiceIds {
    $py = Join-Path $stage 'python\python.exe'
    if (-not (Test-Path $py)) { $py = 'python' }
    $ids = @()
    try {
        foreach ($line in @(& $py (Join-Path $stage 'osp_serve.py') --list $script:data 2>$null)) {
            if ($line -match "`t") { $ids += ($line -split "`t")[0] }
        }
    } catch {}
    $ids
}

function Get-Family([string]$id) {
    if ($id -match '^(\w+):') { return $matches[1] }
    'mt1'
}

function Test-AnyTokens {
    $roots = @('HKLM:\SOFTWARE\Microsoft\Speech\Voices\Tokens',
               'HKLM:\SOFTWARE\Wow6432Node\Microsoft\Speech\Voices\Tokens')
    foreach ($root in $roots) {
        if (-not (Test-Path $root)) { continue }
        foreach ($key in @(Get-ChildItem $root -ErrorAction SilentlyContinue)) {
            if ($key.PSChildName -like 'Outspoken_*') { return $true }
        }
    }
    $false
}

function Invoke-Registration([string]$verb) {
    $arguments = '-NoProfile -WindowStyle Hidden -ExecutionPolicy Bypass -File "{0}" -{1} -DataRoot "{2}"' -f $settingsScript,$verb,$script:data
    $process = Start-Process powershell.exe -Verb RunAs -Wait -PassThru -ArgumentList $arguments
    $process.ExitCode
}

$form = New-Object Windows.Forms.Form
$form.Text = 'outSPOKEN SAPI settings'; $form.Size = New-Object Drawing.Size(640,420)
$form.StartPosition = 'CenterScreen'
$label = New-Object Windows.Forms.Label
$label.Text = 'Macintosh speech engines:'; $label.AutoSize = $true; $label.Location = New-Object Drawing.Point(12,14)
$list = New-Object Windows.Forms.ListBox
$list.Name = 'engineList'; $list.AccessibleName = 'Macintosh speech engines'
$list.AccessibleDescription = 'MacinTalk engine installation status by generation'
$list.Location = New-Object Drawing.Point(12,38); $list.Size = New-Object Drawing.Size(600,160)
$status = New-Object Windows.Forms.Label
$status.Name = 'dataStatus'; $status.AccessibleName = 'Speech data status'
$status.Location = New-Object Drawing.Point(12,206); $status.Size = New-Object Drawing.Size(600,44)

function Refresh-Voices {
    $list.Items.Clear()
    $ids = @(Get-BridgeVoiceIds)
    for ($i = 0; $i -lt $FamilyKeys.Length; $i++) {
        $family = $FamilyKeys[$i]
        $count = 0
        foreach ($id in $ids) { if ((Get-Family $id) -eq $family) { $count++ } }
        $state = 'not installed'
        if ($count) { $state = "$count voices" }
        [void]$list.Items.Add(('{0} - {1}' -f $FamilyNames[$i],$state))
    }
    if ($list.Items.Count) { $list.SelectedIndex = 0 }
    $registered = 'not registered'
    if (Test-AnyTokens) { $registered = 'registered with SAPI' }
    $status.Text = '{0} voice(s) found in {1}; {2}. Extract engines with the NVDA add-on''s Tools menu or tools\extract_rom.py.' -f $ids.Length,$script:data,$registered
}

function Offer-NewData {
    if (Load-Pref 'DeclinedOffer' 0) { return }
    if (Test-AnyTokens) { return }
    if (-not @(Get-BridgeVoiceIds).Length) { return }
    $message = "outSPOKEN voice data is installed but not registered with SAPI.`n`nRegister it now? (Choosing No will not ask again; the Register button always works.)"
    $answer = [Windows.Forms.MessageBox]::Show($form,$message,'outSPOKEN SAPI','YesNo','Question')
    if ($answer -eq 'Yes') {
        if (Invoke-Registration 'Register') {
            [Windows.Forms.MessageBox]::Show($form,'Registration failed. Use Register to try again.','outSPOKEN SAPI','OK','Error') | Out-Null
        }
        Refresh-Voices
    } else {
        Save-Pref 'DeclinedOffer' 1
    }
}

$chooseRoot = New-Object Windows.Forms.Button; $chooseRoot.Text = 'Data &location...'; $chooseRoot.Location = New-Object Drawing.Point(12,262); $chooseRoot.AutoSize=$true
$open = New-Object Windows.Forms.Button; $open.Text = '&Open data folder'; $open.Location = New-Object Drawing.Point(130,262); $open.AutoSize=$true
$register = New-Object Windows.Forms.Button; $register.Text = '&Register'; $register.Location = New-Object Drawing.Point(270,262); $register.AutoSize=$true
$unregister = New-Object Windows.Forms.Button; $unregister.Text = '&Unregister'; $unregister.Location = New-Object Drawing.Point(360,262); $unregister.AutoSize=$true
$close = New-Object Windows.Forms.Button; $close.Text = '&Close'; $close.Location = New-Object Drawing.Point(470,262); $close.AutoSize=$true

$chooseRoot.Add_Click({
    $browser = New-Object Windows.Forms.FolderBrowserDialog
    $browser.Description = 'Choose the folder that holds the extracted outSPOKEN engines (outspoken-roms or macintalk\outspoken lives inside it).'
    if (Test-Path -LiteralPath $script:data) { $browser.SelectedPath = $script:data }
    if ($browser.ShowDialog($form) -eq 'OK') {
        $script:data = $browser.SelectedPath
        Save-Pref 'DataPath' $script:data
        Refresh-Voices
        if (Test-AnyTokens) {
            # Registered tokens carry the old DataPath; follow the data.
            if (Invoke-Registration 'Register') {
                [Windows.Forms.MessageBox]::Show($form,'The folder was remembered, but re-registering from it failed. Use Register to try again.','outSPOKEN SAPI','OK','Error') | Out-Null
            } else {
                [Windows.Forms.MessageBox]::Show($form,'Voices are now registered from the new folder.','outSPOKEN SAPI') | Out-Null
            }
            Refresh-Voices
        }
    }
})
$open.Add_Click({ New-Item -ItemType Directory -Force $script:data | Out-Null; Start-Process explorer.exe -ArgumentList ('"{0}"' -f $script:data) })
$register.Add_Click({
    if (Invoke-Registration 'Register') { [Windows.Forms.MessageBox]::Show($form,'Registration failed.','outSPOKEN SAPI','OK','Error') | Out-Null }
    else {
        Save-Pref 'DeclinedOffer' 0
        [Windows.Forms.MessageBox]::Show($form,'outSPOKEN voices were registered for 32-bit and 64-bit SAPI.','outSPOKEN SAPI') | Out-Null
    }
    Refresh-Voices
})
$unregister.Add_Click({
    if (Invoke-Registration 'Unregister') { [Windows.Forms.MessageBox]::Show($form,'Unregistration failed.','outSPOKEN SAPI','OK','Error') | Out-Null }
    else {
        Save-Pref 'DeclinedOffer' 1
        [Windows.Forms.MessageBox]::Show($form,'outSPOKEN voices were unregistered.','outSPOKEN SAPI') | Out-Null
    }
    Refresh-Voices
})
$close.Add_Click({ $form.Close() })
$form.CancelButton = $close
$form.Controls.AddRange(@($label,$list,$status,$chooseRoot,$open,$register,$unregister,$close))
Refresh-Voices; $form.Add_Shown({ $list.Focus(); Offer-NewData }); [void]$form.ShowDialog()
