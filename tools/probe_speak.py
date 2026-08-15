# -*- coding: utf-8 -*-
"""Make MacinTalk speak, and write what comes out to a WAV.

The sequence, per docs/driver-api.md and docs/sound-model.md:

    Open                       -> allocates dCtlStorage, loads TALK 1
    driver+$001E (MACSTARTSOUND) with {channel, bufferA, bufferB}
    Prime (_Write) with the text
    ...harvest PCM at every bufferCmd

The driver never allocates the sound buffers -- MACSTARTSOUND is handed them --
so the host provides the channel and two blocks of 22 + 3870 bytes.

    py -3 tools/probe_speak.py "Welcome to outSPOKEN."
"""
import os
import struct
import sys
import wave

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import osp                                                    # noqa: E402
from disasm import trap_name                                  # noqa: E402

RSRC = r"C:\git\outspoken-rsrc"
DRVR = os.path.join(RSRC, "DRVR", "1030_.sp.bin")
OUT = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                   "build", "spoken.wav")

DRV_BASE = 0x00040000
HEAP, HEAP_SIZE = 0x00080000, 0x00080000
STACK = 0x00200000
WORK = 0x00190000          # our own structures, clear of the heap

CPUFLAG, RESERR = 0x012F, 0x0A60

EXPORT_MACSTARTSOUND = 0x001E
EXPORT_MACSTOPSOUND = 0x0022
EXPORT_STOPSPEECH = 0x0026

BUF_BYTES = 22 + 3870


def setup(text, before_prime=None):
    """Run Open / MACSTARTSOUND / Prime.

    `before_prime` is called with the Host once everything is staged but
    before Prime runs -- the only moment where a probe can arm a watchpoint
    or a snapshot and still see the synthesiser from its first instruction.
    """
    image = open(DRVR, "rb").read()
    opn, prime, ctl, status, close = osp.driver_entries(image)

    h = osp.Host()
    h.load(DRV_BASE, image)
    h.heap(HEAP, HEAP_SIZE)
    h.mem_traps(True)
    h.w8(CPUFLAG, 0)
    h.w16(RESERR, 0)
    h.add_resource("TALK", 1, open(os.path.join(RSRC, "TALK", "1001.bin"),
                                   "rb").read())

    dce, pb = WORK, WORK + 0x100
    for off in range(0, 0x80, 4):
        h.w32(dce + off, 0)
        h.w32(pb + off, 0)
    h.w32(dce + 0, DRV_BASE)
    h.w16(dce + 4, 0x4600)
    h.w16(dce + 24, 0xFFEF)
    h.w16(pb + 24, 0xFFEF)

    h.set_reg(osp.SR, 0x2700)
    h.set_reg(osp.A7, STACK)
    h.set_reg(osp.A0, pb)
    h.set_reg(osp.A1, dce)
    r = h.call(DRV_BASE + opn, max_instr=5_000_000)
    if r != 1:
        raise RuntimeError("Open did not return: %s" % osp.STOP[r])
    print("Open: ok (%d traps, %d stubbed)" % (len(h.traps), h.stubbed))

    # --- the channel and the two buffers ---------------------------------
    chan = WORK + 0x400
    bufa = WORK + 0x1000
    bufb = WORK + 0x3000
    rec = WORK + 0x300
    for off in range(0, 0x80, 4):
        h.w32(chan + off, 0)
    # ChannelBusy (+$4EAE) short-circuits on `chan+$20 == -1` and reports the
    # channel idle without asking the Sound Manager at all.  That is exactly our
    # model -- buffers are consumed the instant they are handed over -- and it
    # keeps WaitSoundDone from spinning out its full one-second tick timeout.
    h.w16(chan + 0x20, 0xFFFF)
    for base in (bufa, bufb):
        for off in range(0, BUF_BYTES + 4, 4):
            h.w32(base + off, 0)
    h.w32(rec + 0, chan)
    h.w32(rec + 4, bufa)
    h.w32(rec + 8, bufb)

    # The synthesiser calls a stop-speech hook through `jsr $0.l` at +$28D2,
    # and SetStopSpeechCallback (+$0034) patches that instruction's *operand*
    # in place.  Genuinely self-modifying code -- which is why Prime saves and
    # restores CACR on anything past a 68000.  Until it is set, the address is
    # zero and the first call jumps to 0.
    #
    # It is not a notification.  It is part of the frame loader, and it owns
    # the first two bytes of every frame.
    #
    # Frames are 8 bytes.  The steady loader at +$28DA consumes only 6, and the
    # selector reads at +$294E take `-$5(a6)`, `-$4(a6)` and `-$3(a6)`, which
    # are frame bytes 3, 4 and 5 only once a6 has reached f+8.  The two missing
    # bytes are read here, exactly as the first-frame loader at +$27E4 reads
    # them: f[0] and f[1], the low halves of the formant 1 and 2 increments.
    #
    # +$27E4 also fixes the flag contract -- it does `move.b (a6)+, $1(a5)` and
    # then `bmi` straight to the end-of-speech `rts`.  Bit 7 of f[0] is the
    # terminator, and after the first frame the `bmi` at +$28D8 is the ONLY way
    # steady state ends.  A hook that always answers "keep going" runs off the
    # end of the buffer and plays heap for ever, which is what it did: 2167
    # buffers, 377 seconds, 0.3% non-silent.
    #
    # The second `move.b` clobbers N, so N is restored from the stored byte.
    stop_hook = WORK + 0x200
    for i, w in enumerate((0x1B5E, 0x0001,     # move.b (a6)+, $1(a5)
                           0x1B5E, 0x0003,     # move.b (a6)+, $3(a5)
                           0x4A2D, 0x0001,     # tst.b  $1(a5)  -- restore N
                           0x4E75)):           # rts
        h.w16(stop_hook + 2 * i, w)
    h.set_reg(osp.A7, STACK)
    r = h.call_with_args(DRV_BASE + 0x0034, [stop_hook], max_instr=1000)
    if r != 1:
        raise RuntimeError("SetStopSpeechCallback: %s" % osp.STOP[r])
    print("stop-speech hook: patched into +$28D4 -> 0x%X" % h.r32(DRV_BASE + 0x28D4))

    h.set_reg(osp.A7, STACK)
    h.set_reg(osp.A1, dce)
    r = h.call_with_args(DRV_BASE + EXPORT_MACSTARTSOUND, [rec],
                         max_instr=10_000_000)
    print("MACSTARTSOUND: %s" % osp.STOP[r])
    if r != 1:
        report(h, DRV_BASE)
        raise RuntimeError("MACSTARTSOUND failed")

    for name, base in (("A", bufa), ("B", bufb)):
        print("  buffer %s: length=%d rate=0x%08X base=%d"
              % (name, h.r32(base + 4), h.r32(base + 8), h.r8(base + 0x15)))
    print("  channel callBack = 0x%X (driver+0x%X)"
          % (h.r32(chan + 8), h.r32(chan + 8) - DRV_BASE))

    # --- speak ------------------------------------------------------------
    # ioBuffer is a HANDLE, not a pointer.  The synthesiser at +$2EE does
    #     movea.l $20(a0), a0 ; _HLock ; move.l (a0), $96(a5)
    # -- it locks it and then dereferences it.  Passing the text address
    # directly makes it read the first four characters as a pointer, and the
    # only symptom is that Prime returns immediately having said nothing.
    txt = WORK + 0x8000
    txt_h = WORK + 0x7000
    raw = text.encode("mac-roman")
    h.load(txt, raw)
    h.w32(txt_h, txt)
    h.w32(pb + 32, txt_h)        # ioBuffer -- a Handle
    h.w32(pb + 36, len(raw))     # ioReqCount; the synth reads its low word
    h.w32(pb + 40, 0)            # ioActCount
    h.w16(pb + 16, 1)            # ioResult = in progress

    h.pcm_reset()
    if before_prime:
        before_prime(h)
    h.set_reg(osp.A7, STACK)
    h.set_reg(osp.A0, pb)
    h.set_reg(osp.A1, dce)
    r = h.call(DRV_BASE + prime, max_instr=400_000_000)
    print("\nPrime: %s after %d instructions" % (osp.STOP[r], h.instr))
    return h, r


def report(h, base):
    print("\n  traps:")
    seen = {}
    for pc, word, d0, a0, a1, served in h.traps:
        key = (pc, word)
        seen[key] = seen.get(key, 0) + 1
    for (pc, word), n in list(seen.items())[:24]:
        print("    +%05X  %-26s x%d" % (pc - base, trap_name(word), n))
    if h.stop == 3:
        print("  stopped: vector %d (%s) at driver+0x%X"
              % (h.stop_vector, osp.VECTORS.get(h.stop_vector, "?"),
                 h.stop_pc - base))
    print("  stubbed traps: %d   faults: %d" % (h.stubbed, h.fault_count))
    for addr, pc, wr, sz in h.faults[:6]:
        print("      %s%d at 0x%08X from driver+0x%X"
              % ("write" if wr else "read", sz * 8, addr, pc - base))


def main():
    text = sys.argv[1] if len(sys.argv) > 1 else "Welcome to outSPOKEN."
    print("text: %r\n" % text)
    h, r = setup(text)
    report(h, DRV_BASE)

    pcm = h.pcm
    print("\n  buffers taken: %d   short: %d   samples: %d"
          % (h.buffers_taken, h.short_buffers, len(pcm)))
    if not pcm:
        print("\n  NO AUDIO")
        return 1
    rate = h.sample_rate or 22254.5454
    print("  sample rate:   %.4f Hz (from the SoundHeader)" % rate)
    print("  duration:      %.2f s" % (len(pcm) / rate))

    silent = sum(1 for b in pcm if b in (0x80, 0x60))
    print("  non-silent:    %d of %d samples (%.1f%%)"
          % (len(pcm) - silent, len(pcm), 100.0 * (len(pcm) - silent) / len(pcm)))
    print("  range:         %d .. %d" % (min(pcm), max(pcm)))

    # 8-bit unsigned is exactly what WAV wants, so this is a straight copy.
    with wave.open(OUT, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(1)
        w.setframerate(int(round(rate)))
        w.writeframes(pcm)
    print("\n  wrote %s" % OUT)
    return 0


if __name__ == "__main__":
    sys.exit(main())
