# -*- coding: utf-8 -*-
"""Diff the host's search roots against rom.search_roots().

The serve host has to look in every place the driver looks -- NVDA's
configuration folder, the legacy folder and its pointer file, the add-on's
own rom, the SAPI DataPath in HKCU and in both HKLM views, ProgramData and
APPDATA -- in the same order and deduplicated the same way.  This asks both
for a temporary configuration folder and diffs the lists.  Registry and
environment are this machine's, on both sides alike.

    py -3 tools/roots_oracle.py [path/to/osp_host.dll | libosp_host.so]

Data-free: it runs in CI.  Exit status is the number of disagreements.
"""
import ctypes
import os
import sys
import tempfile
import types

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
ADDON = os.path.join(ROOT, "addon")
sys.path.insert(0, os.path.join(ADDON, "synthDrivers", "_outspoken"))


def reference(config_path):
    gv = types.ModuleType("globalVars")
    gv.appArgs = types.SimpleNamespace(configPath=config_path, secure=False)
    sys.modules["globalVars"] = gv
    import importlib
    import rom
    importlib.reload(rom)
    return rom.search_roots()


def load(path=None):
    if path is None:
        import osp
        path = osp.DLL
    lib = ctypes.CDLL(path)
    lib.osp_roots_default.argtypes = [ctypes.c_char_p, ctypes.c_char_p, ctypes.c_char_p, ctypes.c_int]
    lib.osp_roots_default.restype = ctypes.c_int
    return lib


def host(lib, config_path, addon_root):
    cap = 16384
    buf = ctypes.create_string_buffer(cap)
    n = lib.osp_roots_default(config_path.encode("utf-8"), addon_root.encode("utf-8"), buf, cap)
    if n < 0:
        raise RuntimeError("osp_roots_default failed")
    return [line for line in buf.raw[:n].decode("utf-8").split("\n") if line]


def run(lib, verbose=True):
    bad = 0
    with tempfile.TemporaryDirectory(prefix="osp-roots-") as tmp:
        cases = [os.path.join(tmp, "plain")]
        # A pointer file naming another folder, with whitespace to strip.
        pointed = os.path.join(tmp, "pointed")
        os.makedirs(pointed)
        with open(os.path.join(pointed, "outspoken-roms.txt"), "w", encoding="utf-8") as fh:
            fh.write("  " + os.path.join(tmp, "elsewhere") + "\n")
        cases.append(pointed)
        # A pointer file naming a folder already in the list: deduplicated.
        dup = os.path.join(tmp, "dup")
        os.makedirs(dup)
        with open(os.path.join(dup, "outspoken-roms.txt"), "w", encoding="utf-8") as fh:
            fh.write(os.path.join(dup, "macintalk", "outspoken").upper() + "\n")
        cases.append(dup)
        for cfg in cases:
            want = reference(cfg)
            got = host(lib, cfg, ADDON)
            if want != got:
                bad += 1
                if verbose:
                    print("DIFFER for %s" % cfg)
                    print("   python: %r" % want)
                    print("   host  : %r" % got)
    if verbose:
        print("%d configurations, %d disagreement(s)" % (len(cases), bad))
    return bad


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else None
    try:
        lib = load(path)
    except OSError as e:
        raise SystemExit("cannot load the host library (%s); run build.sh" % e)
    return run(lib)


if __name__ == "__main__":
    sys.exit(main())
