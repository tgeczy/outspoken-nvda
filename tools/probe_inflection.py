# -*- coding: utf-8 -*-
"""What does 'pmod' actually do -- measured, one engine per process.

    py -3 tools/probe_inflection.py mt2
    py -3 tools/probe_inflection.py mt3 Fred
    py -3 tools/probe_inflection.py pro Bruce --cliff

`soPitchMod` is the Speech Manager's pitch modulation: how far the contour is
allowed to move, as opposed to `soPitchBase`, which is where it sits. NVDA
calls the same idea *inflection*. One engine per process because only one
emulator may be alive at a time -- see `tests/pitchcheck.py`.

**What it found, 2026-08-21**, and every number here came out of this file:

  * **Each voice has its own depth, and the driver must ask.** MacinTalk 3
    answers five different values across nineteen voices -- 50.000 for Fred,
    Ralph and Whisper, 39.999 for Junior, Kathy and Princess, 25.000 for
    Boing, 12.500 for Albert and Bahh, and 0.000 for the nine novelty voices.
    MacinTalk Pro's three sit far lower: Agnes 5.688, Victoria 6.000, Bruce
    8.000. MacinTalk 2 answers 100.000 for eight of ten and 0.000 for RoboVox
    and Xero.
  * **MacinTalk 2's 'pmod' has two states and not a scale.** 6.25, 12.5, 25
    and 50 all read back as 100.000 and render to the same bytes. Only zero is
    different. The slider means flat or not-flat there, and nothing between.
  * **MacinTalk 3 and Pro are continuous.** Every value is stored exactly as
    given and every one renders differently.
  * **Sending the queried default is byte-identical to sending nothing**, so
    the middle of the slider is safe. Worth checking separately: for 'pbas'
    the value was right long before the call was harmless. Watch for the
    first render after an engine opens, which differs on MacinTalk 2 whatever
    is set -- warm up before comparing anything.
  * **Zero is not a depth every voice can be given.** Four of MacinTalk 3's
    nine flat voices -- Bad News, Bells, Good News, Hysterical -- ignore the
    selector outright at any value; Cellos, Deranged, Pipe Organ, Trinoids
    and Zarvox take an absolute depth and change.

**And one real bug, which is why `--cliff` exists.** A 'pmod' near zero makes
MacinTalk Pro loop forever inside SpeakBuffer: a render that costs 4.3 million
instructions was still going after three billion, with no fault, no trap and
no audio. **The threshold belongs to the voice, not the engine** -- on a
four-sentence text Bruce hangs up to 0.05, Agnes up to 0.025, and Victoria
only at exactly zero. It also needs more than one clause, so the first probe
here, on a single sentence, reported the selector harmless. Both of those are
why `macintalkpro.INFLECTION_FLOOR` is ten times the worst of them rather
than something fitted to whichever voice was tried first.
"""
import hashlib
import os
import sys

_HERE = os.path.dirname(os.path.abspath(__file__))
_ROOT = os.path.dirname(_HERE)
for _p in (os.path.join(_ROOT, "addon", "synthDrivers"),
           os.path.join(_ROOT, "addon", "synthDrivers", "_outspoken"),
           _HERE):
    if _p not in sys.path:
        sys.path.insert(0, _p)

import osp                                                     # noqa: E402
import paths                                                   # noqa: E402

GET_INFO, SET_INFO, SPEAK = 5, 6, 1
SO_PITCH_MOD = 0x706D6F64              # 'pmod'

MODULES = {"mt2": "macintalk2", "mt3": "macintalk3", "pro": "macintalkpro"}

RATE = 200

#: One clause never reproduced Pro's hang and four did. Length is part of the
#: measurement here, not decoration.
TEXT = ("The rain in Spain falls mainly on the plain. "
        "Is that really what you meant? I don't believe it! "
        "Nineteen, twenty, twenty one. What a curious afternoon.")

#: Values to try when hunting the hang. Everything at or below a voice's own
#: threshold costs a full instruction budget to prove, so keep the list short.
CLIFF = (0.0, 0.01, 0.02, 0.025, 0.03, 0.05, 0.1, 0.25, 0.5, 1.0)


def fixed(x):
    return int(round(x * 65536.0)) & 0xFFFFFFFF


def unfixed(u):
    if u & 0x80000000:
        u -= 1 << 32
    return u / 65536.0


def get_mod(eng, mod):
    eng.h.w32(mod.PARAM_BUF, 0)
    reason, result = eng.h.component_call(
        eng.chan, GET_INFO, [SO_PITCH_MOD, mod.PARAM_BUF],
        max_instr=200_000_000)
    if reason != 1 or result != 0:
        return None
    return unfixed(eng.h.r32(mod.PARAM_BUF))


def set_mod(eng, mod, value):
    """Straight at the selector, not through `set_inflection`.

    The point of a probe is to see what the engine does, not what the driver's
    policy lets through -- the floor and the reference depth are exactly the
    things being checked.
    """
    eng.h.w32(mod.PARAM_BUF, fixed(value))
    reason, _res = eng.h.component_call(
        eng.chan, SET_INFO, [SO_PITCH_MOD, mod.PARAM_BUF],
        max_instr=200_000_000)
    return reason


def build(kind, want):
    mod = __import__(MODULES[kind])
    files, voices = mod.find(paths.roots())
    if not voices:
        raise SystemExit("no %s voices; run tools/extract_rom.py" % kind)
    voice = voices[0]
    if want:
        match = [v for v in voices if v.name.lower() == want.lower()]
        if not match:
            raise SystemExit("%r is not installed; have %s"
                             % (want, ", ".join(v.name for v in voices)))
        voice = match[0]
    eng = mod.Engine(files, voices, voice)
    eng.set_rate(RATE)
    return mod, eng, voice, voices


def sweep(mod, eng, voice):
    """The scale: what each value stores, and whether it changes the audio."""
    base = get_mod(eng, mod)
    print("%s: the voice's own 'pmod' is %s"
          % (voice.name, "unreadable" if base is None else "%.3f" % base))
    if base is None:
        return
    # **Warm up on the text being measured, not on a short one.** The first
    # render after an engine opens differs from every later one, and on
    # MacinTalk 2 a short warm-up does not settle it -- the baseline came out
    # 164 bytes short and every row below it looked like a change.
    eng.speak(eng.translate(TEXT))
    untouched = bytes(eng.speak(eng.translate(TEXT)))
    assert untouched == bytes(eng.speak(eng.translate(TEXT))), (
        "this engine is not rendering the same text the same way twice")
    print("  %10s %10s %8s   %s" % ("asked", "readback", "bytes", "sha"))
    print("  %10s %10s %8d   %s"
          % ("(not set)", "--", len(untouched),
             hashlib.sha1(untouched).hexdigest()[:12]))
    wanted = sorted({0.0, base * 0.25, base * 0.5, base, base * 1.5,
                     base * 2.0, base * 4.0, 25.0, 100.0})
    # **Do not sweep an engine into its own hang.** One wedged render poisons
    # every value after it, so the whole table below a floor comes back empty
    # and looks like the selector is broken at every setting. `--cliff` is the
    # tool for the bottom end, and it builds a fresh engine per value.
    floor = getattr(mod, "INFLECTION_FLOOR", 0.0)
    for value in wanted:
        value = round(min(100.0, value), 3)
        if value < floor:
            print("  %10.3f %10s %8s   below this engine's floor of %.3f;"
                  " try --cliff" % (value, "--", "--", floor))
            continue
        set_mod(eng, mod, value)
        back = get_mod(eng, mod)
        pcm = bytes(eng.speak(eng.translate(TEXT)))
        note = ""
        if pcm == untouched:
            note = "   <- same audio as never setting it"
        print("  %10.3f %10.3f %8d   %s%s"
              % (value, -1 if back is None else back, len(pcm),
                 hashlib.sha1(pcm).hexdigest()[:12], note))


def cliff(kind, want):
    """Where the engine stops coming back. A fresh engine per value.

    Fresh because a hung render poisons everything after it: the first probe
    to find this reported the *next* four values as broken too, which sent it
    looking for a state bug that was not there.
    """
    for value in CLIFF:
        mod, eng, voice, _all = build(kind, want)
        set_mod(eng, mod, value)
        raw = eng.translate(TEXT).strip().encode("mac-roman", "replace")
        eng.h.pcm_reset()
        eng.h.load(mod.TEXT_BUF, raw)
        reason, _res = eng.h.component_call(
            eng.chan, SPEAK, [mod.TEXT_BUF, len(raw), 0],
            max_instr=120_000_000)
        print("  %-12s pmod %6.3f -> %-20s %d instructions"
              % (voice.name, value, osp.STOP.get(reason, reason), eng.h.instr))
        eng.close()


def main(argv):
    if not argv or argv[0] not in MODULES:
        print(__doc__)
        return 2
    kind = argv[0]
    rest = [a for a in argv[1:] if not a.startswith("--")]
    want = rest[0] if rest else None
    if "--cliff" in argv:
        cliff(kind, want)
        return 0
    mod, eng, voice, _all = build(kind, want)
    sweep(mod, eng, voice)
    eng.close()
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
