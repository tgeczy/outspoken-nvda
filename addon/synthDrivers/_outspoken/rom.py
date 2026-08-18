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
    "DICT_-4048.bin": "Berkeley's exception dictionary (optional)",
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


#: The dictionary only improves pronunciation; the engine speaks without it.
OPTIONAL = ("DICT_-4048.bin",)


def usable():
    """True when there is enough to speak English."""
    found, _ = find()
    return all(n in found for n in REQUIRED) and "RULZ_1129.bin" in found


def engines_present():
    """-> names of the engines there is enough on disk to run.

    The one place that answers "is there anything here at all". The global
    plugin used to answer it for itself, with a narrower test than the
    synthesizer's: it asked only for REQUIRED, while MacinTalk 1 also needs
    RULZ_1129.bin to turn text into phonemes. A user with the first two files
    and not the third therefore got no dialog *and* no synthesizer -- the two
    halves disagreeing produced silence from both.
    """
    out = []
    found, _missing = find()
    if all(n in found for n in REQUIRED) and "RULZ_1129.bin" in found:
        out.append("MacinTalk 1")
    for mod, label in (("macintalk2", "MacinTalk 2"),
                       ("macintalkpro", "MacinTalk Pro")):
        try:
            m = __import__(mod)
            if m.usable(search_roots()):
                out.append(label)
        except Exception:
            pass
    return out


def explain():
    """-> (anything_runnable, [lines]) -- describe(), plus the verdict.

    Written for a log someone else will read: every root that was searched,
    every file that was looked for, and what it concluded.
    """
    engines = engines_present()
    lines = ["searched:"]
    for r in search_roots():
        lines.append("  %s %s" % (r, "exists" if os.path.isdir(r)
                                  else "MISSING"))
    found, _missing = find()
    for n, what in FILES.items():
        lines.append("  %-16s %-8s (%s)"
                     % (n, "found" if n in found else "MISSING", what))
    lines.append("runnable engines: %s" % (", ".join(engines) or "none"))
    return bool(engines), lines


def describe():
    found, missing = find()
    lines = ["ROM folder: %s" % config_dir()]
    for n, what in FILES.items():
        lines.append("  %-16s %s   (%s)"
                     % (n, "found" if n in found else "MISSING", what))
    return "\n".join(lines)
