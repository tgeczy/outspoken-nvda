# -*- coding: utf-8 -*-
"""Speak, change voice, speak again -- and measure what comes out.

Issue #1. Switching MacinTalk 2 voices leaves the synthesizer buzzing, and
NVDA's log says why in one number: the utterance spoken straight after a
switch renders **33 to 37 seconds of audio** for a phrase worth two, often
hitting `Engine.MAX_BUFFERS`. That blob is what is heard as buzzing.

The first suspect was a threading race -- `select()` ran on NVDA's main thread
while the worker was rendering -- and it was real and is fixed. It was not
this: the log still shows the long renders with every engine call on the
worker. So the fault is in what `select()` leaves behind, and that needs no
threads at all to reproduce.

This drives `macintalk2.Engine` exactly as the driver does, with nothing else
in the picture:

    py -3 tools/probe_mt2_switch.py
    py -3 tools/probe_mt2_switch.py Otis RoboVox Otis
"""
import os
import sys
import types

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

# macintalk2 imports NVDA's logger inside speak(). Outside NVDA there is none,
# and the warning it emits at the buffer ceiling is exactly what we are here
# to see, so it is printed rather than swallowed.
if "logHandler" not in sys.modules:
    _m = types.ModuleType("logHandler")

    class _Log(object):
        def warning(self, m, *a, **k):
            print("      LOG: " + (m % a if a else m))
        info = debug = error = warning
    _m.log = _Log()
    sys.modules["logHandler"] = _m

sys.path.insert(0, os.path.join(
    os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
    "addon", "synthDrivers", "_outspoken"))

import paths                                                   # noqa: E402
import macintalk2                                              # noqa: E402

RATE = 232                             # the driver's midpoint
PHRASE = "Voice testing, one two three."


def secs(pcm):
    return len(pcm) / float(macintalk2.NATIVE_RATE)


def main():
    order = sys.argv[1:]
    files, allv = macintalk2.find(paths.roots())
    if not allv:
        raise SystemExit("no MacinTalk 2 voices found; run tools/extract_rom.py")
    byname = {v.name: v for v in allv}
    if not order:
        # Two voices, back and forth: a switch, a switch back, and a repeat of
        # each without a switch. The repeats are the control -- if only the
        # post-switch utterances are long, the switch is what does it.
        a, b = allv[0].name, allv[1].name
        order = [a, a, b, b, a, b, a]
    missing = [n for n in order if n not in byname]
    if missing:
        raise SystemExit("no such voice: %s\nhave: %s"
                         % (", ".join(missing), ", ".join(sorted(byname))))

    eng = macintalk2.Engine(files, allv, byname[order[0]])
    eng.set_rate(RATE)
    print("MacinTalk 2, rate %d, %r\n" % (RATE, PHRASE))
    print("  %-12s %-9s %8s  %8s  %s"
          % ("voice", "switched", "buffers", "audio", ""))

    baseline = {}
    bad = 0
    for i, name in enumerate(order):
        switched = (i > 0 and order[i - 1] != name) or i == 0
        if i > 0 and order[i - 1] != name:
            ok = eng.select(byname[name])
            if not ok:
                print("  %-12s select REFUSED" % name)
                continue
            # The driver re-applies the rate after every switch, so do that
            # here too rather than testing a path nobody runs.
            eng.set_rate(RATE)
        before = eng.h.buffers_taken
        pcm = eng.speak(eng.translate(PHRASE))
        took = eng.h.buffers_taken - before
        s = secs(pcm)
        # First clean render of a voice is its own yardstick.
        if name not in baseline and not switched:
            baseline[name] = s
        ref = baseline.get(name)
        flag = ""
        if ref and s > ref * 2:
            flag = "  <-- %.1fx its own baseline" % (s / ref)
            bad += 1
        elif s > 10:
            flag = "  <-- far too long for this phrase"
            bad += 1
        print("  %-12s %-9s %8d  %7.2f s%s"
              % (name, "yes" if switched and i else "", took, s, flag))

    eng.close()
    print("\n  %d of %d utterances were oversized" % (bad, len(order)))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
