# -*- coding: utf-8 -*-
"""Open MacinTalk 2 and ask it to say something.

Milestone 3.  `probe_mt2_open.py` gets both components initialised; this goes
one further and issues a speak call, then reports whatever audio came out.

The selector map, read off the front end's handlers rather than recalled:

    0  +$5CE   status -- forwards 'stat' to the back end and fills a
                SpeechStatusInfo, field for field
    1  +$3F2   SpeakBuffer(textBuf, byteLen, controlFlags) -- three arguments,
                and it tests the last against 2, which is `kNoSpeechInterrupt`
    2  +$4B0   StopSpeech(whereToStop) -- selector 1 calls it internally with
                0, and `kImmediate` is 0

    py -3 tools/probe_mt2_speak.py
    py -3 tools/probe_mt2_speak.py "some other words"
    py -3 tools/probe_mt2_speak.py "some other words" RoboVox

The ten MacinTalk 2 voices are Ben, Boris, Brenda, Mariel, Marvin, Mr. Hughes,
Otis, RoboVox, Votron and Xero; `py -3 tools/voices.py --engine mtk2` lists
whichever of them you have extracted.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import osp                                                     # noqa: E402
import paths                                                   # noqa: E402
from disasm import trap_name                                   # noqa: E402
from probe_mt2_open import (FRONT_BASE, BACK_BASE, HEAP, HEAP_SIZE, STACK,
                            MEMERR, RESERR, CPUFLAG, TABLES, CM_NAMES,
                            OPEN, rom, signed)                 # noqa: E402

SPEAK, STOP, STATUS = 1, 2, 0
#: Selector 6 takes (OSType, void *) and its first comparison is against
#: 'char' -- soCharacterMode -- so it is SetSpeechInfo.  Selector 5 is the
#: same shape and rejects a nil pointer, which makes it the getter.
SET_INFO, GET_INFO = 6, 5
TEXT_BUF = 0x00195000
VOICE_SPEC = 0x00196100

SO_CURRENT_VOICE = 0x63766F78          # 'cvox', from Apple's Speech.h

#: A MacinTalk 2 voice is ttvi + ttvd + ttvw.  Ben is the smallest complete
#: one, which makes it the cheapest thing to try first.
VOICE = "Ben"


def load_voice(h, name):
    """Register a voice's three resources under their own ids."""
    for root in paths.roots():
        d = os.path.join(root, "voices", name)
        if not os.path.isdir(d):
            continue
        out = []
        for f in sorted(os.listdir(d)):
            rtype, rest = f.split("_", 1)
            rid = int(rest.split(".")[0])
            data = open(os.path.join(d, f), "rb").read()
            h.add_resource(rtype, rid, data)
            out.append("%s %d (%d bytes)" % (rtype, rid, len(data)))
        return out
    return []


def main():
    text = sys.argv[1] if len(sys.argv) > 1 else "Hello, this is MacinTalk."
    voice_name = sys.argv[2] if len(sys.argv) > 2 else VOICE
    front = open(rom("Cecy_3.bin"), "rb").read()
    back = open(rom("Cecy_1.bin"), "rb").read()

    h = osp.Host()
    h.load(FRONT_BASE, front)
    h.load(BACK_BASE, back)
    h.heap(HEAP, HEAP_SIZE)
    h.mem_traps(True)
    h.w8(CPUFLAG, 0)
    h.w16(RESERR, 0)
    h.w16(MEMERR, 0)

    for rtype, rid in TABLES:
        p = paths.find("%s_%d.bin" % (rtype, rid))
        if p:
            h.add_resource(rtype, rid, open(p, "rb").read())
    got = load_voice(h, voice_name)
    print("voice %s: %s"
          % (voice_name, ", ".join(got) if got else "NOT FOUND"))

    fe = h.add_component("ttsc", "mtk2", "mtk2", FRONT_BASE)
    h.add_component("t2be", "t2be", "mtk2", BACK_BASE)
    chan = h.open_instance(fe)

    h.set_reg(osp.A7, STACK)
    h.set_reg(osp.SR, 0x2700)

    reason, result = h.component_call(chan, OPEN, [chan], max_instr=50_000_000)
    print("open:  %s, result %d" % (osp.STOP[reason], signed(result)))
    if reason != 1 or signed(result) != 0:
        print("  cannot speak until Open succeeds -- run probe_mt2_open.py")
        return 1

    # A VoiceSpec is {creator, id}, and both come straight out of the voice's
    # own ttvd -- see tools/voices.py.
    import voices
    vs = [v for v in voices.installed()[0] if v.name == voice_name]
    if vs:
        v = vs[0]
        h.w32(VOICE_SPEC + 0, int.from_bytes(v.creator.encode("mac-roman"),
                                             "big"))
        h.w32(VOICE_SPEC + 4, v.id)
        # The host plays Speech Manager: GetVoiceInfo('fref') answers with the
        # ttvd id, which is what the engine actually opens the voice by.
        ttvd_id = int(os.path.basename(v.files["ttvd"]).split("_")[1]
                      .split(".")[0])
        h.add_voice(v.creator, v.id, ttvd_id)
        mark = h.lib.osp_reslog_n()
        r, res = h.component_call(chan, SET_INFO,
                                  [SO_CURRENT_VOICE, VOICE_SPEC],
                                  max_instr=50_000_000)
        print("set 'cvox' to %s (creator %r id 0x%08X): %s, result %d"
              % (v.name, v.creator, v.id, osp.STOP[r], signed(res)))
        missing = [x for x in h.resource_requests[mark:] if not x[2]]
        print("  resources it asked for: %s"
              % ", ".join("%s %d%s" % (t, i, "" if ok else " MISSING")
                          for t, i, ok in h.resource_requests[mark:]) or "none")
        if missing:
            print("  -> %d not registered" % len(missing))

    raw = text.encode("mac-roman", "replace")
    h.load(TEXT_BUF, raw)
    print("\nspeaking %r (%d bytes) at 0x%X" % (text, len(raw), TEXT_BUF))

    before = len(h.traps)
    h.pcm_reset()
    h.defer_callbacks(True)
    reason, result = h.component_call(chan, SPEAK, [TEXT_BUF, len(raw), 0],
                                      max_instr=400_000_000)

    # SpeakBuffer returns as soon as the first buffer is queued, so the host
    # has to keep being the Sound Manager until the engine stops asking.
    rounds = h.run_callbacks()
    print("  sound callbacks run: %d%s"
          % (rounds, "   <-- STALLED, not finished" if rounds >= 4096 else ""))

    print("\n$A82A calls during the speak:")
    for d0, pc, csp, words, served in h.cm_log:
        if pc < FRONT_BASE:
            continue
        where = ("front+0x%05X" % (pc - FRONT_BASE) if pc < BACK_BASE
                 else "back+0x%05X" % (pc - BACK_BASE))
        if not served:
            print("  %-16s D0=0x%08X  *** UNKNOWN ***" % (where, d0))
            print("      stack: %s" % " ".join("0x%08X" % w for w in words))

    print("\nnew traps:")
    seen = {}
    for i, (pc, word, d0, a0, a1, served) in enumerate(h.traps):
        if i < before:
            continue
        key = (word, served)
        seen[key] = seen.get(key, 0) + 1
        if not served:
            where = ("front+0x%05X" % (pc - FRONT_BASE) if pc < BACK_BASE
                     else "back+0x%05X" % (pc - BACK_BASE))
            print("  %-16s %-22s <-- STUBBED" % (where, trap_name(word)))
    for (word, served), n in sorted(seen.items()):
        print("  %-22s x%-5d %s"
              % (trap_name(word), n, "" if served else "STUBBED"))

    # Selector 0 fills a SpeechStatusInfo -- outputBusy, outputPaused,
    # inputBytesLeft, phonemeCode -- which says whether the text was merely
    # queued or actually consumed.
    STATUS_BUF = 0x00196000
    for off in range(0, 12, 4):
        h.w32(STATUS_BUF + off, 0)
    sr, _ = h.component_call(chan, STATUS, [STATUS_BUF], max_instr=20_000_000)
    if sr == 1:
        print("\n  status: outputBusy=%d outputPaused=%d inputBytesLeft=%d "
              "phonemeCode=%d"
              % (h.r8(STATUS_BUF), h.r8(STATUS_BUF + 1),
                 h.r32(STATUS_BUF + 2), h.r16(STATUS_BUF + 6)))
    else:
        print("\n  status: call did not return (%s)" % osp.STOP[sr])

    print("\n  stop:          %s" % osp.STOP[reason], end="")
    if reason == 3:
        print("   vector %d (%s) at 0x%X"
              % (h.stop_vector, osp.VECTORS.get(h.stop_vector, "?"),
                 h.stop_pc))
    elif reason == 1:
        print("   <-- speak RETURNED, result = %d" % signed(result))
    else:
        print("   at 0x%X" % h.stop_pc)
    print("  instructions:  %d" % h.instr)
    print("  faults:        %d" % h.fault_count)
    print("  buffers taken: %d" % h.buffers_taken)
    print("  PCM:           %d samples" % len(h.pcm))
    if h.pcm:
        rate = h.sample_rate or 22254.0
        print("  sample rate:   %.1f Hz -> %.2f seconds"
              % (rate, len(h.pcm) / rate))
        out = os.path.join(os.path.dirname(os.path.dirname(
            os.path.abspath(__file__))), "build", "mt2-%s.wav" % voice_name.replace(" ", "_"))
        write_wav(out, h.pcm, rate)
        print("  wrote          %s" % out)
    return 0


def write_wav(path, pcm, rate):
    """8-bit unsigned mono, which is what the Sound Manager hands over."""
    import struct
    n = len(pcm)
    with open(path, "wb") as f:
        f.write(b"RIFF" + struct.pack("<I", 36 + n) + b"WAVEfmt ")
        f.write(struct.pack("<IHHIIHH", 16, 1, 1, int(rate), int(rate), 1, 8))
        f.write(b"data" + struct.pack("<I", n) + pcm)


if __name__ == "__main__":
    sys.exit(main())
