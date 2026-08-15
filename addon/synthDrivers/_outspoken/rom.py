# -*- coding: utf-8 -*-
"""Find the engine the user supplied.

This add-on ships no part of MacinTalk or outSPOKEN. The user extracts three
resources from their own copy -- `tools/extract_rom.py` does it from a disk
image -- and drops them somewhere we look.

**The ROM lives in NVDA's configuration folder, not the add-on folder.**
Updating an add-on deletes and recreates its directory, so a ROM kept inside it
would be silently destroyed on every upgrade. The add-on folder is still
searched, because someone will put it there anyway and it should work.

The folder is `outspoken-roms` rather than anything shorter: it sits directly
in NVDA's configuration directory alongside every other add-on's data, so a
generic name like `roms` would be a collision waiting to happen.

Subfolders are searched recursively, so the layout `tools/extract_rom.py`
produces works unchanged:

    outspoken-roms/
        macintalk1/     DRVR_1030.bin, TALK_1001.bin, RULZ_1129.bin
        macintalk2/     (a later engine; not used yet)
        voices/<name>/  (likewise)

A flat folder with the three files loose in it works just as well.
"""
import os

FILES = {
    "DRVR_1030.bin": "the 1984 MacinTalk engine, named '.sp'",
    "TALK_1001.bin": "MacinTalk Rules",
    "RULZ_1129.bin": "letter-to-sound rules (needed for English text)",
}

#: `RULZ` only drives the English front end. Without it the synthesiser still
#: speaks, but only if it is handed phonemes, which no screen reader does.
REQUIRED = ("DRVR_1030.bin", "TALK_1001.bin")


def config_dir():
    """`<nvda user config>/outspoken-roms`."""
    try:
        import globalVars
        base = globalVars.appArgs.configPath
    except Exception:
        base = os.path.join(os.path.expanduser("~"), ".nvda")
    return os.path.join(base, "outspoken-roms")


def search_roots():
    here = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    addon = os.path.dirname(here)                    # ...\addon
    return [config_dir(), os.path.join(addon, "rom")]


def find():
    """-> ({name: path}, [missing names]).

    Searches each root recursively, so the layout `tools/extract_rom.py`
    produces (`rom/macintalk1/DRVR_1030.bin`) works as well as a flat folder.
    """
    found = {}
    for root in search_roots():
        if not os.path.isdir(root):
            continue
        for dirpath, _dirs, names in os.walk(root):
            for n in names:
                if n in FILES and n not in found:
                    found[n] = os.path.join(dirpath, n)
    return found, [n for n in FILES if n not in found]


def usable():
    """True when there is enough to speak English."""
    found, _ = find()
    return all(n in found for n in REQUIRED) and "RULZ_1129.bin" in found


def describe():
    found, missing = find()
    lines = ["ROM folder: %s" % config_dir()]
    for n, what in FILES.items():
        lines.append("  %-16s %s   (%s)"
                     % (n, "found" if n in found else "MISSING", what))
    return "\n".join(lines)
