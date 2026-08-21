# -*- coding: utf-8 -*-
"""Classic MacinTalk 3 under the emulator: extract it, open it, speak at it.

**A research probe on a branch, not a shipped path.** `tools/extract_rom.py`
still refuses MacinTalk 3 by name and `engine-scope-and-modularity` still says
that engine is delegated -- a native NVDA add-on already builds it from Apple's
own source and does it better. Nothing here changes that. The reason to run the
real 68k build is that it tells you how the engine behaved *in 1994*, which no
native port can, and `C:\\git\\fred-ab` exists to settle whether that differs
from Tiger's 3.3.

    py -3 tools/probe_mt3_open.py --extract "C:/path/to/MacOS7.hfv"
    py -3 tools/probe_mt3_open.py                       # open only
    py -3 tools/probe_mt3_open.py --speak "Hello there."

## What it is, measured rather than recalled

`System Folder/Extensions/MacinTalk 3`, creator `mtk3`, 358,659 bytes of
resource fork and **no data fork**. Its `thng 1` is `ttsc` / `mtk3` / `mtk3`
and names its code as **`ttvi` 10** -- the type that means "voice info"
everywhere else. Apple named the resources after composers:

    ttvi 10  Bach          9,416   68k, the component entry
    ttvi  8  Beethoven    35,036   68k
    ttvi  9  Prokofieff   29,576   68k
    ttvi 11  Mozart      102,688   Joy!peff pwpc -- the POWERPC build, skip it
    ttvi 3-7               ~173 KB data; Wagner alone is 125,400

A voice is almost nothing: Fred is a 714-byte `ttvd`, because MacinTalk 3 is
formant and a voice is a parameter set. Only the novelty voices carry a `ttvw`.
Voice ids are `mtk3` 1 Fred, 2 Kathy, 4 Junior, 5 Ralph, 26 Bells.

## Three things it needs that no other engine here does

* **A 68040.** `+0x234` is `rtd #$8`, which is 68010 and up; on a 68000 it dies
  there, and on a 68020 Open returns -249. Only 68040 answers 0.
* **`defer_callbacks(True)`.** Without it the host answers the callback before
  the engine has finished writing the command -- the SndCommand reads back
  `00 00 00 00 00 09 80 7d`, cmd 0 and an odd param2, instead of
  `00 0d 00 00 00 0c 67 2c` -- and it jumps through freed memory. 38 million
  faults, and it looks exactly like an engine bug.
* **A Sound Manager command QUEUE, which this host does not have.** That is
  where the spike stops; see the bottom of this file.
"""
import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import osp                                                     # noqa: E402
from disasm import trap_name                                   # noqa: E402
from probe_mt2_open import signed                              # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
#: Deliberately NOT `rom/`, which is the shipped extractor's territory.
DEFAULT_OUT = os.path.join(ROOT, "build", "mt3")

CODE = 0x00040000
HEAP = 0x00080000
HEAP_SIZE = 0x00200000
STACK = 0x00400000
VOICE_SPEC = 0x00420000
PARAM_BUF = 0x00420100
TEXT_BUF = 0x00430000
STATUS_BUF = 0x00440000

MEMERR, RESERR, CPUFLAG = 0x0220, 0x0A60, 0x012F
OPEN, CLOSE = -1, -2
#: MacinTalk 2's selector map, which MacinTalk Pro turned out to share.
STATUS, SPEAK, STOP, GET_INFO, SET_INFO = 0, 1, 2, 5, 6
SO_CURRENT_VOICE, SO_RATE = 0x63766F78, 0x72617465

#: `ttvi 11` is the PowerPC build of the same engine and must not come along.
SKIP = (("ttvi", 11),)
WANT = ("ttvi", "ttss", "ttsp", "thng", "vers", "STR ")


def extract(image, out):
    """Pull the engine and its voices out of a disk image into `out`."""
    import extract_rom
    import rsrc
    files = extract_rom.open_image(image)
    if files is None:
        raise SystemExit("not a disk image (needs machfs): %s" % image)

    def dump(path, sub, want=None):
        data, res, _creator = files[path]
        folder = os.path.join(out, sub)
        os.makedirs(folder, exist_ok=True)
        rows = 0
        for r in rsrc.parse(res):
            if want and r.type not in want:
                continue
            if (r.type, r.id) in SKIP:
                continue
            with open(os.path.join(folder, "%s_%d.bin" % (r.type, r.id)),
                      "wb") as fh:
                fh.write(r.data)
            rows += 1
        print("  %-30s -> %-18s %2d resources" % (path.split("/")[-1], sub, rows))

    dump("System Folder/Extensions/MacinTalk 3", "macintalk3", WANT)
    for path in sorted(files):
        if "/Voices/" not in path:
            continue
        try:
            import rsrc as _r
            kinds = {x.type for x in _r.parse(files[path][1])}
        except Exception:
            continue
        # A MacinTalk 3 voice is a ttvd and nothing else, or a ttvd plus its
        # own ttvw for the singing ones. Anything with ttvi or gtss belongs to
        # another engine.
        if "ttvd" in kinds and not (kinds & {"ttvi", "gtss", "ttvw2"}):
            dump(path, "voices/" + path.split("/")[-1])


def load_folder(h, folder):
    n = 0
    for name in sorted(os.listdir(folder)):
        stem, ext = os.path.splitext(name)
        if ext != ".bin" or "_" not in stem:
            continue
        rtype, rid = stem.rsplit("_", 1)
        try:
            rid = int(rid)
        except ValueError:
            continue
        if rtype == "thng":
            continue
        with open(os.path.join(folder, name), "rb") as fh:
            h.add_resource(rtype, rid, fh.read())
        n += 1
    return n


def build(out, voice_id=1):
    """-> (host, instance). Open only; the caller decides what to ask next."""
    with open(os.path.join(out, "macintalk3", "ttvi_10.bin"), "rb") as fh:
        code = fh.read()
    h = osp.Host()
    # `rtd` at +0x234 is 68010+, and only a 68040 answers Open with 0.
    h.set_cpu(osp.Host.CPU_68040)
    h.load(CODE, code)
    h.heap(HEAP, HEAP_SIZE)
    h.mem_traps(True)
    h.w8(CPUFLAG, 0)
    h.w16(RESERR, 0)
    h.w16(MEMERR, 0)
    load_folder(h, os.path.join(out, "macintalk3"))
    # **Only the voice being used.** `MAX_RES` in osp_host.c is 64, the engine
    # takes 18, and the image carries twenty MacinTalk 3 voices -- registering
    # them all overflows the table. MacinTalk 3 is formant, so a voice is a
    # 714-byte `ttvd` and the data lives in the engine; there is nothing to
    # gain from loading the others.
    vdir = os.path.join(out, "voices")
    for v in sorted(os.listdir(vdir)):
        folder = os.path.join(vdir, v)
        ids = [f for f in os.listdir(folder) if f == "ttvd_%d.bin" % voice_id]
        if ids:
            load_folder(h, folder)
            print("voice  %s (mtk3 id %d)" % (v, voice_id))
            break
    else:
        raise SystemExit("no voice with mtk3 id %d under %s" % (voice_id, vdir))
    h.add_voice("mtk3", voice_id, voice_id)
    tok = h.open_instance(h.add_component("ttsc", "mtk3", "mtk3", CODE))
    h.set_reg(osp.A7, STACK)
    h.set_reg(osp.SR, 0x2700)
    # Not optional. See the module docstring.
    h.defer_callbacks(True)
    reason, result = h.component_call(tok, OPEN, [tok], max_instr=50_000_000)
    print("Open -> %s, %d" % (osp.STOP[reason], signed(result)))
    return h, tok


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--extract", metavar="IMAGE",
                    help="pull MacinTalk 3 out of a disk image first")
    ap.add_argument("--out", default=DEFAULT_OUT)
    ap.add_argument("--speak", metavar="TEXT")
    ap.add_argument("--voice", type=int, default=1, help="mtk3 id; Fred is 1")
    a = ap.parse_args()

    if a.extract:
        extract(a.extract, a.out)
    if not os.path.isdir(os.path.join(a.out, "macintalk3")):
        raise SystemExit("nothing in %s -- run with --extract first" % a.out)

    h, tok = build(a.out, a.voice)
    h.w32(VOICE_SPEC, int.from_bytes(b"mtk3", "big"))
    h.w32(VOICE_SPEC + 4, a.voice)
    r, res = h.component_call(tok, SET_INFO, [SO_CURRENT_VOICE, VOICE_SPEC],
                              max_instr=50_000_000)
    print("SetSpeechInfo('cvox') -> %s, %d" % (osp.STOP[r], signed(res)))
    h.w32(PARAM_BUF, int(180 * 65536))
    r, res = h.component_call(tok, SET_INFO, [SO_RATE, PARAM_BUF],
                              max_instr=50_000_000)
    print("SetSpeechInfo('rate') -> %s, %d" % (osp.STOP[r], signed(res)))

    if a.speak:
        raw = a.speak.encode("mac-roman", "replace")
        h.load(TEXT_BUF, raw + b"\0")
        r, res = h.component_call(tok, SPEAK, [TEXT_BUF, len(raw), 0],
                                  max_instr=400_000_000)
        print("SpeakBuffer -> %s, %d" % (osp.STOP[r], signed(res)))
        while h.buffers_taken < 400:
            if not h.lib.osp_run_callbacks(4, 200_000_000):
                break
        pcm = h.pcm
        print("audio  %d buffers, %d bytes (%.2f s), faults %d"
              % (h.buffers_taken, len(pcm), len(pcm) / 22254.0, h.fault_count))
        if pcm:
            print("       range %d..%d" % (min(pcm), max(pcm)))

    un = {}
    for pc, w, d0, a0, a1, served in h.traps:
        if not served:
            un[(w, pc)] = un.get((w, pc), 0) + 1
    print("unserved traps:", ["$%04X %s x%d" % (w, trap_name(w) or "?", c)
                              for (w, pc), c in un.items()] or "none")
    return 0


if __name__ == "__main__":
    sys.exit(main())

# ---------------------------------------------------------------------------
# IT SPEAKS, and the Sound Manager was never the problem.
#
# For a long time this stopped after two buffers -- 4,480 bytes, 0.20 s -- and
# then ran away into four hundred million bus faults. The recorded diagnosis
# was that `SndDoCommand` must COPY each command into the channel's queue, the
# way real hardware does, so the engine is free to reuse its scratch. That was
# wrong, and it was wrong in a way worth remembering: the host had copied the
# command into `CB_SCRATCH` all along. The note and the code disagreed, and
# nobody had gone back to check which one was lying.
#
# What it actually was:
#
#   ttvi10+0x08E2   pea      $f6(a4)        push the destination
#   ttvi10+0x08E6   _Microseconds           ($A193)
#   ttvi10+0x08E8   movea.l  (a7)+,a1       pop it back -- untouched
#   ttvi10+0x08EA   move.l   a0,(a1)+       high word from A0
#   ttvi10+0x08EC   move.l   d0,(a1)        low  word from D0
#
# That is Apple's own `Microseconds` glue. **The trap returns the count in the
# A0/D0 pair and writes no memory**; the pointer belongs to the glue. This host
# read the C prototype instead -- `Microseconds(UnsignedWide *)` -- and stored
# eight bytes through A0, which on entry is whatever the caller left there.
#
# In MacinTalk 3 it landed exactly on the engine's own SndCommand at 0x000BD864
# and rewrote its `param2`. The callback trampoline at ttvi10+0x135E takes that
# word as a record pointer, reads a routine out of `$b4` of it, and jumps:
#
#     movea.l  $8(a6),a3      the SndCommand
#     movea.l  $4(a3),a4      param2 -- the word the clock had just eaten
#     tst.l    $b4(a4)
#     movea.l  $b4(a4),a0
#     jsr      (a0)           -> 0x2E091F0F, and away
#
# It ran that trampoline 41 times correctly during one SpeakBuffer. The 42nd
# was the first one after _Microseconds fired. The clock was corrupting the
# thing it was timing.
#
# Fixed in src/osp_host_sane.c. Fred, Kathy, Princess, Junior and Ralph now
# render 3.19 s of speech-shaped audio for the same sentence, with zero faults
# and five distinct renders -- F0 106, 210, 242, 212 and 71 Hz, which is
# exactly the right order for those five voices. MacinTalk 2 and MacinTalk Pro
# renders are byte-identical across the change.
#
# Ruled out by measurement, so nobody repeats them:
#   * a Sound Manager command queue -- the command sequence was already
#     correct and in order: bufferCmd, callBackCmd, bufferCmd, callBackCmd;
#   * the clock as a *source* -- zero references to $016A in any of the three
#     code resources, `_TickCount` never called, `auto_ticks(True)` changes
#     nothing. (The clock was guilty of something else entirely.)
#   * the in-call callback delay -- `osp_cb_wait` swept 100..1,000,000 gives
#     byte-identical results;
#   * resource ORDER -- the engine does not index its ttvi resources.
#
# Still to do before this is an add-on: a `macintalk3.py` engine module, the
# voice-folder gating, and a person listening to it. Extraction still refuses
# MacinTalk 3 by name until then.
# ---------------------------------------------------------------------------
