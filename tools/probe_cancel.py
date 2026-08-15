# -*- coding: utf-8 -*-
"""Stop an utterance half way through, then speak again.

NVDA calls `cancel()` constantly -- every keystroke while speech is running --
so the driver stands or falls on this, and the engine already has the mechanism.
The routine installed by driver+$0034 is called once per frame and the `bmi` at
+$28D8 tests what it returns.  That is the designed cancel path, and it is why
the export is named SetStopSpeechCallback rather than something about frames.

So the hook grows a flag byte the host can set from outside:

    tst.b   FLAG            ; set by Python, mid-utterance
    bne.s   stop
    move.b  (a6)+, $1(a5)   ; the normal per-frame work
    move.b  (a6)+, $3(a5)
    tst.b   $1(a5)          ; N = end-of-speech bit of f[0]
    rts
  stop:
    moveq   #-1, d0         ; N set -- "stop now"
    rts

What must hold afterwards: `Prime` returns cleanly rather than wedging, the
audio is short rather than truncated mid-buffer, and a *second* utterance on
the same engine still works.  A synthesiser that cannot be interrupted twice is
no use in a screen reader.

    py -3 tools/probe_cancel.py
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import osp                                                    # noqa: E402
import probe_speak as ps                                      # noqa: E402

FLAG = ps.WORK + 0x280
HOOK = ps.WORK + 0x200
CALLBACK_PC = 0x28D2          # the `jsr $0.l` that reaches our hook


def install_hook(h):
    """The stop-capable hook, replacing the one probe_speak installs."""
    w = [0x4A39, (FLAG >> 16) & 0xFFFF, FLAG & 0xFFFF,   # tst.b FLAG.l
         0x660E,                                          # bne.s -> +22
         0x1B5E, 0x0001,                                  # move.b (a6)+,$1(a5)
         0x1B5E, 0x0003,                                  # move.b (a6)+,$3(a5)
         0x4A2D, 0x0001,                                  # tst.b  $1(a5)
         0x4E75,                                          # rts
         0x70FF,                                          # moveq #-1,d0
         0x4E75]                                          # rts
    for i, x in enumerate(w):
        h.w16(HOOK + 2 * i, x)
    h.w8(FLAG, 0)


def main():
    text = "AY1 KAEN SPIY1K AXGEH1N"

    # --- 1. interrupt it -------------------------------------------------
    def arm(h):
        install_hook(h)
        h.snap_at(ps.DRV_BASE + CALLBACK_PC, halt_on=40)   # 40 frames in

    h, r = ps.setup(text, before_prime=arm)
    if r != 5:
        print("FAIL: expected a breakpoint, got %s" % osp.STOP[r])
        return 1
    print("\n  broke at frame 40, %d samples so far" % len(h.pcm))

    h.w8(FLAG, 1)                       # <- what cancel() will do
    h.snap_at(0, halt_on=0)             # disarm, so we run to the end
    r = h.resume()
    short = len(h.pcm)
    print("  after stop: %s, %d samples (%.2f s), %d buffers"
          % (osp.STOP[r], short, short / (h.sample_rate or 22254.5), h.buffers_taken))
    if r != 1:
        print("FAIL: engine did not return cleanly after a stop")
        return 1

    # A stop must not leave a half-buffer of noise: the driver pads to the end
    # of the buffer it is filling, so the sample count stays a whole number of
    # buffers and the tail is silence.
    tail = h.pcm[-64:]
    print("  final 8 bytes: %s" % " ".join("%02X" % b for b in tail[-8:]))
    print("  buffer-aligned: %s" % (short % 3870 == 0))

    # --- 2. speak again on the SAME engine -------------------------------
    # Not a fresh Host: that would prove nothing about state left behind by an
    # interrupted utterance, which is the only thing at issue here.
    image = open(ps.DRVR, "rb").read()
    prime = osp.driver_entries(image)[1]
    pb, txt, txt_h = ps.WORK + 0x100, ps.WORK + 0x8000, ps.WORK + 0x7000
    raw = text.encode("mac-roman")

    h.w8(FLAG, 0)                       # release the stop
    h.load(txt, raw)
    h.w32(txt_h, txt)
    h.w32(pb + 32, txt_h)
    h.w32(pb + 36, len(raw))
    h.w32(pb + 40, 0)
    h.w16(pb + 16, 1)
    h.pcm_reset()
    h.set_reg(osp.A7, ps.STACK)
    h.set_reg(osp.A0, pb)
    h.set_reg(osp.A1, ps.WORK)
    r2 = h.call(ps.DRV_BASE + prime, max_instr=400_000_000)
    full = len(h.pcm)
    print("\n  second utterance, same engine: %s, %d samples (%.2f s), %d buffers"
          % (osp.STOP[r2], full, full / (h.sample_rate or 22254.5),
             h.buffers_taken))

    ok = (r2 == 1 and full > short and h.buffers_taken > 0)
    print("\n  %s  stop works, and the engine speaks again afterwards"
          % ("PASS:" if ok else "FAIL:"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
