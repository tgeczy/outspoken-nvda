# -*- coding: utf-8 -*-
"""Which folder the SAPI tool would move, and which it refuses to touch.

Ported from Panthera 2.0.0's classifier with the one rule that add-on could
not have: **the other add-on's data is not ours to move.**  The shared
`macintalk` folder holds Panthera's generations beside this add-on's ROMs,
and a mover that swept them along would silence four synthesizers belonging
to a different program -- so only the `outspoken` subtree ever moves, and a
source that itself contains generation folders is refused outright with
words saying whose data stopped it.

`settings.ps1 -Plan` prints the classification and does nothing else, and
`-DataRoot` pins the resolved root, which is what keeps these independent of
whatever the machine running them has in its registry.
"""
import os
import subprocess
import sys

import pytest

_HERE = os.path.dirname(os.path.abspath(__file__))
_SETTINGS = os.path.join(os.path.dirname(_HERE), "sapi", "settings.ps1")

pytestmark = pytest.mark.skipif(sys.platform != "win32",
                                reason="the SAPI tool is Windows PowerShell")


def _plan(root, appdata, programdata):
    env = dict(os.environ)
    for key in [k for k in env if k.casefold() in
                ("appdata", "programdata", "allusersprofile")]:
        del env[key]
    env["APPDATA"] = appdata
    if programdata is not None:
        env["ProgramData"] = programdata
    out = subprocess.run(
        ["powershell.exe", "-NoProfile", "-ExecutionPolicy", "Bypass",
         "-File", _SETTINGS, "-Plan", "-DataRoot", root],
        capture_output=True, text=True, env=env, timeout=120)
    assert out.returncode == 0, out.stderr
    plan = {}
    for line in out.stdout.splitlines():
        if ": " in line:
            key, _, value = line.partition(": ")
            plan[key.strip()] = value.strip()
    assert "plan" in plan, out.stdout
    return plan


def _rom(tree):
    """A folder `Test-OspRoot` accepts: a DRVR under it."""
    inner = os.path.join(tree, "macintalk1")
    os.makedirs(inner)
    with open(os.path.join(inner, "DRVR_1030.bin"), "wb") as f:
        f.write(b"x")
    return tree


def test_the_per_user_default_moves(tmp_path):
    appdata = str(tmp_path / "roaming")
    root = _rom(os.path.join(appdata, "outspoken-data"))
    plan = _plan(root, appdata, str(tmp_path / "common"))
    assert plan["plan"] == "move"
    assert plan["to"] == os.path.join(str(tmp_path / "common"),
                                      "macintalk", "outspoken")


def test_only_the_outspoken_subtree_of_a_shared_folder_moves(tmp_path):
    """Tiger sits beside it and is not swept along -- `from` says so."""
    appdata = str(tmp_path / "roaming")
    os.makedirs(os.path.join(appdata, "macintalk", "tiger"))
    _rom(os.path.join(appdata, "macintalk", "outspoken"))
    plan = _plan(appdata, appdata, str(tmp_path / "common"))
    assert plan["plan"] == "move"
    assert plan["from"] == os.path.join(appdata, "macintalk", "outspoken")


def test_a_folder_that_itself_holds_panthera_data_is_refused(tmp_path):
    """ROMs loose beside generation folders: nothing separable to take."""
    appdata = str(tmp_path / "roaming")
    mixed = os.path.join(appdata, "outspoken-data")
    _rom(mixed)
    os.makedirs(os.path.join(mixed, "leopard"))
    plan = _plan(mixed, appdata, str(tmp_path / "common"))
    assert plan["plan"] == "panthera"
    assert "Panthera" in plan["reason"]


def test_nvdas_own_folder_is_never_moved(tmp_path):
    appdata = str(tmp_path / "roaming")
    nvda = os.path.join(appdata, "nvda")
    _rom(os.path.join(nvda, "macintalk", "outspoken"))
    plan = _plan(nvda, appdata, str(tmp_path / "common"))
    assert plan["plan"] == "nvda"


def test_a_chosen_folder_stays_where_they_put_it(tmp_path):
    root = _rom(str(tmp_path / "my-roms"))
    plan = _plan(root, str(tmp_path / "roaming"), str(tmp_path / "common"))
    assert plan["plan"] == "chosen"


def test_already_machine_wide_is_done(tmp_path):
    common = str(tmp_path / "common")
    _rom(os.path.join(common, "macintalk", "outspoken"))
    plan = _plan(common, str(tmp_path / "roaming"), common)
    assert plan["plan"] == "done"


def test_an_empty_folder_has_nothing_to_move(tmp_path):
    root = str(tmp_path / "empty")
    os.makedirs(root)
    plan = _plan(root, str(tmp_path / "roaming"), str(tmp_path / "common"))
    assert plan["plan"] == "none"
