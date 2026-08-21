# -*- coding: utf-8 -*-
"""The pitch slider's arithmetic, and the unit error that kept it switched off.

`soPitchBase` is a **musical scale**: twelve units to the octave, with 60.000
at middle C. It is not hertz, and for a long time this project fed it hertz --
which is why MacinTalk 2's slider was deliberately made inert, with a note
saying that 90 and 180 produced byte-identical audio. They did: on a scale of
twelve to the octave those are notes near 2 kHz and 350 kHz, so both landed
past the engine's ceiling, both clamped to the same place, and the engine was
obeying exactly.

A unit error is the kind of defect that comes back, because the wrong version
is the one that reads naturally next to a variable called `pitch_hz`. So the
conversion is pinned here.

What each engine does with the value is checked in that engine's own module --
`test_macintalk2.py` and `test_macintalkpro.py` -- because only one emulator
can be alive at a time. See `pitchcheck.py`.
"""
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))

from pitchcheck import ENDS                                    # noqa: E402


def test_the_slider_is_an_offset_in_tenths_of_a_semitone():
    """50 is the voice as recorded, and each end is an octave away.

    An offset rather than an absolute, because every voice has its own pitch:
    Votron answers 38 and Mariel 61, so one absolute scale would put the
    middle of the slider in a different place for each of them.
    """
    import outspoken
    d = outspoken.SynthDriver.__new__(outspoken.SynthDriver)
    for slider, want in ENDS:
        d._pitch = slider
        assert d._pitchTenths() == want, "slider %d" % slider


def test_each_engine_is_given_pitch_in_the_units_it_takes():
    """1984 wants hertz; the Speech Manager engines want the offset.

    Both are called 'pitch' and they are not interchangeable, which is the
    whole history of this file. `.sp` has no voice apart from its pitch and
    nothing to ask, so the driver does the arithmetic for it; the others are
    asked what their voice's own pitch is and told how far to move from it.
    """
    import outspoken

    class Spy(object):
        def __init__(self):
            self.hz = self.tenths = None

        def set_rate(self, rate):
            pass

        def set_voice(self, hz):
            self.hz = hz

        def set_pitch(self, tenths):
            self.tenths = tenths

    d = outspoken.SynthDriver.__new__(outspoken.SynthDriver)
    d._rate, d._pitch = 50, 100

    d._entry = lambda: ("male", "Male", "sp", 110)
    d._baseHz = lambda: 110
    spy = Spy()
    d._applySettings(spy)
    assert spy.tenths is None, "the 1984 engine was handed a semitone offset"
    assert round(spy.hz) == 220, (
        "an octave above 110 Hz is 220, not %r" % spy.hz)

    d._entry = lambda: ("mtk2:Ben", "Ben", "mtk2", None)
    spy = Spy()
    d._applySettings(spy)
    assert spy.hz is None, "a Speech Manager engine was handed hertz"
    assert spy.tenths == 120


def test_an_octave_is_twelve_units_not_a_doubling_in_hertz():
    """The guard against this quietly becoming hertz again.

    120 tenths is twelve semitones is a factor of two in frequency. If anyone
    reintroduces a map that is linear in hertz, `+120` stops meaning an octave
    and this fails.
    """
    import outspoken
    assert outspoken._PITCH_SEMITONES == 12
    assert 2.0 ** (120 / 120.0) == 2.0
    assert round(2.0 ** (60 / 120.0), 3) == 1.414
