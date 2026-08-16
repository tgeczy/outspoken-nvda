# -*- coding: utf-8 -*-
"""Enumerate the installed voices by reading `ttvd` as a `VoiceDescription`.

`ttvd` is not a private format that needs reverse engineering -- it is Apple's
own `VoiceDescription` struct, laid out exactly as Tim Schaaff's `Speech.h`
declares it:

    long          length;         /* sizeof(VoiceDescription), 362 */
    unsigned long creator;        /* 'mtk2', 'mtk3', 'gala'        */
    unsigned long id;
    long          version;
    Str63         name;           /* Pascal string, 64 bytes       */
    Str255        comment;        /* Pascal string, 256 bytes      */
    short         gender, age, script, language, region;
    long          reserved[4];

which sums to 362, and every one of the 32 voices on the OS 7 disk decodes
cleanly against it -- names, genders, ages, and the original demo sentences.

**`creator` is the engine, and it is authoritative.**  `extract_rom.py` has to
classify by which resources are present (`ttvi`+`ttvd`+`ttvw` -> MacinTalk 2)
because it is deciding what to pull before it has parsed anything; once a
`ttvd` is in hand there is no need to infer.  Prefer this.

A resource is usually *longer* than 362 bytes and that is not a defect: the
standard struct comes first and each engine appends its own data after it.
MacinTalk 2 adds 20 bytes, MacinTalk 3 appends its whole parameter set (that is
why an mtk3 "voice" is a `ttvd` and nothing else), and Pro appends nothing
because its voice data lives in separate files.  So `length` is 362 on every
voice of every engine, and the extra is engine-private.

**MacinTalk 2's 20 bytes are how it finds the rest of the voice.**  Its back
end reads them at Cecy 1 +$862: it locks the `ttvd`, adds `VoiceDescription
.length` to the block address to step over the standard struct, and then reads
two resource ids out of the extension --

    extension +$08   the `ttvi` id
    extension +$12   the `ttvw` id

-- fetching each with `_Get1Resource`.  So a MacinTalk 2 voice is
self-describing: given its `ttvd`, the other two resources name themselves, and
nothing has to be inferred from file names or ids.  `ttvi_ID` / `ttvw_ID` below
report what the extension asks for so a mismatch is visible.

    py -3 tools/voices.py                 # everything under rom/voices
    py -3 tools/voices.py --engine mtk2   # just the MacinTalk 2 ten
"""
import argparse
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

VOICE_DESCRIPTION_LEN = 362

#: VoiceDescription.gender
GENDER = {0: "neuter", 1: "male", 2: "female"}

#: VoiceSpec.creator -> what this project can do with it
ENGINES = {
    "mtk2": "MacinTalk 2",
    "mtk3": "MacinTalk 3",
    "gala": "MacinTalk Pro",
}

#: What has to be in `rom/` before an engine's voices can be spoken at all.
#:
#: **A voice folder is not enough, and the two arrive separately.** The
#: extractor pulls MacinTalk Pro's Victoria, Bruce and Agnes out of any disk
#: image that carries them -- roughly 900 KB each -- whether or not that image
#: also had the engine that reads them, and whether or not this project can
#: drive it yet. So a user can easily hold three Pro voices and nothing able to
#: speak them.
#:
#: `mtk3` is deliberately absent rather than missing: a native NVDA add-on
#: builds that engine from Apple's own source, so those voices are never ours
#: to offer however complete the extraction is.
ENGINE_FILES = {
    "mtk2": ("Cecy_1.bin", "Cecy_3.bin"),
    "gala": ("gtse_1.bin",),
}


class Voice(object):
    __slots__ = ("name", "comment", "creator", "id", "version", "gender",
                 "age", "script", "language", "region", "folder", "files",
                 "extra", "ttvi_id", "ttvw_id", "names")

    def __repr__(self):
        return "<%s %s %s>" % (self.creator, self.name, self.gender)

    @property
    def engine(self):
        return ENGINES.get(self.creator, "unknown (%r)" % self.creator)


#: Written by the extractor beside the `.bin`s: type, id, map entry, name.
#: A Mac resource name is not a file name -- MacinTalk Pro's modules are
#: `*TTS`, `*Wave`, `*Lex` -- so they live in their own file rather than in
#: the ones they name, and the map entry rides along because Pro asks
#: `RsrcMapEntry` where a resource sits before reading it out of the file.
NAMES_FILE = "resources.tsv"


def _read_names(path):
    """-> {(type, id): (map entry, mac name)}, empty when there is no index."""
    out = {}
    if not path or not os.path.isfile(path):
        return out
    try:
        with open(path, encoding="utf-8") as fh:
            for line in fh:
                row = line.rstrip(chr(13) + chr(10))
                if row.startswith("#") or "	" not in row:
                    continue
                rtype, rid, entry, nm = row.split("	", 3)
                out[(rtype, int(rid))] = (int(entry), nm)
    except (OSError, ValueError):
        return {}
    return out


def _pstr(b):
    """A Pascal string in a fixed-width field: length byte, then the text."""
    return b[1:1 + b[0]].decode("mac-roman", "replace")


def describe(data):
    """Parse a `ttvd` resource -> Voice.

    Raises ValueError rather than returning something half-filled: a voice we
    cannot describe must not reach NVDA's voice list at all.
    """
    if len(data) < VOICE_DESCRIPTION_LEN:
        raise ValueError("ttvd is %d bytes, need at least %d"
                         % (len(data), VOICE_DESCRIPTION_LEN))
    v = Voice()
    length, creator, v.id, v.version = struct.unpack(">I4sII", data[:16])
    if length != VOICE_DESCRIPTION_LEN:
        # Not fatal in principle, but it has never happened, and if it ever
        # does the field offsets below are the first thing to doubt.
        raise ValueError("VoiceDescription.length is %d, expected %d"
                         % (length, VOICE_DESCRIPTION_LEN))
    v.creator = creator.decode("mac-roman", "replace")
    v.name = _pstr(data[16:80])
    v.comment = _pstr(data[80:336])
    gender, v.age, v.script, v.language, v.region = \
        struct.unpack(">hhhhh", data[336:346])
    v.gender = GENDER.get(gender, "gender %d" % gender)
    v.extra = len(data) - VOICE_DESCRIPTION_LEN
    # MacinTalk 2's extension names the other two resources; see above.
    v.ttvi_id = v.ttvw_id = None
    if v.creator == "mtk2" and v.extra >= 0x14:
        ext = data[VOICE_DESCRIPTION_LEN:]
        v.ttvi_id, v.ttvw_id = struct.unpack(">h", ext[8:10])[0], \
            struct.unpack(">h", ext[0x12:0x14])[0]
    v.folder = None
    v.files = {}
    v.names = {}
    return v


def _roots():
    """Where to look. Deferred so this module imports inside the add-on too,
    where `tools/paths.py` does not exist and the caller passes its own roots.
    """
    import paths
    return paths.roots()


def _voice_dirs(roots=None):
    for root in (roots if roots is not None else _roots()):
        d = os.path.join(root, "voices")
        if os.path.isdir(d):
            yield d


def engine_installed(creator, roots=None):
    """-> True if the engine that speaks `creator`'s voices is in `rom/`.

    Asks for the engine's own code, not for its voices: having Victoria says
    nothing about whether MacinTalk Pro is there to read her.
    """
    need = ENGINE_FILES.get(creator)
    if not need:
        return False
    left = set(need)
    for root in (roots if roots is not None else _roots()):
        for _dirpath, _dirs, names in os.walk(root):
            left -= set(names)
            if not left:
                return True
    return not left


def installed(engine=None, roots=None, speakable=False):
    """Every voice under `<rom>/voices`, in name order.

    A folder that will not decode is skipped, not raised -- one damaged
    extraction must not cost the user every other voice.  `bad` collects them
    so a caller can say so out loud.

    `speakable=True` drops voices whose engine is not installed, and **anything
    building NVDA's voice list wants it.**  A synthesizer that lists a voice
    and then says nothing is worse than one that does not list it, and the
    extractor hands out Pro voices to anyone whose disk image had them.
    """
    out, bad, seen = [], [], set()
    havEngine = {}
    for base in _voice_dirs(roots):
        for folder in sorted(os.listdir(base)):
            p = os.path.join(base, folder)
            if not os.path.isdir(p):
                continue
            # First root wins, the same precedence `paths.find` uses.  Without
            # this, having both $OUTSPOKEN_ROM and ./rom populated lists every
            # voice twice -- and a duplicated voice reaches NVDA's voice list
            # as two identical entries the user cannot tell apart.
            if folder in seen:
                continue
            seen.add(folder)
            # Only `<type>_<id>.bin` -- a voice folder holds other things now.
            # `names.tsv` records the Mac resource names MacinTalk Pro looks
            # its modules up by, and taking it for a resource cost every
            # MacinTalk 2 voice at once: the key became "names.tsv", the id
            # parse raised, and the loader dropped the lot. Be specific about
            # what a resource file looks like rather than assuming the folder
            # contains nothing else.
            files, extra = {}, {}
            for f in os.listdir(p):
                stem, ext = os.path.splitext(f)
                if ext.lower() != ".bin" or "_" not in stem:
                    extra[f] = os.path.join(p, f)
                    continue
                rtype, rid = stem.rsplit("_", 1)
                try:
                    int(rid)
                except ValueError:
                    extra[f] = os.path.join(p, f)
                    continue
                files[rtype] = os.path.join(p, f)
            if "ttvd" not in files:
                bad.append((folder, "no ttvd"))
                continue
            try:
                with open(files["ttvd"], "rb") as fh:
                    v = describe(fh.read())
            except (ValueError, OSError) as e:
                bad.append((folder, str(e)))
                continue
            v.folder, v.files = p, files
            v.names = _read_names(extra.get(NAMES_FILE))
            if engine is not None and v.creator != engine:
                continue
            if speakable:
                if v.creator not in havEngine:
                    havEngine[v.creator] = engine_installed(v.creator, roots)
                if not havEngine[v.creator]:
                    bad.append((folder, "%s is not installed" % v.engine))
                    continue
            out.append(v)
    out.sort(key=lambda v: v.name.lower())
    return out, bad


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--engine", help="only this creator: mtk2, mtk3, gala")
    ap.add_argument("--comments", action="store_true",
                    help="show each voice's demo sentence")
    args = ap.parse_args()

    voices, bad = installed(args.engine)
    if not voices and not bad:
        print("No voices found. Looked under: %s\n"
              "Fill them in with:\n"
              "    py -3 tools/extract_rom.py <your disk image>"
              % (", ".join(_voice_dirs()) or "(no rom folder yet)"))
        return 1

    print("%-14s %-14s %-7s %5s %6s  %s"
          % ("name", "engine", "gender", "age", "+bytes", "resources"))
    for v in voices:
        print("%-14s %-14s %-7s %5d %6d  %s"
              % (v.name, v.engine, v.gender, v.age, v.extra,
                 " ".join(sorted(v.files))))
        if v.ttvi_id is not None:
            have_i = v.files.get("ttvi", "")
            have_w = v.files.get("ttvw", "")
            print("%-14s   wants ttvi %d, ttvw %d%s"
                  % ("", v.ttvi_id, v.ttvw_id,
                     "" if (("_%d." % v.ttvi_id) in have_i
                            and ("_%d." % v.ttvw_id) in have_w)
                     else "   <-- does not match the extracted ids"))
        if args.comments:
            print("%-14s   %r" % ("", v.comment))

    by_engine, missing = {}, {}
    for v in voices:
        by_engine[v.engine] = by_engine.get(v.engine, 0) + 1
        if v.creator not in missing:
            missing[v.creator] = not engine_installed(v.creator)
    print("\n  %d voices: %s"
          % (len(voices),
             ", ".join("%d %s" % (n, e) for e, n in sorted(by_engine.items()))))
    for folder, why in bad:
        print("  skipped %s -- %s" % (folder, why))
    # Listing a voice whose engine is absent is the diagnostic this tool is
    # for, so they stay in the table -- but saying nothing about it would let
    # a user believe an extraction is finished when half of it is missing.
    for creator, gone in sorted(missing.items()):
        if not gone:
            continue
        what = ENGINES.get(creator, creator)
        if creator in ENGINE_FILES:
            print("\n  %s is NOT installed (%s), so its voices above cannot\n"
                  "  speak and NVDA will not offer them. Extract it from the\n"
                  "  same disk image the voices came from."
                  % (what, ", ".join(ENGINE_FILES[creator])))
        else:
            print("\n  %s is not emulated here -- a native NVDA add-on builds\n"
                  "  that engine from Apple's own source; use that instead."
                  % what)
    return 0


if __name__ == "__main__":
    sys.exit(main())
