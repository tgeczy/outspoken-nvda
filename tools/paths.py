# -*- coding: utf-8 -*-
"""Where the engine lives, without hard-coding anybody's disk.

Every tool here needs the same three files, and none of them should care where
you keep your copy. Resolution order:

  1. ``$OUTSPOKEN_ROM``            -- point this anywhere
  2. ``<repo>/rom``                -- what ``tools/extract_rom.py`` fills
  3. ``<repo>/../outspoken-rsrc``  -- a loose folder of extracted resources

Each root is searched **recursively**, so the layout the extractor produces
(``rom/macintalk1/DRVR_1030.bin``) and a flat folder both work.

Names are the extractor's: ``DRVR_1030.bin``, ``TALK_1001.bin``,
``RULZ_1129.bin``. A few historical spellings are accepted too, because early
extractions used the resource's own name.
"""
import os

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

#: canonical name -> other spellings that mean the same resource
ALIASES = {
    "DRVR_1030.bin": ("1030_.sp.bin", "DRVR_1030", "sp.bin"),
    "TALK_1001.bin": ("1001.bin", "TALK_1001"),
    "RULZ_1129.bin": ("1129_new.bin", "RULZ_1129"),
    "Cecy_1.bin": (),          # MacinTalk 2 back end
    "Cecy_3.bin": (),          # MacinTalk 2 front end
}


def roots():
    out = []
    env = os.environ.get("OUTSPOKEN_ROM")
    if env:
        out.append(env)
    out.append(os.path.join(ROOT, "rom"))
    out.append(os.path.join(os.path.dirname(ROOT), "outspoken-rsrc"))
    return [p for p in out if os.path.isdir(p)]


def find(name):
    """-> full path, or None. `name` is a canonical name from ALIASES."""
    wanted = {name} | set(ALIASES.get(name, ()))
    for root in roots():
        for dirpath, _dirs, names in os.walk(root):
            for n in names:
                if n in wanted:
                    return os.path.join(dirpath, n)
    return None


def require(name):
    p = find(name)
    if p:
        return p
    raise SystemExit(
        "cannot find %s.\n"
        "Looked in: %s\n\n"
        "Fill the rom folder from your own copy:\n"
        "    py -3 tools/extract_rom.py <your disk image or outSPOKEN file>\n"
        "or set OUTSPOKEN_ROM to a folder that already has it."
        % (name, ", ".join(roots()) or "(nothing exists yet)"))


def driver():
    return require("DRVR_1030.bin")


def talk():
    return require("TALK_1001.bin")


def rulz():
    return require("RULZ_1129.bin")
