# -*- coding: utf-8 -*-
"""Pull the speech engines out of your own Macintosh disk image into `rom/`.

This project ships no part of any engine, so the add-on starts with an empty
`rom/` folder. This tool fills it from a disk image you already own -- the same
arrangement Apple-Eloquence-ELF uses: publish the recipe and the extractor,
never the bits.

Sources it understands:

  * an HFS disk image (`.hfv`, `.dsk`, `.img`) as used by Basilisk II / Mini vMac
  * a single file: outSPOKEN itself, a MacBinary `.bin`, or an already-extracted
    resource fork

What it looks for:

  * **outSPOKEN** (`cdev`, creator `BSDo`) -> `DRVR 1030` the 1984 MacinTalk
    engine, `TALK 1001`, `RULZ 1129`
  * **MacinTalk 2** (`Extensions/MacinTalk 2`) -> its front end, back end,
    rules, dictionary and phoneme tables
  * **MacinTalk 2 voices** (`Extensions/Voices/*`) -> one folder each
  * **MacinTalk Pro** (`Extensions/MacinTalk Pro`) -> its engine, its tables
    and its 573 KB data-fork lexicon
  * **MacinTalk Pro voices** -> one folder each, including the `gtss` unit
    database and the per-voice code

MacinTalk 3 is deliberately skipped: an existing NVDA add-on builds that engine
natively from Apple's own source, and emulating it would be strictly worse.
MacinTalk Pro is the same kind of component as MacinTalk 2 -- same 'ttsc' type,
same standard component entry -- so the host glue is shared.

    py -3 tools/extract_rom.py "C:/path/to/MacOS7.hfv"
    py -3 tools/extract_rom.py "C:/path/to/outSPOKEN" --out rom
    py -3 tools/extract_rom.py <image> --list
"""
import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import rsrc                                                   # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# (folder, [(type, id-or-None, description)]).  id None means "every one".
WANTED = {
    "macintalk1": [("DRVR", 1030, "the 1984 engine, named '.sp'"),
                   ("TALK", 1001, "MacinTalk Rules"),
                   ("RULZ", 1129, "letter-to-sound rules"),
                   ("DICT", None, "Berkeley's exception dictionaries"),
                   ("PHNM", None, "phoneme table")],
    "macintalk2": [("Cecy", 1, "back end -- the synthesiser"),
                   ("Cecy", 3, "front end -- text to phonemes"),
                   ("ttsr", None, "pronunciation rules"),
                   ("ttsd", None, "dictionary"),
                   ("ttss", None, "phoneme symbols"),
                   ("ttph", None, "magic char map"),
                   ("ttop", None, "magic opcode map"),
                   ("thng", None, "component descriptors")],
    "voice":      [("ttvi", None, "voice info"),
                   ("ttvd", None, "voice description"),
                   ("ttvw", None, "voice data")],
    # MacinTalk Pro is a 'ttsc' component like MacinTalk 2, so the same host
    # glue serves it; see docs/macintalk2-components.md.  Its code is gtse 1
    # and everything else here is table data.
    "macintalkpro": [("gtse", None, "the engine (gtse 1) and its tables"),
                     ("gtst", None, "text tables"),
                     ("gtsa", None, "phoneme alphabets"),
                     ("gtsp", None, "phoneme sets"),
                     ("gtsg", None, "configuration"),
                     ("gtsm", None, "module map"),
                     ("gtsi", None, "index"),
                     ("gala", None, "copyright"),
                     ("thng", None, "component descriptor"),
                     # Not decoration.  Pro's Open asks for its own `vers 1`
                     # (gtse 1 +$6E8) and returns synthOpenFailed without it.
                     ("vers", None, "version")],
    # A Pro voice is nothing like a MacinTalk 2 one.  Alongside the standard
    # ttvd it carries a 128-byte gtsv record, and `gtss` holds both the
    # concatenative unit database (789-922 KB) and about 11 KB of per-voice
    # *code*.  That is why asking only for ttvi/ttvd/ttvw returned 362 bytes
    # out of an 870 KB file.
    "provoice":   [("ttvd", None, "voice description"),
                   ("gtsv", None, "voice record"),
                   ("gtss", None, "unit database and per-voice code"),
                   ("gtsg", None, "configuration"),
                   ("gtsi", None, "index"),
                   ("gtsm", None, "wave map"),
                   ("vers", None, "version")],
}


def _safe(s):
    return "".join(c if c.isalnum() or c in " ._-" else "_" for c in s).strip()


def open_image(path):
    """-> {posix path: (dataFork, rsrcFork)} for an HFS image, else None."""
    try:
        import machfs
    except ImportError:
        print("  ! machfs is not installed -- `py -3 -m pip install machfs`\n"
              "    (needed only for disk images, not for single files)")
        return None
    try:
        v = machfs.Volume()
        v.read(open(path, "rb").read())
    except Exception as e:
        return None
    out = {}
    for parts, o in v.iter_paths():
        if isinstance(o, machfs.Folder):
            continue
        out["/".join(parts)] = (o.data, o.rsrc,
                                getattr(o, "creator", b"").decode("mac-roman",
                                                                 "replace"))
    return out


def take(fork, spec, outdir, label):
    """Write the resources `spec` asks for. -> (written, missing)."""
    try:
        rs = rsrc.parse(fork)
    except Exception as e:
        print("  ! %s: cannot read resource fork (%s)" % (label, e))
        return 0, [d for _, _, d in spec]
    by = {}
    for r in rs:
        by.setdefault(r.type, []).append(r)
    os.makedirs(outdir, exist_ok=True)
    n, missing = 0, []
    for rtype, rid, desc in spec:
        got = [r for r in by.get(rtype, []) if rid is None or r.id == rid]
        if not got:
            missing.append("%s %s (%s)" % (rtype, rid if rid else "*", desc))
            continue
        for r in got:
            name = "%s_%d.bin" % (_safe(r.type).strip() or "res", r.id)
            open(os.path.join(outdir, name), "wb").write(r.data)
            n += 1
    return n, missing


def main():
    ap = argparse.ArgumentParser(
        description="Extract speech engines from your own Mac disk image.")
    ap.add_argument("source", help="an HFS image, or a single Mac file")
    ap.add_argument("--out", default=os.path.join(ROOT, "rom"),
                    help="destination folder (default: ./rom)")
    ap.add_argument("--list", action="store_true",
                    help="only show what was found; write nothing")
    a = ap.parse_args()

    if not os.path.exists(a.source):
        print("no such file: %s" % a.source)
        return 1

    files = open_image(a.source)
    if files is None:                      # not an image -- treat as one file
        raw = open(a.source, "rb").read()
        files = {os.path.basename(a.source): (b"", raw, "")}
        print("reading a single file: %s\n" % os.path.basename(a.source))
    else:
        print("mounted %s -- %d files\n" % (os.path.basename(a.source),
                                            len(files)))

    jobs, skipped = [], []
    for path, (data, res, creator) in sorted(files.items()):
        base = path.split("/")[-1]
        low = base.lower()
        fork = res or data
        if not fork:
            continue
        if low.startswith("outspoken") and "prefs" not in low:
            # outSPOKEN 8 (creator 'oSM ') dropped the bundled 1984 engine and
            # speaks through the Speech Manager instead, so it has nothing for
            # us. Only the earlier product (creator 'BSDo') carries '.sp'.
            if creator.strip() and creator.strip() != "BSDo":
                skipped.append((path, "outSPOKEN %s -- no bundled engine; "
                                      "it uses the Speech Manager" % creator))
                continue
            jobs.append((path, fork, WANTED["macintalk1"], "macintalk1", b""))
        elif base == "MacinTalk 2" or low.startswith("macintalk 2."):
            jobs.append((path, fork, WANTED["macintalk2"], "macintalk2", b""))
        elif base == "MacinTalk Pro" or low.startswith("macintalk pro"):
            # Pro is the only one of these with a data fork that matters: 573 KB
            # of lexicon, which no resource type covers.  Both halves are needed
            # and only one of them is a resource fork, so carry it separately.
            jobs.append((path, res or data, WANTED["macintalkpro"],
                         "macintalkpro", data if res else b""))
        elif "/Voices/" in path or path.startswith("Voices/"):
            # Which engine a voice belongs to is written in its resource types,
            # not its name. A MacinTalk 2 voice carries ttvi + ttvd + ttvw; the
            # MacinTalk 3 voices are parameter sets with no ttvi, and the Pro
            # voices carry only a ttvd beside a very large data fork.
            try:
                kinds = {r.type for r in rsrc.parse(fork)}
            except Exception:
                kinds = set()
            nm = base.replace(".rsrc", "")
            if {"ttvi", "ttvd", "ttvw"} <= kinds:
                jobs.append((path, fork, WANTED["voice"],
                             "voices/" + _safe(nm), b""))
            elif "gtss" in kinds:
                jobs.append((path, fork, WANTED["provoice"],
                             "voices/" + _safe(nm), b""))
            elif "ttvd" in kinds:
                skipped.append((nm, "MacinTalk 3 -- a native NVDA add-on "
                                    "already builds that engine from Apple's "
                                    "source; use that instead"))

    if not jobs:
        print("  Nothing recognised. Expected outSPOKEN, an Extensions/MacinTalk 2,\n"
              "  or an Extensions/Voices folder. Try --list on a mounted image.")
        return 1

    total = 0
    for path, fork, spec, sub, datafork in jobs:
        outdir = os.path.join(a.out, sub)
        if a.list:
            try:
                rs = rsrc.parse(fork)
                kinds = sorted({r.type for r in rs})
            except Exception:
                kinds = ["<unreadable>"]
            print("  %-52s %s" % (path[:52], " ".join(kinds)))
            continue
        n, missing = take(fork, spec, outdir, path)
        if datafork:
            os.makedirs(outdir, exist_ok=True)
            with open(os.path.join(outdir, "datafork.bin"), "wb") as fh:
                fh.write(datafork)
            n += 1
        total += n
        flag = "" if not missing else "   MISSING: " + "; ".join(missing)
        print("  %-52s %2d resources -> %s%s"
              % (path[:52], n, os.path.relpath(outdir, ROOT), flag))

    if skipped:
        print("\n  skipped, on purpose:")
        for nm, why in sorted(skipped):
            print("    %-14s %s" % (nm[:14], why))

    if not a.list:
        print("\n  %d resources written under %s" % (total, a.out))
        print("  Nothing here is redistributable -- these are your copies, from\n"
              "  your image, and they stay out of the repository and out of any\n"
              "  release. See README.md.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
