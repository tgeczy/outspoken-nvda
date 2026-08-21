# -*- coding: utf-8 -*-
"""Shared checks for the pitch slider, one engine at a time.

Not a test module. These live here rather than in a single `test_pitch.py`
because **two engines cannot be alive in one test module**: `osp_init()`
resets the emulator's globals, and `Host.close()` deliberately drains the
DLL's reference count with `FreeLibrary` so a stale binary cannot be left
locked. Build a MacinTalk Pro engine while a MacinTalk 2 one is still held and
the second teardown reaches into an unmapped module -- an access violation
*after* every test has reported success, which is the worst way to find out.

So each engine's pitch tests sit in that engine's own test module, next to the
rest of its fixtures, and the assertions they share sit here.
"""

#: What the driver hands the engines at each end of NVDA's 0-100 slider.
ENDS = ((0, -120), (25, -60), (50, 0), (75, 60), (100, 120))

PHRASE = "Voice testing, one two three."


def live(eng):
    assert not eng._dead, (
        "this engine was killed by another one built after it -- only one "
        "can be alive at a time, so keep them in separate test modules")
    return eng


def sane_base(eng):
    """The voice's own 'pbas', and a check that it is not hertz.

    Every voice measured sits between 38 and 61. Anything near 110 or 250
    would mean the selector had gone back to answering in frequency, which is
    the mistake that kept this slider switched off for months.
    """
    base = live(eng).base_pitch()
    assert base is not None, "the engine would not report the voice's pitch"
    assert 20.0 < base < 90.0, (
        "'pbas' answered %r, which looks like hertz" % base)
    return base


def takes_the_offset(eng):
    """Set each end of the slider and read the selector back.

    Reading back rather than trusting the result code, which on MacinTalk Pro
    is not a result code at all: it answers 'pbas' with the frequency it just
    computed, so a perfectly good call looks like OSErr -32314.
    """
    base = sane_base(eng)
    for _slider, tenths in ENDS:
        eng.set_pitch(tenths)
        got = eng.current_pitch()
        assert abs(got - (base + tenths / 10.0)) < 0.01, (
            "asked for %+d tenths from %.1f, engine holds %.3f"
            % (tenths, base, got))
    eng.set_pitch(0)


def slider_is_audible(eng):
    """The bug all of this exists for: a slider that changes nothing.

    Byte comparison rather than a pitch measurement, because the failure being
    guarded against is not "slightly wrong pitch" -- it is three renders that
    are the same bytes, which is what an ignored selector produces and what
    was actually shipped.
    """
    live(eng)
    out = {}
    for tenths in (-120, 0, 120):
        eng.set_pitch(tenths)
        out[tenths] = bytes(eng.speak(eng.translate(PHRASE)))
        assert out[tenths], "no audio at %+d tenths" % tenths
    eng.set_pitch(0)
    assert out[-120] != out[0], "an octave down changed nothing"
    assert out[120] != out[0], "an octave up changed nothing"
    assert out[-120] != out[120], "both ends of the slider are the same audio"
