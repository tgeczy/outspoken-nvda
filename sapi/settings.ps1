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
param([switch]$Plan,[string]$DataRoot)
Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing
if (-not $Plan) { [Windows.Forms.Application]::EnableVisualStyles() }

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

# The machine-wide DataPath, both registry views, read the PowerShell 2.0
# way: on a 64-bit OS the Wow6432Node path IS the 32-bit view, written
# directly.  `HKLM\Software` is redirected under WOW64 and `HKCU\Software`
# is not, so a machine-wide value written to one view alone is perfectly
# present and entirely invisible to half the programs on the machine --
# the trap Panthera shipped into and caught the same week.
$machinePrefPaths = @('HKLM:\SOFTWARE\outSPOKEN SAPI',
                      'HKLM:\SOFTWARE\Wow6432Node\outSPOKEN SAPI')
function Get-MachineDataPath {
    foreach ($path in $machinePrefPaths) {
        try {
            $value = (Get-ItemProperty -Path $path -Name DataPath -ErrorAction Stop).DataPath
            if ($value) { return $value }
        } catch {}
    }
    $null
}

# Does this root actually hold ROMs?  **Not "is the folder there".**
#
# An emptied folder still stands, and every existence check is satisfied by
# it -- Panthera went silent twice in one night because "is it there" was
# asked where "is anything in it" was meant.  The known tree locations are
# checked first so that a root like `%APPDATA%\nvda`, which also holds a
# 717 MB Leopard tree, is never walked whole.
function Test-OspRoot([string]$root) {
    if (-not $root) { return $false }
    if (-not (Test-Path -LiteralPath $root)) { return $false }
    $trees = @((Join-Path $root 'macintalk\outspoken'),
               (Join-Path $root 'outspoken'),
               (Join-Path $root 'outspoken-roms'))
    $narrowed = @()
    foreach ($tree in $trees) {
        if (Test-Path -LiteralPath $tree) { $narrowed += $tree }
    }
    if (-not $narrowed.Length) { $narrowed = @($root) }
    foreach ($tree in $narrowed) {
        $hit = @(Get-ChildItem -Path $tree -Recurse -Filter 'DRVR_1030.bin' -ErrorAction SilentlyContinue)
        if ($hit.Length) { return $true }
    }
    $false
}

# The places the ROMs are kept, best first -- and a root with ROMs in it
# beats a root that merely exists.  Two passes over one list, ported from
# Panthera 2.0.0 with its scars: the machine-wide folder is NVDA's own
# folder name at the root of the machine (`%ProgramData%\macintalk`), and
# bare `%APPDATA%` covers the shared folder kept outside NVDA's
# configuration directory.  `outspoken-data` stays searched forever; only
# what a fresh install *creates* uses the one shared name.
function Resolve-DataRoot {
    # An explicit -DataRoot pins the answer: what lets the classifier be
    # held still by tests, on machines whose registry says anything at all.
    if ($DataRoot) { return $DataRoot }
    $candidates = @()
    $remembered = Load-Pref 'DataPath' $null
    if ($remembered) { $candidates += $remembered }
    $machine = Get-MachineDataPath
    if ($machine) { $candidates += $machine }
    $candidates += (Join-Path $env:APPDATA 'nvda')
    if ($env:ProgramData) { $candidates += $env:ProgramData }
    $candidates += $env:APPDATA
    $candidates += (Join-Path $env:APPDATA 'outspoken-data')
    foreach ($c in $candidates) { if (Test-OspRoot $c) { return $c } }
    if ($remembered) { return $remembered }
    foreach ($c in $candidates) {
        if (Test-Path -LiteralPath $c) {
            # A bare profile root with no tree under it is not an answer
            # anybody typed in; skip to something meaningful.
            if (($c -eq $env:APPDATA) -or ($c -eq $env:ProgramData)) { continue }
            return $c
        }
    }
    # Nothing anywhere: a fresh install, and it gets the one shared name.
    Join-Path $env:APPDATA 'macintalk\outspoken'
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
    Invoke-Elevated ('-{0} -DataRoot "{1}"' -f $verb,$script:data)
}

# **A cancelled elevation prompt is not a yes.**  `-Verb RunAs` throws a
# non-terminating error when somebody backs out, `$process` stays null, and
# `$null.ExitCode` is null -- which every caller here read as success:
# "voices were registered" after a prompt nobody accepted.  Backing out is
# now its own answer, -1, and callers treat it as "do nothing and say
# nothing", because they did not decline the work, they closed a prompt.
function Invoke-Elevated([string]$switches) {
    $arguments = '-NoProfile -WindowStyle Hidden -ExecutionPolicy Bypass -File "{0}" {1}' -f $settingsScript,$switches
    $process = $null
    try {
        $process = Start-Process powershell.exe -Verb RunAs -Wait -PassThru -ArgumentList $arguments -ErrorAction Stop
    } catch { return -1 }
    if (-not $process) { return -1 }
    $process.ExitCode
}

# Where the outspoken tree itself sits under a data root: the shared-name
# subfolder, the pre-0.9.0 name, or the root itself when the ROMs are loose
# in it (`outspoken-data` style).
function Get-OspTree([string]$root) {
    if (-not $root) { return $null }
    $shared = Join-Path $root 'macintalk\outspoken'
    if (Test-Path -LiteralPath $shared) { return $shared }
    # A root that IS the shared macintalk folder: its outspoken subfolder is
    # the tree, and taking only that is what keeps Panthera's generations
    # beside it out of everybody's way.
    $inside = Join-Path $root 'outspoken'
    if (Test-Path -LiteralPath $inside) { return $inside }
    $legacy = Join-Path $root 'outspoken-roms'
    if (Test-Path -LiteralPath $legacy) { return $legacy }
    $root
}

function Test-PathUnder([string]$path, [string]$base) {
    if ((-not $path) -or (-not $base)) { return $false }
    $p = [System.IO.Path]::GetFullPath($path).TrimEnd('\') + '\'
    $b = [System.IO.Path]::GetFullPath($base).TrimEnd('\') + '\'
    $p.ToLower().StartsWith($b.ToLower())
}

# **The other add-on's data is not ours to move.**  The shared `macintalk`
# folder holds Panthera's generations beside this add-on's ROMs, and a move
# that swept them along would silence four synthesizers that belong to a
# different program -- so only the `outspoken` subtree ever moves, and a
# source that turns out to contain generation folders is refused outright
# rather than trimmed.  Panthera's own mover got the mirror-image rule the
# same day.
$PantheraFolders = @('tiger','leopard','snowleopard','lion')
function Test-HoldsPanthera([string]$tree) {
    foreach ($name in $PantheraFolders) {
        if (Test-Path -LiteralPath (Join-Path $tree $name)) { return $true }
    }
    $false
}

# What the move button would do, decided without doing it -- the shape that
# let Panthera's classifier be held still by tests, ported.  Returns a
# hashtable: action (none|done|nvda|chosen|panthera|move), from, to, reason.
function Get-MovePlan {
    $plan = @{ action='none'; from=$script:data; to=''; reason='' }
    if (-not $env:ProgramData) {
        $plan.reason = 'this machine has no ProgramData folder'; return $plan
    }
    $plan.to = Join-Path $env:ProgramData 'macintalk\outspoken'
    if (-not (Test-OspRoot $script:data)) {
        $plan.reason = 'no speech data was found to move'; return $plan
    }
    $tree = Get-OspTree $script:data
    $plan.from = $tree
    if (Test-HoldsPanthera $tree) {
        $plan.action = 'panthera'
        $plan.reason = 'this folder also holds Panthera speech data'
        return $plan
    }
    if (Test-PathUnder $tree $env:ProgramData) {
        $plan.action = 'done'
        $plan.reason = 'the data is already in the machine-wide folder'
        return $plan
    }
    if (Test-PathUnder $tree (Join-Path $env:APPDATA 'nvda')) {
        $plan.action = 'nvda'
        $plan.reason = 'the data belongs to NVDA and moving it breaks the sign-in screen and portable copies'
        return $plan
    }
    if (-not (Test-PathUnder $tree $env:APPDATA)) {
        $plan.action = 'chosen'
        $plan.reason = 'this folder was chosen deliberately'
        return $plan
    }
    $plan.action = 'move'
    $plan.reason = 'the data is in a folder only this account can read'
    $plan
}

if ($Plan) {
    $p = Get-MovePlan
    Write-Output ('plan: {0}' -f $p.action)
    Write-Output ('from: {0}' -f $p.from)
    Write-Output ('to: {0}' -f $p.to)
    Write-Output ('reason: {0}' -f $p.reason)
    exit 0
}

$form = New-Object Windows.Forms.Form
$form.Text = 'outSPOKEN SAPI settings'; $form.Size = New-Object Drawing.Size(640,460)
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
    # Read aloud, so "1 voices" is not good enough -- and "0 voice(s) found"
    # is a true sentence that reads like a working program.  Nothing found is
    # the one state worth naming, and nothing found while tokens exist is the
    # one worth naming loudly: those voices sit in every program's list and
    # say nothing at all.
    if ($ids.Length) {
        $s = 's'
        if ($ids.Length -eq 1) { $s = '' }
        $status.Text = '{0} voice{1} found in {2}; {3}.' -f $ids.Length,$s,$script:data,$registered
    } elseif (Test-AnyTokens) {
        $status.Text = 'Engines are registered with SAPI but no speech data was found in {0} - the voices will say nothing until it is back. Use Data location, or extract engines with the NVDA add-on''s Tools menu or tools\extract_rom.py.' -f $script:data
    } else {
        $status.Text = 'No speech data found in {0}. Extract engines with the NVDA add-on''s Tools menu or tools\extract_rom.py.' -f $script:data
    }
}

# **The voices point at data that is not there any more.**  Tokens carry a
# DataPath written once, at registration; the data can move afterwards and
# nothing notices on its own -- SAPI lists every voice, hands them text, and
# the bridge renders nothing.  The bridge enumerating through the same
# resolver this window uses is the witness: tokens present, zero voices
# enumerable, means something was lost.  Somebody with engines simply not
# installed has no tokens and is never asked (registering them is what a
# token *means*), which is what keeps this quiet for the person who owns
# one disc on purpose.
function Offer-Rebind {
    if (-not (Test-AnyTokens)) { return }
    if (@(Get-BridgeVoiceIds).Length) { return }
    $message = "Your outSPOKEN voices are registered, but no speech data was found where they expect it:`n`n{0}`n`nUntil this is fixed they will appear in every program's voice list and say nothing at all.`n`nFind the folder now?" -f $script:data
    $answer = [Windows.Forms.MessageBox]::Show($form,$message,'outSPOKEN SAPI','YesNo','Warning')
    if ($answer -eq 'Yes') {
        $chooseRoot.PerformClick()
        return
    }
    # Saying no leaves dead voices in every list; offer to clear them out.
    # Asked rather than done -- it costs an elevation prompt, and they may be
    # about to plug in the drive the data lives on.
    $message = "Remove the outSPOKEN voices for now?`n`nThey can be registered again from this window the moment the data is back. This needs administrator permission."
    $answer = [Windows.Forms.MessageBox]::Show($form,$message,'outSPOKEN SAPI','YesNo','Question')
    if ($answer -ne 'Yes') { return }
    $code = Invoke-Registration 'Unregister'
    if ($code -gt 0) {
        [Windows.Forms.MessageBox]::Show($form,'The voices could not be removed. Use Unregister to try again.','outSPOKEN SAPI','OK','Error') | Out-Null
    } elseif ($code -eq 0) {
        [Windows.Forms.MessageBox]::Show($form,'The voices are no longer offered to your programs. Register them again once the data is back.','outSPOKEN SAPI') | Out-Null
        Refresh-Voices
    }
}

function Offer-NewData {
    if (Load-Pref 'DeclinedOffer' 0) { return }
    if (Test-AnyTokens) { return }
    if (-not @(Get-BridgeVoiceIds).Length) { return }
    $message = "outSPOKEN voice data is installed but not registered with SAPI.`n`nRegister it now? (Choosing No will not ask again; the Register button always works.)"
    $answer = [Windows.Forms.MessageBox]::Show($form,$message,'outSPOKEN SAPI','YesNo','Question')
    if ($answer -eq 'Yes') {
        $code = Invoke-Registration 'Register'
        if ($code -gt 0) {
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
$move = New-Object Windows.Forms.Button
$move.Text = 'Move engines for &all users...'
$move.AccessibleName = 'Move engines for all users'
$move.AccessibleDescription = 'Move the speech data to a folder every Windows account on this machine can read'
$move.Location = New-Object Drawing.Point(12,300); $move.AutoSize=$true
$updates = New-Object Windows.Forms.Button
$updates.Text = 'Check for SAPI &updates...'
$updates.AccessibleName = 'Check for SAPI updates'
$updates.Location = New-Object Drawing.Point(200,300); $updates.AutoSize=$true
$close = New-Object Windows.Forms.Button; $close.Text = '&Close'; $close.Location = New-Object Drawing.Point(470,262); $close.AutoSize=$true

# **Check for updates, the way the NVDA add-on's button does it**: fetch the
# installer itself and run it, rather than sending somebody to a web page to
# find the right file among a release's assets.  The installer's own UI --
# and its UAC prompt -- are still the things that ask; pressing the button
# is the consent to look, and nothing here runs on its own.
#
# PowerShell 2.0 throughout, like the rest of this file: the JSON is read
# with regular expressions because ConvertFrom-Json is 3.0, and the fetch is
# WebClient because Invoke-WebRequest is too.  On a stock Windows 7 whose
# .NET cannot speak TLS 1.2 the check fails with words rather than silence,
# which is the honest best available there.
function Get-InstalledSapiVersion {
    foreach ($path in @(
        'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\{4D6071E1-B142-4F49-8C5C-97C661EA748B}_is1',
        'HKLM:\SOFTWARE\Wow6432Node\Microsoft\Windows\CurrentVersion\Uninstall\{4D6071E1-B142-4F49-8C5C-97C661EA748B}_is1')) {
        try {
            $v = (Get-ItemProperty -Path $path -Name DisplayVersion -ErrorAction Stop).DisplayVersion
            if ($v) { return $v }
        } catch {}
    }
    $null
}

function Compare-Versions([string]$a, [string]$b) {
    # -> 1 when $a is newer, -1 when older, 0 when the same; digits only, so
    # a tag prefix or suffix never makes a version look newer.
    $pa = @(); $pb = @()
    if ($a -match '(\d+(?:\.\d+)*)') { $pa = @($matches[1] -split '\.') }
    if ($b -match '(\d+(?:\.\d+)*)') { $pb = @($matches[1] -split '\.') }
    if ((-not $pa.Length) -or (-not $pb.Length)) { return 0 }
    $width = [Math]::Max($pa.Length, $pb.Length)
    for ($i = 0; $i -lt $width; $i++) {
        $x = 0; $y = 0
        if ($i -lt $pa.Length) { $x = [int]$pa[$i] }
        if ($i -lt $pb.Length) { $y = [int]$pb[$i] }
        if ($x -gt $y) { return 1 }
        if ($x -lt $y) { return -1 }
    }
    0
}

$updates.Add_Click({
    $status.Text = 'Checking for updates...'
    $form.Refresh()
    # GitHub is TLS 1.2 or nothing; older .NET defaults to less.  Additive,
    # and forgiven where the enum does not exist.
    try { [Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor 3072 } catch {}
    $tag = $null; $asset = $null; $problem = $null
    try {
        $wc = New-Object Net.WebClient
        $wc.Headers.Add('User-Agent','outspoken-sapi-settings')
        $json = $wc.DownloadString('https://api.github.com/repos/tgeczy/outspoken-nvda/releases/latest')
        if ($json -match '"tag_name"\s*:\s*"([^"]+)"') { $tag = $matches[1] }
        if ($json -match '"browser_download_url"\s*:\s*"([^"]*-setup\.exe)"') { $asset = $matches[1] }
    } catch { $problem = $_.Exception.Message }
    $status.Text = ''
    if ($problem -or (-not $tag)) {
        if (-not $problem) { $problem = 'the newest release could not be read' }
        [Windows.Forms.MessageBox]::Show($form,("Could not check for updates:`n`n{0}" -f $problem),'outSPOKEN SAPI','OK','Warning') | Out-Null
        return
    }
    $installed = Get-InstalledSapiVersion
    if (-not $installed) {
        [Windows.Forms.MessageBox]::Show($form,("The newest release is {0}. No installed copy was found to compare against." -f $tag),'outSPOKEN SAPI','OK','Information') | Out-Null
        return
    }
    if ((Compare-Versions $tag $installed) -le 0) {
        [Windows.Forms.MessageBox]::Show($form,("You have the newest version, {0}." -f $installed),'outSPOKEN SAPI','OK','Information') | Out-Null
        return
    }
    if (-not $asset) {
        [Windows.Forms.MessageBox]::Show($form,("A newer version exists ({0}), but its installer could not be found on the release. Visit the releases page to download it." -f $tag),'outSPOKEN SAPI','OK','Warning') | Out-Null
        return
    }
    $answer = [Windows.Forms.MessageBox]::Show($form,("A newer version is available: {0}. You have {1}.`n`nDownload and run the installer now? It will ask before changing anything." -f $tag,$installed),'outSPOKEN SAPI','YesNo','Question')
    if ($answer -ne 'Yes') { return }
    $parts = $asset -split '/'
    $file = Join-Path $env:TEMP $parts[$parts.Length - 1]
    $status.Text = 'Downloading the update...'
    $form.Refresh()
    try {
        # The old file goes first, so a half-written download from a failed
        # attempt is never the thing that runs.
        if (Test-Path -LiteralPath $file) { Remove-Item -Path $file -Force }
        $wc = New-Object Net.WebClient
        $wc.Headers.Add('User-Agent','outspoken-sapi-settings')
        $wc.DownloadFile($asset, $file)
    } catch {
        $status.Text = ''
        [Windows.Forms.MessageBox]::Show($form,("The update could not be downloaded:`n`n{0}" -f $_.Exception.Message),'outSPOKEN SAPI','OK','Warning') | Out-Null
        return
    }
    $status.Text = ''
    try { Start-Process -FilePath $file } catch {
        [Windows.Forms.MessageBox]::Show($form,("The update was downloaded but could not be started. It is saved at:`n`n{0}" -f $file),'outSPOKEN SAPI','OK','Warning') | Out-Null
    }
})

# **A button, not a prompt.**  `%APPDATA%` is read perfectly well from the
# sign-in screen -- NVDA runs there as SYSTEM, which can read any profile --
# so the machine-wide folder buys one copy shared between accounts: worth
# offering, not worth interrupting anybody about.  A press is consent, and
# because somebody pressed it deliberately, every answer other than "yes,
# and here is what will move" owes them a reason.
$move.Add_Click({
    $p = Get-MovePlan
    if ($p.action -ne 'move') {
        $why = 'There is no speech data to move: {0}.' -f $p.reason
        if ($p.action -eq 'done')   { $why = 'The speech data is already in {0}, where every account on this machine can read it.' -f $p.from }
        if ($p.action -eq 'nvda')   { $why = "This data belongs to NVDA, in its own configuration folder.`n`nIt is not moved from there: NVDA copies that folder to the Windows sign-in screen and carries it into a portable copy, and moving it out is what breaks both." }
        if ($p.action -eq 'chosen') { $why = "The speech data is in a folder you chose:`n`n{0}`n`nThat is left where you put it. Use Data location to point the voices somewhere else." -f $p.from }
        if ($p.action -eq 'panthera') { $why = "This folder also holds Panthera speech data:`n`n{0}`n`nMoving it would silence another add-on's voices, so nothing was moved. Panthera's own settings tool is the right place to move Panthera's data." -f $p.from }
        [Windows.Forms.MessageBox]::Show($form,$why,'outSPOKEN SAPI','OK','Information') | Out-Null
        return
    }
    $message = "Move the outSPOKEN speech data so every account on this machine can use it?`n`nFrom: {0}`nTo: {1}`n`nOnly outSPOKEN's own folder moves; anything else kept beside it stays where it is. This needs administrator permission." -f $p.from,$p.to
    $answer = [Windows.Forms.MessageBox]::Show($form,$message,'outSPOKEN SAPI','YesNo','Question')
    if ($answer -ne 'Yes') { return }
    $code = Invoke-Elevated ('-Move -MoveFrom "{0}" -MoveTo "{1}"' -f $p.from,$p.to)
    if ($code -eq -1) { return }
    if ($code -eq 5) {
        $script:data = $env:ProgramData
        Save-Pref 'DataPath' $script:data
        [Windows.Forms.MessageBox]::Show($form,('The data moved to {0}, but registering the voices from it failed. Use Register to try again.' -f $p.to),'outSPOKEN SAPI','OK','Warning') | Out-Null
        Refresh-Voices
        return
    }
    if ($code) {
        [Windows.Forms.MessageBox]::Show($form,'The speech data could not be moved, and nothing was changed. Anything speaking with an outSPOKEN voice right now will be holding the files open - close it and try again.','outSPOKEN SAPI','OK','Error') | Out-Null
        return
    }
    $script:data = $env:ProgramData
    if (Load-Pref 'DataPath' $null) { Save-Pref 'DataPath' $script:data }
    Refresh-Voices
    [Windows.Forms.MessageBox]::Show($form,('The speech data now lives in {0}, where every account on this machine can read it.' -f $p.to),'outSPOKEN SAPI') | Out-Null
})

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
            $code = Invoke-Registration 'Register'
            if ($code -gt 0) {
                [Windows.Forms.MessageBox]::Show($form,'The folder was remembered, but re-registering from it failed. Use Register to try again.','outSPOKEN SAPI','OK','Error') | Out-Null
            } elseif ($code -eq 0) {
                [Windows.Forms.MessageBox]::Show($form,'Voices are now registered from the new folder.','outSPOKEN SAPI') | Out-Null
            }
            Refresh-Voices
        }
    }
})
$open.Add_Click({ New-Item -ItemType Directory -Force $script:data | Out-Null; Start-Process explorer.exe -ArgumentList ('"{0}"' -f $script:data) })
$register.Add_Click({
    # **Register what, exactly?**  A root with no speech data has no voices
    # to register, and elevating anyway ends with "voices were registered"
    # and a list that still says not installed -- the lie Panthera caught.
    # Checked here so a pointless registration costs no UAC prompt at all.
    if (-not @(Get-BridgeVoiceIds).Length) {
        [Windows.Forms.MessageBox]::Show($form,('There is no speech data in {0}, so there is nothing to register. Extract engines first, or use Data location if they are somewhere this tool has not looked.' -f $script:data),'outSPOKEN SAPI','OK','Information') | Out-Null
        return
    }
    $code = Invoke-Registration 'Register'
    if ($code -gt 0) { [Windows.Forms.MessageBox]::Show($form,'Registration failed.','outSPOKEN SAPI','OK','Error') | Out-Null }
    elseif ($code -eq 0) {
        Save-Pref 'DeclinedOffer' 0
        [Windows.Forms.MessageBox]::Show($form,'outSPOKEN voices were registered for 32-bit and 64-bit SAPI.','outSPOKEN SAPI') | Out-Null
    }
    Refresh-Voices
})
$unregister.Add_Click({
    $code = Invoke-Registration 'Unregister'
    if ($code -gt 0) { [Windows.Forms.MessageBox]::Show($form,'Unregistration failed.','outSPOKEN SAPI','OK','Error') | Out-Null }
    elseif ($code -eq 0) {
        Save-Pref 'DeclinedOffer' 1
        [Windows.Forms.MessageBox]::Show($form,'outSPOKEN voices were unregistered.','outSPOKEN SAPI') | Out-Null
    }
    Refresh-Voices
})
$close.Add_Click({ $form.Close() })
$form.CancelButton = $close
$form.Controls.AddRange(@($label,$list,$status,$chooseRoot,$open,$register,$unregister,$move,$updates,$close))
Refresh-Voices; $form.Add_Shown({ $list.Focus(); Offer-Rebind; Offer-NewData }); [void]$form.ShowDialog()
