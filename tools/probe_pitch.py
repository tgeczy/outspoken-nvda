# -*- coding: utf-8 -*-
"""What does 'pbas' actually do -- measured, one engine at a time.

MacinTalk 2's pitch was wired long ago and then deliberately switched off,
because setting it produced audio that made no sense: 90 and 180 gave *byte
identical* renders while both halved the peak amplitude.  That was recorded as
"something about the value or its units is wrong", and left alone.

The sibling project answers it.  `tiger_host_serve.c` measured 'pbas' 40 ->
109 Hz and 50 -> 193 Hz -- a ratio of 1.77 across ten units, which is 2^(10/12)
to three digits.  **'pbas' is a musical scale at twelve units to the octave,
not hertz.**  So 90 and 180 asked for roughly 2 kHz and 350 kHz, both far past
anything a formant synthesizer will do; both clamped to the same ceiling, which
is exactly why they rendered identically and sounded wrong.

That is a good story and this file exists because a good story is not a
measurement.  It sweeps the selector and reports what came out:

    py -3 tools/probe_pitch.py mt2
    py -3 tools/probe_pitch.py pro Agnes
    py -3 tools/probe_pitch.py mt2 Ben --text "The rain in Spain."

Three things are being asked, and the table answers all three:

  * **Does GetSpeechInfo('pbas') return the voice's own pitch?**  If it comes
    back somewhere in 30..70 the units are settled before a single sample is
    rendered, and the offset model needs that number anyway.
  * **Is the scale twelve to the octave here too?**  Six units should multiply
    F0 by 1.414 and twelve should double it.
  * **Does the amplitude drop come from the value or from the call?**  The
    sweep renders once with no 'pbas' sent at all, and once with the voice's
    own queried default.  If those two differ, the *call* is the problem and
    the driver must not send anything at the middle of the slider.  If they
    match, the old 78 -> 37 was just what a voice sounds like clamped at an
    absurd ceiling, and there is nothing to work around.

A fresh host per sweep point, deliberately: re-speaking on one instance is the
path the driver takes, but it is also the path where a leftover setting looks
like a result.

**What it found, 2026-08-21.** All three questions answered, and the story
held:

  * `GetSpeechInfo('pbas')` answers **60.000** for Ben and **56.000** for
    Agnes -- Apple's musical scale, with 60 at middle C. Not hertz.
  * Twelve units to the octave, on both engines. Ben: -24 gives 0.253 of the
    base frequency against a predicted 0.250, -6 gives 0.722 against 0.707,
    +6 gives 1.391 against 1.414. Agnes: -12 gives 0.507 against 0.500, +12
    gives 1.982 against 2.000.
  * **The call is harmless; only the value was wrong.** Sending the queried
    default renders byte-identically to sending nothing at all, so there is
    nothing to work around at the middle of the slider. The recorded 78 -> 37
    amplitude drop does not reproduce: peak is 58 at every value including
    90 and 180.

Each engine has a ceiling and neither of them hurts. MacinTalk 2 renders
identically from 'pbas' 72 upward -- 72, 78, 84, 90 and 180 are the same
bytes -- and MacinTalk Pro saturates near 404 Hz at about 69, and clamps the
selector itself at 100.

One real bug fell out of it. Above 'pbas' 69, and only on longer text, Pro
issues SANE `_FP68K` opword $0015 to clear its exception flags; the host did
not serve it, so `sane_fail` took vector 10 and the utterance died partway
through. Served now -- see `src/osp_host_sane.c`. Nothing had ever asked this
engine for a high pitch, so nothing had ever reached it.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import osp                                                     # noqa: E402
import paths                                                   # noqa: E402
import voices as voicelib                                      # noqa: E402

STATUS, SPEAK, STOP, GET_INFO, SET_INFO = 0, 1, 2, 5, 6

SO_CURRENT_VOICE = 0x63766F78          # 'cvox'
SO_RATE = 0x72617465                   # 'rate'
SO_PITCH_BASE = 0x70626173             # 'pbas'

NATIVE_RATE = 22254
TEXT = "The rain in Spain falls mainly on the plain."

#: Sent before the sweep so a pitch change is not confused with a rate change.
RATE = 180


def fixed(x):
    return int(round(x * 65536.0)) & 0xFFFFFFFF


def unfixed(u):
    """Fixed 16.16 back to a float, signed."""
    if u & 0x80000000:
        u -= 1 << 32
    return u / 65536.0


# -- measuring what came out ----------------------------------------------
def f0(pcm, rate=NATIVE_RATE, lo=50.0, hi=500.0):
    """Median fundamental across the voiced frames, by autocorrelation.

    Per-frame and median rather than one autocorrelation over the whole
    utterance: speech is not a steady tone, and a single lag over a sentence
    locks onto whatever vowel happened to last longest.

    -> (hz, voiced_frames) and (0.0, 0) when nothing was voiced.
    """
    if not pcm:
        return 0.0, 0
    x = [b - 128.0 for b in pcm]
    n = len(x)
    win = int(rate * 0.040)                  # 40 ms, a few periods at 100 Hz
    step = win // 2
    lag_lo, lag_hi = int(rate / hi), int(rate / lo)
    if n < win or lag_hi >= win:
        return 0.0, 0

    out = []
    # Loud frames only. An unvoiced frame has a best lag too, and it is noise.
    energies = []
    for s in range(0, n - win, step):
        e = sum(v * v for v in x[s:s + win])
        energies.append((e, s))
    if not energies:
        return 0.0, 0
    peak_e = max(e for e, _ in energies)
    if peak_e <= 0:
        return 0.0, 0

    for e, s in energies:
        if e < peak_e * 0.25:                # quiet: silence or a stop closure
            continue
        f = x[s:s + win]
        best, best_r = 0, 0.0
        for lag in range(lag_lo, lag_hi):
            r = 0.0
            for i in range(win - lag_hi):
                r += f[i] * f[i + lag]
            if r > best_r:
                best_r, best = r, lag
        # A voiced frame correlates with itself a period later. 0.3 is loose
        # on purpose -- the job is to reject noise, not to grade a voice.
        if best and best_r > 0.3 * e:
            # Autocorrelation reports an octave too low whenever a period fits
            # twice in the window, and this sweep is *about* octaves -- an
            # uncorrected estimator would read a doubling as a halving. If half
            # the winning lag correlates nearly as well, half is the period.
            for div in (2, 3):
                half = best // div
                if half < lag_lo:
                    continue
                r = 0.0
                for i in range(win - lag_hi):
                    r += f[i] * f[i + half]
                if r > 0.85 * best_r:
                    best = half
                    break
            out.append(rate / float(best))
    if not out:
        return 0.0, 0
    out.sort()
    return out[len(out) // 2], len(out)


def peak(pcm):
    return max(abs(b - 128) for b in pcm) if pcm else 0


# -- the two engines -------------------------------------------------------
def build_mt2(voice_name):
    """Front end, back end, both tables, one voice, opened. -> (h, chan, v)"""
    from probe_mt2_open import (FRONT_BASE, BACK_BASE, HEAP, HEAP_SIZE, STACK,
                                MEMERR, RESERR, CPUFLAG, TABLES, OPEN, rom,
                                signed)
    from probe_mt2_speak import load_voice

    h = osp.Host()
    h.load(FRONT_BASE, open(rom("Cecy_3.bin"), "rb").read())
    h.load(BACK_BASE, open(rom("Cecy_1.bin"), "rb").read())
    h.heap(HEAP, HEAP_SIZE)
    h.mem_traps(True)
    h.w8(CPUFLAG, 0)
    h.w16(RESERR, 0)
    h.w16(MEMERR, 0)
    for rtype, rid in TABLES:
        p = paths.find("%s_%d.bin" % (rtype, rid))
        if p:
            h.add_resource(rtype, rid, open(p, "rb").read())
    load_voice(h, voice_name)

    fe = h.add_component("ttsc", "mtk2", "mtk2", FRONT_BASE)
    h.add_component("t2be", "t2be", "mtk2", BACK_BASE)
    chan = h.open_instance(fe)
    h.set_reg(osp.A7, STACK)
    h.set_reg(osp.SR, 0x2700)
    reason, result = h.component_call(chan, OPEN, [chan], max_instr=50_000_000)
    if reason != 1 or signed(result) != 0:
        raise RuntimeError("MacinTalk 2 Open failed (%s, %d)"
                           % (osp.STOP[reason], signed(result)))

    v = [x for x in voicelib.installed()[0] if x.name == voice_name]
    if not v:
        raise RuntimeError("voice %r is not installed" % voice_name)
    v = v[0]
    ttvd = int(os.path.basename(v.files["ttvd"]).split("_")[1].split(".")[0])
    h.add_voice(v.creator, v.id, ttvd)
    from probe_mt2_open import CM_NAMES                        # noqa: F401
    VOICE_SPEC = 0x00196100
    h.w32(VOICE_SPEC, int.from_bytes(v.creator.encode("mac-roman"), "big"))
    h.w32(VOICE_SPEC + 4, v.id)
    r, res = h.component_call(chan, SET_INFO, [SO_CURRENT_VOICE, VOICE_SPEC],
                              max_instr=50_000_000)
    if r != 1 or signed(res) != 0:
        raise RuntimeError("'cvox' refused %s" % voice_name)
    return h, chan, v, 0x00195000, 0x00196200


def build_pro(voice_name):
    from probe_pro_open import (TEXT_BUF, PARAM_BUF, VOICE_SPEC, build,
                                signed)
    h, tok, voice, (reason, result) = build(voice_name)
    if reason != 1 or result != 0:
        raise RuntimeError("MacinTalk Pro Open failed (%s, %d)"
                           % (osp.STOP[reason], signed(result)))
    creator = voice.creator.encode("mac-roman", "replace")
    h.w32(VOICE_SPEC, int.from_bytes(creator[:4].ljust(4, b" "), "big"))
    h.w32(VOICE_SPEC + 4, voice.id)
    r, res = h.component_call(tok, SET_INFO, [SO_CURRENT_VOICE, VOICE_SPEC],
                              max_instr=200_000_000)
    if r != 1 or signed(res) != 0:
        raise RuntimeError("'cvox' refused %s" % voice.name)
    return h, tok, voice, TEXT_BUF, PARAM_BUF


ENGINES = {"mt2": build_mt2, "pro": build_pro}
DEFAULT_VOICE = {"mt2": "Ben", "pro": "Agnes"}


def render(engine, voice_name, text, pbas):
    """One utterance. `pbas` of None means send no 'pbas' at all.

    -> (pcm, queried_default_or_None)
    """
    h, tok, _v, text_buf, param = ENGINES[engine](voice_name)

    # Ask the voice what its own pitch is. This is the number the offset model
    # needs, and getting a sane one back settles the units on its own.
    got = None
    h.w32(param, 0)
    r, res = h.component_call(tok, GET_INFO, [SO_PITCH_BASE, param],
                              max_instr=50_000_000)
    if r == 1 and res == 0:
        got = unfixed(h.r32(param))

    h.w32(param, fixed(RATE))
    h.component_call(tok, SET_INFO, [SO_RATE, param], max_instr=50_000_000)

    if pbas is not None:
        h.w32(param, fixed(pbas))
        r, _res = h.component_call(tok, SET_INFO, [SO_PITCH_BASE, param],
                                   max_instr=50_000_000)
        if r != 1:
            return "SetSpeechInfo('pbas') did not return (%s)" % osp.STOP[r], got
        # Deliberately NOT trusting the result code. MacinTalk Pro returns the
        # frequency it computed rather than an OSErr -- 'pbas' 44 gives 49379
        # and 56 gives 33222, which is 49379 doubled and truncated to sixteen
        # bits, exactly the octave those twelve units are worth. Reading the
        # selector back says whether it took; D0 here says nothing at all.
        h.w32(param, 0)
        h.component_call(tok, GET_INFO, [SO_PITCH_BASE, param],
                         max_instr=50_000_000)
        back = unfixed(h.r32(param))
        if abs(back - pbas) > 0.01:
            return ("asked for %.1f, reads back %.3f" % (pbas, back)), got

    raw = text.encode("mac-roman", "replace")
    h.load(text_buf, raw)
    h.pcm_reset()
    h.defer_callbacks(True)
    r, res = h.component_call(tok, SPEAK, [text_buf, len(raw), 0],
                              max_instr=400_000_000)
    if r != 1 or res != 0:
        return ("SpeakBuffer -> %s, result %d"
                % (osp.STOP[r], res - (1 << 32) if res & 0x80000000
                   else res)), got
    h.run_callbacks()
    return h.pcm, got


def main():
    engine = sys.argv[1] if len(sys.argv) > 1 else "mt2"
    if engine not in ENGINES:
        print("usage: probe_pitch.py {mt2|pro} [voice] [--text ...]")
        return 2
    rest = sys.argv[2:]
    text = TEXT
    if "--text" in rest:
        i = rest.index("--text")
        text = rest[i + 1]
        rest = rest[:i] + rest[i + 2:]
    voice_name = rest[0] if rest else DEFAULT_VOICE[engine]

    print("%s, voice %s, rate %d wpm\n  %r\n"
          % (engine, voice_name, RATE, text))

    # No 'pbas' at all: the baseline every other row is compared against.
    base_pcm, queried = render(engine, voice_name, text, None)
    if isinstance(base_pcm, str) or not base_pcm:
        print("the baseline utterance produced nothing -- fix that first")
        print("  %s" % base_pcm)
        return 1
    b_hz, b_n = f0(base_pcm)
    print("GetSpeechInfo('pbas') -> %s"
          % ("%.3f" % queried if queried is not None else "REFUSED"))
    print("  (a value near 30..70 means a musical scale; near 100..200 means "
          "hertz)\n")

    print("  %-14s %7s %7s %7s %6s  %s"
          % ("pbas", "F0 Hz", "vs base", "expect", "peak", "samples"))
    print("  %-14s %7.1f %7s %7s %6d  %d"
          % ("(not sent)", b_hz, "-", "-", peak(base_pcm), len(base_pcm)))

    # The queried default first, then a symmetric sweep around it. If the
    # engine refused the query, fall back to the scale the sibling project
    # measured, where 40 is a low male voice.
    centre = queried if queried else 40.0
    steps = [("default", centre)]
    for d in (-24, -18, -12, -6, -3, 3, 6, 9, 12, 18, 24):
        steps.append(("%+d" % d, centre + d))
    steps += [("abs 40", 40.0), ("abs 50", 50.0)]
    # The two values that produced identical audio and half the amplitude.
    steps += [("abs 90", 90.0), ("abs 180", 180.0)]

    renders = {}
    for label, val in steps:
        pcm, _ = render(engine, voice_name, text, val)
        if isinstance(pcm, str) or not pcm:
            print("  %-14s   %s" % ("%s (%.1f)" % (label, val), pcm))
            continue
        hz, nv = f0(pcm)
        renders[label] = bytes(pcm)
        ratio = (hz / b_hz) if b_hz and hz else 0.0
        expect = 2.0 ** ((val - centre) / 12.0) if centre else 0.0
        print("  %-14s %7.1f %7.3f %7.3f %6d  %d%s"
              % ("%s (%.1f)" % (label, val), hz, ratio, expect, peak(pcm),
                 len(pcm), "" if nv else "   <-- nothing voiced"))

    # Byte identity locates a clamp without trusting the estimator at all: two
    # different requests that render the same bytes were not both honoured.
    groups = {}
    for label, val in steps:
        if label in renders:
            groups.setdefault(renders[label], []).append("%.0f" % val)
    dupes = [v for v in groups.values() if len(v) > 1]
    print("\n  identical renders (a group of more than one is a clamp):")
    if dupes:
        for g in dupes:
            print("    pbas %s" % ", ".join(g))
    else:
        print("    none -- every value produced its own audio")

    if "default" in renders:
        same = renders["default"] == bytes(base_pcm)
        print("  sending the queried default == sending nothing: %s%s"
              % (same, "" if same else
                 "   <-- the CALL changes the audio, not just the value"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
