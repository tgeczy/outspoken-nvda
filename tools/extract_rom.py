# -*- coding: utf-8 -*-
"""Pull the speech engines out of your own Macintosh disk image into `rom/`.

**The work moved into the add-on and this is now the command line over it.**
It used to be the whole thing, which meant extracting an engine needed Python,
a terminal and `pip install machfs` -- a real barrier for the people most
likely to want a 1984 speech synthesiser. The logic lives in
`addon/synthDrivers/_outspoken/ospextract.py` now, where NVDA can reach it, and
the Tools menu offers the same thing as a dialog.

`git log --follow` on that file reaches this one: it was moved, not rewritten.

Nothing here changed for anybody who was already using it:

    py -3 tools/extract_rom.py <image>            -> ./rom
    py -3 tools/extract_rom.py <image> --nvda     -> NVDA's own folder
    py -3 tools/extract_rom.py <image> --list     -> say what is there only

`<image>` is an HFS disk image (`.hfv`, `.dsk`, `.img`) or a single Mac file:
outSPOKEN itself, a MacBinary `.bin`, or an already-extracted resource fork.
"""
import argparse
import os
import sys

#: Two homes: the repository (tools/ beside addon/), and the SAPI install,
#: where build.ps1 stages this file at the root with the driver tree at
#: `synthDrivers\_outspoken` beside it -- the same layout the bundled
#: Python's `._pth` names.  Tried in that order so the repository wins when
#: both shapes are present.
_HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(_HERE)
for _private in (os.path.join(ROOT, "addon", "synthDrivers", "_outspoken"),
                 os.path.join(_HERE, "synthDrivers", "_outspoken")):
    if os.path.isdir(_private):
        sys.path.insert(0, _private)
        break

import ospextract                                             # noqa: E402


def main():
    ap = argparse.ArgumentParser(
        description="Extract speech engines from your own Mac disk image.")
    ap.add_argument("source", help="an HFS image, or a single Mac file")
    ap.add_argument("--out", default=None,
                    help="destination folder (default: ./rom)")
    ap.add_argument("--nvda", action="store_true",
                    help="write straight into NVDA's macintalk folder, "
                         "which is the one the add-on actually reads")
    ap.add_argument("--list", action="store_true",
                    help="only show what was found; write nothing")
    a = ap.parse_args()

    if a.nvda:
        if a.out:
            print("give either --nvda or --out, not both")
            return 1
        a.out = ospextract.nvda_roms()
        if not a.out:
            print("cannot find NVDA's configuration directory.\n"
                  "  Looked for %%APPDATA%%\\nvda and ~/.nvda.\n"
                  "  Extract with --out <folder> and copy it there yourself.")
            return 1
        print("writing into NVDA's own folder: %s\n" % a.out)
    elif not a.out:
        a.out = os.path.join(ROOT, "rom")

    if not os.path.exists(a.source):
        print("no such file: %s" % a.source)
        return 1

    got = ospextract.extract(a.source, a.out, listing=a.list)
    if got is None:
        print("  Nothing recognised. Expected outSPOKEN, an "
              "Extensions/MacinTalk 2,\n  or an Extensions/Voices folder. "
              "Try --list on a mounted image.")
        return 1
    total, skipped = got

    if skipped:
        print("\n  skipped, on purpose:")
        for nm, why in sorted(skipped):
            print("    %-14s %s" % (nm[:14], why))

    if not a.list:
        print("\n  %d resources written under %s" % (total, a.out))
        ospextract.report_ready(a.out)
        if not a.nvda and ospextract.nvda_roms():
            print("\n  This folder is not the one NVDA reads. Re-run with "
                  "--nvda to write\n  straight into %s, or copy it there."
                  % ospextract.nvda_roms())
        print("\n  Nothing here is redistributable -- these are your copies, "
              "from\n  your image, and they stay out of the repository and "
              "out of any\n  release. See README.md.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
