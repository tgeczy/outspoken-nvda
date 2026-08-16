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
import paths                                                   # noqa: E402

VOICE_DESCRIPTION_LEN = 362

#: VoiceDescription.gender
GENDER = {0: "neuter", 1: "male", 2: "female"}

#: VoiceSpec.creator -> what this project can do with it
ENGINES = {
    "mtk2": "MacinTalk 2",
    "mtk3": "MacinTalk 3",
    "gala": "MacinTalk Pro",
}


class Voice(object):
    __slots__ = ("name", "comment", "creator", "id", "version", "gender",
                 "age", "script", "language", "region", "folder", "files",
                 "extra", "ttvi_id", "ttvw_id")

    def __repr__(self):
        return "<%s %s %s>" % (self.creator, self.name, self.gender)

    @property
    def engine(self):
        return ENGINES.get(self.creator, "unknown (%r)" % self.creator)


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
    return v


def _voice_dirs():
    for root in paths.roots():
        d = os.path.join(root, "voices")
        if os.path.isdir(d):
            yield d


def installed(engine=None):
    """Every voice under `<rom>/voices`, in name order.

    A folder that will not decode is skipped, not raised -- one damaged
    extraction must not cost the user every other voice.  `bad` collects them
    so a caller can say so out loud.
    """
    out, bad = [], []
    for base in _voice_dirs():
        for folder in sorted(os.listdir(base)):
            p = os.path.join(base, folder)
            if not os.path.isdir(p):
                continue
            files = {}
            for f in os.listdir(p):
                files[f.split("_")[0]] = os.path.join(p, f)
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
            if engine is None or v.creator == engine:
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

    by_engine = {}
    for v in voices:
        by_engine[v.engine] = by_engine.get(v.engine, 0) + 1
    print("\n  %d voices: %s"
          % (len(voices),
             ", ".join("%d %s" % (n, e) for e, n in sorted(by_engine.items()))))
    for folder, why in bad:
        print("  skipped %s -- %s" % (folder, why))
    return 0


if __name__ == "__main__":
    sys.exit(main())
