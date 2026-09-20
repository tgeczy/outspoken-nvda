# -*- coding: utf-8 -*-
"""The installer's first-install/upgrade decision, run in an isolated Inno setup.

An installer upgrade must refresh the COM classes and leave the voice tokens
alone: which voices are registered, and from which folder, is a choice the
person already made, and rebuilding them from whatever the elevated account
can see is how Panthera's 3.2.0 lost people's choices until its r2 -- the
fault this test keeps out of here, ported with the fix.

Only the registry locations and the subprocess effects are redirected: the
probe installer writes two uniquely named keys under HKCU and nothing else,
and the register script's dispatch runs with its COM registration, its
data-path write and its token pass replaced by counters.  No installed SAPI
token, class, setting or product file is touched.  Needs the Inno Setup
compiler; skipped where it is absent.
"""
import json
import os
import re
import subprocess
import sys
import uuid
from pathlib import Path

import pytest

pytestmark = pytest.mark.skipif(sys.platform != "win32", reason="Windows installer")
ROOT = Path(__file__).resolve().parents[1]
ISCC = Path(os.environ.get("ProgramFiles(x86)", "C:/Program Files (x86)")) / "Inno Setup 6/ISCC.exe"

PROBE = r"""
param([switch]$Register, [switch]$RegisterServer, [string]$DataRoot, [string]$MirrorSettings)
$ErrorActionPreference = 'Stop'
$statePath = Join-Path $PSScriptRoot 'state.json'
$script:state = Get-Content -Raw $statePath | ConvertFrom-Json
$script:classes = 0; $script:added = 0; $script:pathWrites = 0
$stage = $PSScriptRoot; $wow = $true; $reg32 = 'regsvr32.exe'
function Grant-SettingsFolder { }
function Set-MachineSettings($unused) { }
function Resolve-InstallDataRoot($explicit) { 'D:\resolved' }
function Set-MachineDataPath($root) { $script:pathWrites++ }
function Add-Tokens($root) {
    $script:added++
    $script:state.tokens = @(@{ id = 'mtk3:Fred'; dataPath = $root }, @{ id = 'gala:Bruce'; dataPath = $root })
}
$source = Get-Content -Raw -Encoding UTF8 '__REGISTER__'
$tokens = $null; $errors = $null
$ast = [Management.Automation.Language.Parser]::ParseInput($source,[ref]$tokens,[ref]$errors)
if ($errors.Count) { throw $errors[0] }
$dispatch = @($ast.EndBlock.Statements | Where-Object {
    $_ -is [Management.Automation.Language.IfStatementAst] -and
    $_.Clauses[0].Item1.Extent.Text -match '\$Register\b'
})
if ($dispatch.Count -ne 1) { throw 'registration dispatch missing or ambiguous' }
$text = $dispatch[0].Extent.Text
# Mock only the two regsvr32 calls (one through $reg32, one literal); keep
# the real dispatch, the settings calls,
# the resolve/write/add calls and the exit-status checks.
$commands = $dispatch[0].FindAll({param($n)
    $n -is [Management.Automation.Language.CommandAst] -and
    $n.Extent.Text -match '^& (\$reg32\b|"\$env:SystemRoot\\System32\\regsvr32\.exe")'
}, $true)
if ($commands.Count -ne 2) { throw 'expected both COM registration calls' }
foreach ($command in $commands) {
    $text = $text.Replace($command.Extent.Text, '$script:classes++; $global:LASTEXITCODE = 0')
}
$text = $text.Replace('exit 0', 'return')
& ([scriptblock]::Create($text))
@{tokens=$script:state.tokens; classes=$script:classes; added=$script:added; pathWrites=$script:pathWrites} |
    ConvertTo-Json -Depth 8 | Set-Content -Encoding UTF8 $statePath
"""


@pytest.mark.parametrize("previous, tokens", [
    (None, []),
    ("View32", [{"id": "cami:Carlos", "dataPath": "E:/chosen/root"}]),
    ("View64", [{"id": "cami:Carlos", "dataPath": "E:/chosen/root"}]),
    ("View64", []),
    ("View64", [{"id": "male", "dataPath": "E:/a"}, {"id": "mtk2:Ben", "dataPath": "E:/a"}]),
])
def test_an_upgrade_keeps_the_registered_voices(tmp_path, previous, tokens):
    if not ISCC.exists():
        pytest.skip("Inno Setup compiler not installed")
    import winreg

    installer = (ROOT / "sapi/installer.iss").read_text(encoding="utf-8-sig")
    code = installer.split("[Code]", 1)[1]
    key = "Software\\OutspokenTests\\Installer-" + uuid.uuid4().hex
    # The real decision, including its InitializeSetup snapshot, reads two
    # isolated view keys.  The harness writes View32 during installation, so
    # checking too late would break the fresh-install case.
    code = re.sub(r"UninstallKey = '[^']+';", lambda _: "UninstallKey = '" + key + "';", code)
    code = code.replace("RegKeyExists(HKLM32, UninstallKey)", "RegKeyExists(HKCU, UninstallKey + '\\View32')")
    code = code.replace("RegKeyExists(HKLM64, UninstallKey)", "RegKeyExists(HKCU, UninstallKey + '\\View64')")
    run = installer.split("[Run]", 1)[1].split("[UninstallRun]", 1)[0]
    run = "\n".join(line for line in run.splitlines()
                    if line.startswith("Filename:") and "-Register" in line)
    assert len(run.splitlines()) == 2, run
    run = run.replace(r"{app}\register.ps1", str(tmp_path / "probe.ps1"))
    (tmp_path / "probe.ps1").write_text(PROBE.replace("__REGISTER__", str(ROOT / "sapi/register.ps1")),
                                        encoding="utf-8-sig")
    (tmp_path / "state.json").write_text(json.dumps({"tokens": tokens}), encoding="utf8")
    script = f"""[Setup]
AppName=outSPOKEN isolated installer check
AppVersion=1
DefaultDirName={{tmp}}\\OutspokenInstallerCheck
CreateAppDir=no
Uninstallable=no
PrivilegesRequired=lowest
DisableWelcomePage=yes
DisableReadyPage=yes
DisableFinishedPage=yes
OutputDir={tmp_path}
OutputBaseFilename=probe-setup
[Registry]
Root: HKCU; Subkey: "{key}\\View32"
[Run]
{run}
[Code]
{code}
"""
    iss = tmp_path / "probe.iss"
    iss.write_text(script, encoding="utf-8-sig")
    try:
        if previous:
            with winreg.CreateKey(winreg.HKEY_CURRENT_USER, key + "\\" + previous):
                pass
        built = subprocess.run([str(ISCC), str(iss)], capture_output=True, text=True, timeout=120)
        assert built.returncode == 0, built.stdout + built.stderr
        log = tmp_path / "install.log"
        installed = subprocess.run([str(tmp_path / "probe-setup.exe"), "/VERYSILENT",
                                   "/SUPPRESSMSGBOXES", "/SP-", "/NORESTART", "/LOG=" + str(log)],
                                  capture_output=True, timeout=120, creationflags=subprocess.CREATE_NO_WINDOW)
        assert installed.returncode == 0, log.read_text(errors="replace")
        result = json.loads((tmp_path / "state.json").read_text(encoding="utf-8-sig"))
        assert result["classes"] == 2, result
        if previous:
            # Identities and their data paths unchanged, and nothing written.
            assert result["tokens"] == tokens, result
            assert result["added"] == 0 and result["pathWrites"] == 0, result
        else:
            assert result["added"] == 1 and result["pathWrites"] == 1, result
            assert [t["dataPath"] for t in result["tokens"]] == ["D:\\resolved"] * 2, result
    finally:
        # These exact, uniquely named test keys are the only registry writes.
        for leaf in ("View32", "View64"):
            try:
                winreg.DeleteKey(winreg.HKEY_CURRENT_USER, key + "\\" + leaf)
            except FileNotFoundError:
                pass
        try:
            winreg.DeleteKey(winreg.HKEY_CURRENT_USER, key)
        except FileNotFoundError:
            pass
