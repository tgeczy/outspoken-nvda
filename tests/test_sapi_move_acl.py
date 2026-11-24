# -*- coding: utf-8 -*-
"""Move to all users (register.ps1 -Move) leaves every file readable.

The mover locks the shared `macintalk` root -- readable by everybody, writable
by an administrator alone -- and used to do it with the grants sent down the
tree (/T).  On a file that left an empty ACL: the inherited entries removed,
and a grant carrying (OI)(CI) takes nothing on a file.  Every ROM it moved,
and every Panthera voice bank already under the shared root, was then
unreadable by everybody, SYSTEM included.

The real -Move block runs here, taken out of register.ps1 by its syntax tree
as test_sapi_installer_upgrade.py takes the dispatch, with the settings calls
stubbed and the re-registration it starts answered at once.  The folders are
temporary; nothing of the installed product is touched.  Then: the moved ROM
and Panthera's bank both readable by Users, and Users unable to write in the
shared root.
"""
import shutil
import subprocess
import sys
import uuid
from pathlib import Path

import pytest

pytestmark = pytest.mark.skipif(sys.platform != "win32", reason="Windows ACLs")
ROOT = Path(__file__).resolve().parents[1]

# The probe writes the real -Move block into a script of its own (the move starts that same script again with
# -Register to re-register, by its own path) and runs it with the folders given.
PROBE = r"""
param([string]$MoveFrom, [string]$MoveTo)
$ErrorActionPreference = 'Stop'
$source = Get-Content -Raw -Encoding UTF8 '__REGISTER__'
$tokens = $null; $errors = $null
$ast = [Management.Automation.Language.Parser]::ParseInput($source,[ref]$tokens,[ref]$errors)
if ($errors.Count) { throw $errors[0] }
$block = @($ast.EndBlock.Statements | Where-Object {
    $_ -is [Management.Automation.Language.IfStatementAst] -and $_.Clauses[0].Item1.Extent.Text -eq '$Move' })
if ($block.Count -ne 1) { throw 'move dispatch missing or ambiguous' }
$mover = Join-Path (Split-Path -Parent $MyInvocation.MyCommand.Path) 'mover.ps1'
Set-Content -LiteralPath $mover -Encoding UTF8 -Value @(
    'param([switch]$Move,[string]$MoveFrom,[string]$MoveTo,[string]$MirrorSettings,[switch]$Register,[string]$DataRoot)',
    'if ($Register) { exit 0 }',                      # the re-registration the move starts: nothing registered
    'function Grant-SettingsFolder { }', 'function Set-MachineSettings($unused) { }',
    $block[0].Extent.Text)
& powershell.exe -NoProfile -ExecutionPolicy Bypass -File $mover -Move -MoveFrom $MoveFrom -MoveTo $MoveTo
exit $LASTEXITCODE
"""

USERS_RIGHTS = r"""
param([string]$Path)
@((Get-Acl -LiteralPath $Path).Access | Where-Object {
    $_.AccessControlType -eq 'Allow' -and
    $_.IdentityReference.Translate([Security.Principal.SecurityIdentifier]).Value -eq 'S-1-5-32-545' } |
  ForEach-Object { $_.FileSystemRights.ToString() }) -join ','
"""


def _ps(script, *args):
    return subprocess.run(["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", str(script)] +
                          [str(a) for a in args], capture_output=True, text=True, timeout=120)


def test_move_leaves_every_file_readable(tmp_path):
    work = tmp_path / ("move-" + uuid.uuid4().hex[:8])
    move_from = work / "Roaming" / "macintalk" / "outspoken"
    shared = work / "ProgramData" / "macintalk"
    move_to = shared / "outspoken"
    (move_from / "rom").mkdir(parents=True)
    (move_from / "rom" / "DRVR_1030.bin").write_bytes(b"a ROM")
    (shared / "tiger").mkdir(parents=True)                     # Panthera's, already under the shared root
    (shared / "tiger" / "voice.bank").write_bytes(b"a voice bank")
    probe = work / "probe.ps1"
    probe.write_text(PROBE.replace("__REGISTER__", str(ROOT / "sapi" / "register.ps1")), encoding="utf-8")
    rights = work / "rights.ps1"
    rights.write_text(USERS_RIGHTS, encoding="utf-8")
    try:
        run = _ps(probe, "-MoveFrom", move_from, "-MoveTo", move_to)
        assert run.returncode == 0, run.stdout + run.stderr
        rom = move_to / "rom" / "DRVR_1030.bin"
        assert rom.is_file()
        for f in (rom, shared / "tiger" / "voice.bank"):
            got = _ps(rights, f).stdout.strip()
            assert "Read" in got, "%s: Users have %r -- an empty ACL, unreadable" % (f.name, got)
            assert not any(w in got for w in ("Write", "Modify", "FullControl")), got
        root = _ps(rights, shared).stdout.strip()
        assert root and not any(w in root for w in ("Write", "Modify", "FullControl", "CreateFiles", "AppendData")), root
    finally:
        subprocess.run(["icacls", str(work), "/reset", "/T", "/C", "/Q"], capture_output=True)   # removable again
        shutil.rmtree(work, ignore_errors=True)
