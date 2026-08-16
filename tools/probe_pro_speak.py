# -*- coding: utf-8 -*-
"""Ask MacinTalk Pro to say something, and report whatever came out.

`probe_pro_open.py` gets the component open; this goes one further.

The selector map is Pro's own, read off the jump table at gtse 1 +$BC rather
than assumed from MacinTalk 2 -- though it turns out to be identical, which is
what you would hope from two components of the same 'ttsc' type.  The argument
byte count each stub loads into d6 is the giveaway:

    0  +$D0   4 bytes   SpeechStatus(SpeechStatusInfo *)
    1  +$D8  12 bytes   SpeakBuffer(textBuf, byteLen, controlFlags)
    2  +$E0   4 bytes   StopSpeech(whereToStop)
    5  +$100  8 bytes   GetSpeechInfo(selector, void *)
    6  +$108  8 bytes   SetSpeechInfo(selector, void *)

Audio is expected to leave the same way `.sp` and MacinTalk 2's does, through
$A804/$A800, which the host already models -- and Pro checks TrapAvailable for
$A082 during Open, which is _DTInstall, so it very likely renders from a
DeferredTask exactly as MacinTalk 2 does.  Both of those are expectations, not
findings; the trap log below is what settles them.

    py -3 tools/probe_pro_speak.py
    py -3 tools/probe_pro_speak.py "some other words" Victoria
"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import osp                                                     # noqa: E402
from disasm import trap_name                                   # noqa: E402
from probe_pro_open import (TEXT_BUF, PARAM_BUF, STATUS_BUF, VOICE_SPEC, CODE,
                            build, signed)                     # noqa: E402

STATUS, SPEAK, STOP, GET_INFO, SET_INFO = 0, 1, 2, 5, 6

#: Speech Manager selectors, from Apple's Speech.h -- the same ones MacinTalk 2
#: is driven with.
SO_CURRENT_VOICE = 0x63766F78          # 'cvox'
SO_RATE = 0x72617465                   # 'rate'

#: A ceiling on one utterance. Reaching it means the engine is producing
#: without ever finishing, which is a finding rather than an utterance.
MAX_BUFFERS = 400

NATIVE_RATE = 22254
TEXT = "Hello, this is MacinTalk Pro."
OUT = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                   "build", "pro-spoken.wav")


def fixed(x):
    return int(round(x * 65536.0)) & 0xFFFFFFFF


def wav(path, pcm8, rate=NATIVE_RATE):
    """8-bit unsigned mono, which is what the Sound Manager deals in."""
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as f:
        f.write(b"RIFF" + struct.pack("<I", 36 + len(pcm8)) + b"WAVEfmt ")
        f.write(struct.pack("<IHHIIHH", 16, 1, 1, rate, rate, 1, 8))
        f.write(b"data" + struct.pack("<I", len(pcm8)) + pcm8)


def main():
    text = sys.argv[1] if len(sys.argv) > 1 else TEXT
    want = sys.argv[2] if len(sys.argv) > 2 else None

    h, tok, voice, (reason, result) = build(want)
    if reason != 1 or result != 0:
        print("Open failed (%s, result %d) -- run tools/probe_pro_open.py"
              % (osp.STOP[reason], signed(result)))
        return 1
    print("open           %s, noErr\n" % voice.name)

    mark_traps = len(h.traps)

    # The voice first, because an open synthesizer has not been told which one
    # to be: every selector after Open answered -244, voiceNotFound, until this
    # was sent. A VoiceSpec is (creator, id), passed by address like everything
    # else SetSpeechInfo takes.
    creator = voice.creator.encode("mac-roman", "replace")
    h.w32(VOICE_SPEC, int.from_bytes(creator[:4].ljust(4, b" "), "big"))
    h.w32(VOICE_SPEC + 4, voice.id)
    r, res = h.component_call(tok, SET_INFO, [SO_CURRENT_VOICE, VOICE_SPEC],
                              max_instr=200_000_000)
    print("SetSpeechInfo('cvox', %s id %d) -> %s, %d%s"
          % (voice.creator, voice.id, osp.STOP[r],
             signed(res) if r == 1 else -1, whined(h)))

    # Rate next, so a wrong one is not mistaken for a broken renderer. Every
    # SetSpeechInfo selector takes a POINTER, including the scalar ones --
    # that cost a day on MacinTalk 2, where passing 232 directly had the engine
    # dereference address $00E80000 and quietly corrupt itself.
    h.w32(PARAM_BUF, fixed(180))
    r, res = h.component_call(tok, SET_INFO, [SO_RATE, PARAM_BUF],
                              max_instr=50_000_000)
    print("SetSpeechInfo('rate', 180) -> %s, %d"
          % (osp.STOP[r], signed(res) if r == 1 else -1))

    raw = text.encode("mac-roman", "replace")
    h.load(TEXT_BUF, raw)
    h.pcm_reset()
    # MacinTalk 2's callback only refills on its second invocation, so the host
    # holds callbacks until the engine is between calls. Pro is expected to
    # want the same; if it does not, the symptom is silence and the trap log
    # will show the callback never being answered.
    h.defer_callbacks(True)

    r, res = h.component_call(tok, SPEAK, [TEXT_BUF, len(raw), 0],
                              max_instr=400_000_000)
    print("SpeakBuffer(%r) -> %s, %d\n"
          % (text[:40], osp.STOP[r], signed(res) if r == 1 else -1))
    if r != 1:
        report(h, mark_traps)
        if r == 3:
            print("  vector %d (%s) at 0x%X"
                  % (h.stop_vector, osp.VECTORS.get(h.stop_vector, "?"),
                     h.stop_pc))
        return 1

    # Speaking is asynchronous: SpeakBuffer queues the first buffer and
    # returns, and everything after it only exists if the host keeps being the
    # Sound Manager. The stopping condition has to come from the ENGINE --
    # pumping until nothing is pending gave 131 seconds of silence on
    # MacinTalk 2, because a real-time synthesiser keeps its channel fed
    # whether or not it has anything left to say.
    rounds = 0
    while h.buffers_taken < MAX_BUFFERS:
        rounds += 1
        if not h.run_callbacks(max_rounds=8):
            break
        if not busy(h, tok):
            break
    else:
        print("  hit the %d buffer ceiling -- producing without finishing"
              % MAX_BUFFERS)

    pcm = h.pcm
    print("audio          %d buffers taken, %d bytes, %.2f s"
          % (h.buffers_taken, len(pcm), len(pcm) / float(NATIVE_RATE)))
    if pcm:
        lo, hi = min(pcm), max(pcm)
        live = sum(1 for c in pcm if c != 0x80)
        print("               range %d..%d around silence 128, %d live samples"
              % (lo, hi, live))
        wav(OUT, bytes(pcm))
        print("               -> %s" % os.path.relpath(OUT, os.getcwd()))
        if lo == hi:
            print("  ! flat: the engine cleared its buffers and never filled "
                  "them")
    else:
        print("  ! nothing at all -- see the traps below")

    report(h, mark_traps)
    return 0 if pcm else 1


def whined(h):
    """What the component last reported through SetComponentInstanceError.

    The engine names its own error on the way out of a failed selector, which
    beats inferring one from where it stopped -- and until that Component
    Manager selector was served it halted instead, turning "the call failed"
    into "unhandled exception" and hiding the reason entirely.
    """
    e = h.lib.osp_instance_error()
    return "   (it reported %d)" % e if e else ""


def busy(h, tok):
    """SpeechStatusInfo.outputBusy, which is how a synthesizer says it is done."""
    for off in (0, 4, 8):
        h.w32(STATUS_BUF + off, 0)
    r, _res = h.component_call(tok, STATUS, [STATUS_BUF], max_instr=20_000_000)
    if r != 1:
        return False
    return bool(h.r8(STATUS_BUF)) or bool(h.r32(STATUS_BUF + 2))


def report(h, mark):
    seen = {}
    for pc, word, d0, a0, a1, served in h.traps[mark:]:
        nm = trap_name(word)
        e = seen.setdefault(nm, [0, served, pc])
        e[0] += 1
        e[1] = e[1] and served
    print("\ntraps during speaking:")
    for nm in sorted(seen):
        cnt, served, pc = seen[nm]
        print("  %-24s x%-4d %s%s"
              % (nm, cnt, "+%05X" % (pc - CODE) if pc >= CODE else "0x%X" % pc,
                 "" if served else "   <-- STUBBED"))
    bad = sorted(k for k, v in seen.items() if not v[1])
    if bad:
        print("\n  NOT SERVED, and this is the list that matters:")
        for k in bad:
            print("     %s" % k)
    print("\n  instructions:  %d" % h.instr)
    print("  faults:        %d" % h.fault_count)
    for addr, pc, wr, sz in h.faults[:6]:
        print("      %s%d at 0x%08X from 0x%X"
              % ("write" if wr else "read", sz * 8, addr, pc))


if __name__ == "__main__":
    sys.exit(main())
