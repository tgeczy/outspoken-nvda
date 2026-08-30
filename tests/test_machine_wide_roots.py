# -*- coding: utf-8 -*-
"""The machine-wide and bare-APPDATA homes for the ROMs are actually searched.

Ported from what Panthera 2.0.0 learned the hard way, one bug per root:

* `%ProgramData%\\macintalk\\<name>` -- NVDA's own folder name at the root of
  the machine.  Tomi moved the shared `macintalk` folder there and got
  "5 Macintosh speech engines are missing": four Panthera generations and
  this add-on, every one looking at a machine-wide root spelled one word
  differently from the folder he had made, and nothing said so.  A tree
  there is read by SYSTEM on the sign-in screen with **no copy at all**,
  where data inside NVDA's configuration directory reaches it only because
  NVDA copies the whole directory -- voice banks included -- into
  `systemConfig`.
* bare `%APPDATA%\\macintalk\\<name>`, no `nvda` in the path -- a real
  arrangement (kept beside a SAPI install), and the one place neither
  add-on used to look, which made "searched everywhere" a lie on the one
  machine that tested it.
* the machine-wide `HKLM\\Software\\outSPOKEN SAPI\\DataPath`, read from
  **both registry views**, because `HKLM\\Software` is redirected under
  WOW64 while `HKCU\\Software` is not: this module runs inside 64-bit NVDA,
  NVDA's 32-bit synth-driver host, and the SAPI bridge's 32- and 64-bit
  clients, and each would otherwise see a different half of the registry.

`rom.find()` merges per-file across every root rather than electing one, so
the empty-folder-beats-real-data failure Panthera had cannot happen here --
which is why these tests are about *roots being present in the search* and
not about preference order.

Each addition is proven the way `test_common_tree.py` proved Panthera's:
the new-root tests fail with the lookup removed.
"""
import os
import sys
import types

import pytest

sys.path.insert(0, os.path.join(
    os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
    "addon", "synthDrivers", "_outspoken"))

import rom  # noqa: E402


class _FakeKey(object):
    def __init__(self, value):
        self.value = value

    def __enter__(self):
        return self

    def __exit__(self, *a):
        return False


def _fake_winreg(user=None, machine=None, machineByView=None):
    """A winreg with a per-user and a machine-wide DataPath.

    `machineByView` maps a view flag to a path, for proving that both views
    are genuinely asked -- every OpenKey is recorded in `mod.asked`.
    """
    mod = types.ModuleType("winreg")
    mod.HKEY_CURRENT_USER = object()
    mod.HKEY_LOCAL_MACHINE = object()
    mod.REG_SZ = 1
    mod.KEY_READ = 0x20019
    mod.KEY_WOW64_32KEY = 0x0200
    mod.KEY_WOW64_64KEY = 0x0100
    views = mod.KEY_WOW64_32KEY | mod.KEY_WOW64_64KEY
    asked = []

    def OpenKey(root, path, reserved=0, access=0):
        asked.append((root, access & views))
        if root is mod.HKEY_CURRENT_USER:
            value = user
        elif machineByView is not None:
            value = machineByView.get(access & views)
        else:
            value = machine
        if value is None:
            raise OSError("no such key")
        return _FakeKey(value)

    mod.OpenKey = OpenKey
    mod.QueryValueEx = lambda key, name: (key.value, mod.REG_SZ)
    mod.asked = asked
    return mod


@pytest.fixture
def isolated(monkeypatch, tmp_path):
    """No real config dir, registry, or environment reaches the search."""
    cfg = tmp_path / "cfg" / "macintalk" / "outspoken"
    monkeypatch.setattr(rom, "config_dir", lambda: str(cfg))
    monkeypatch.setitem(sys.modules, "winreg", _fake_winreg())
    monkeypatch.delenv("APPDATA", raising=False)
    monkeypatch.delenv("ProgramData", raising=False)
    monkeypatch.delenv("ALLUSERSPROFILE", raising=False)
    return tmp_path


def _norm(paths):
    return [os.path.normcase(os.path.abspath(p)) for p in paths]


def test_the_machine_wide_macintalk_folder_is_searched(isolated, monkeypatch):
    monkeypatch.setenv("ProgramData", str(isolated / "common"))
    expected = os.path.join(str(isolated / "common"), "macintalk", "outspoken")
    assert os.path.normcase(os.path.abspath(expected)) in _norm(
        rom.search_roots())


def test_a_rom_dropped_there_is_actually_found(isolated, monkeypatch):
    """Not merely listed: `find()` walks it and comes back with the file."""
    monkeypatch.setenv("ProgramData", str(isolated / "common"))
    inner = isolated / "common" / "macintalk" / "outspoken" / "macintalk1"
    inner.mkdir(parents=True)
    (inner / "DRVR_1030.bin").write_bytes(b"x")
    found, _missing = rom.find()
    assert found.get("DRVR_1030.bin") == str(inner / "DRVR_1030.bin")


def test_the_bare_appdata_macintalk_folder_is_searched(isolated, monkeypatch):
    monkeypatch.setenv("APPDATA", str(isolated / "roaming"))
    expected = os.path.join(str(isolated / "roaming"), "macintalk",
                            "outspoken")
    assert os.path.normcase(os.path.abspath(expected)) in _norm(
        rom.search_roots())


def test_the_standalone_default_is_still_searched(isolated, monkeypatch):
    """`outspoken-data` is where earlier releases put it, and stays forever."""
    monkeypatch.setenv("APPDATA", str(isolated / "roaming"))
    expected = os.path.join(str(isolated / "roaming"), "outspoken-data")
    assert os.path.normcase(os.path.abspath(expected)) in _norm(
        rom.search_roots())


def test_a_machine_datapath_in_only_the_64_bit_view_is_found(isolated,
                                                             monkeypatch):
    """The WOW64 trap: written by a 64-bit tool, read by a 32-bit client."""
    mod = _fake_winreg(machineByView={
        _fake_winreg().KEY_WOW64_64KEY: str(isolated / "shared")})
    monkeypatch.setitem(sys.modules, "winreg", mod)
    assert os.path.normcase(str(isolated / "shared")) in _norm(
        rom.search_roots())


def test_both_views_are_asked_for_the_machine_hive(isolated, monkeypatch):
    mod = _fake_winreg(machine=str(isolated / "shared"))
    monkeypatch.setitem(sys.modules, "winreg", mod)
    rom.search_roots()
    machineViews = sorted(v for hive, v in mod.asked
                          if hive is mod.HKEY_LOCAL_MACHINE)
    assert machineViews == sorted([mod.KEY_WOW64_32KEY, mod.KEY_WOW64_64KEY])


def test_one_folder_in_both_views_is_offered_once(isolated, monkeypatch):
    """The usual arrangement -- the tool writes both views -- is one folder."""
    mod = _fake_winreg(machine=str(isolated / "shared"))
    monkeypatch.setitem(sys.modules, "winreg", mod)
    roots = _norm(rom.search_roots())
    assert roots.count(os.path.normcase(str(isolated / "shared"))) == 1


def test_config_dir_still_wins_the_order(isolated, monkeypatch):
    """First mention decides which copy of a file is used, on real machines
    today -- the new roots must join the queue, never cut it."""
    monkeypatch.setenv("ProgramData", str(isolated / "common"))
    monkeypatch.setenv("APPDATA", str(isolated / "roaming"))
    roots = rom.search_roots()
    assert os.path.normcase(roots[0]) == os.path.normcase(rom.config_dir())


def test_a_winreg_without_the_view_flags_is_not_fatal(isolated, monkeypatch):
    """A stand-in registry is a registry with nothing in it, not a crash."""
    mod = types.ModuleType("winreg")
    mod.HKEY_CURRENT_USER = object()
    mod.HKEY_LOCAL_MACHINE = object()
    mod.REG_SZ = 1

    def OpenKey(*a, **k):
        raise OSError("no such key")

    mod.OpenKey = OpenKey
    mod.QueryValueEx = lambda *a: (None, 1)
    monkeypatch.setitem(sys.modules, "winreg", mod)
    assert rom.search_roots()
