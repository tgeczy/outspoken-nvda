# -*- coding: utf-8 -*-
"""Shared checks for the inflection slider, one engine at a time.

Not a test module, and here for the same reason `pitchcheck.py` is: **two
engines cannot be alive in one test module**. `osp_init()` resets the
emulator's globals and `Host.close()` drains the DLL's reference count, so a
second engine built while the first is held kills it, and the teardown of the
dead one reaches into an unmapped module -- an access violation *after* every
test has reported success.

What the slider means, in one place, because three engines implement it:

    0    as flat as this engine will safely go
    50   the voice exactly as Apple recorded it
    100  twice its own modulation depth

`'pmod'` is a depth rather than a position, so the slider scales it instead of
offsetting it -- which is the whole difference from `pbas` and the pitch
slider next door.
"""

#: NVDA's slider, and what each end asks of a voice whose own depth is `base`.
ENDS = (0, 25, 50, 75, 100)

PHRASE = ("The rain in Spain falls mainly on the plain. "
          "Is that really what you meant? I don't believe it! "
          "Nineteen, twenty, twenty one. What a curious afternoon.")


def live(eng):
    assert not eng._dead, (
        "this engine was killed by another one built after it -- only one "
        "can be alive at a time, so keep them in separate test modules")
    return eng


def sane_base(eng):
    """The voice's own 'pmod', and a check that it is on Apple's scale.

    Zero is a real answer and not a failure: nine of MacinTalk 3's voices and
    two of MacinTalk 2's are robots or singers that never vary their pitch.
    """
    base = live(eng).base_inflection()
    assert base is not None, "the engine would not report the voice's depth"
    assert 0.0 <= base <= 100.0, "'pmod' answered %r" % base
    return base


def takes_the_setting(eng, quantised=False, floor=0.0):
    """Move the slider and read the selector back.

    `quantised` for MacinTalk 2, whose 'pmod' has two states and not a scale:
    anything above zero is stored as 100.000. `floor` for MacinTalk Pro, which
    must never be given a depth near zero -- see `macintalkpro.set_inflection`.
    """
    base = sane_base(eng)
    for percent in ENDS:
        eng.set_inflection(percent)
        got = eng.current_inflection()
        want = min(100.0, base * percent / 50.0)
        if base <= 0:
            continue                    # the reference path; not a scaling
        want = max(want, floor)
        if quantised:
            want = 100.0 if want > 0 else 0.0
        assert abs(got - want) <= 0.01, (
            "slider %d on a voice whose own depth is %.3f should hold %.3f, "
            "engine holds %.3f" % (percent, base, want, got))
    eng.set_inflection(50)


def the_midpoint_changes_nothing(eng, pristine=True):
    """50 must be the voice as it was, or every existing user is changed.

    Two claims, and they are not the same one. **The value is right**: the
    channel holds the voice's own depth at the midpoint. **The call is
    harmless**: sending it renders byte-for-byte what never sending it does --
    the question `probe_pitch.py` had to ask of 'pbas', where a perfectly
    correct value still halved the amplitude for a while.

    `pristine=False` for MacinTalk Pro, and the reason is a trap rather than a
    preference. Its module has a second fixture that drives the engine through
    `probe_pro_modules`, which builds its own `osp.Host` -- and `osp_init()`
    re-initialises the emulator underneath the fixture engine while leaving
    `_dead` False, because nothing constructed a second `Engine`. **That is
    the one-engine rule failing in the one way `live()` cannot see.** The
    renders afterwards are self-consistent but no longer match a baseline
    taken before it, so Pro checks the round trip instead: away from the
    midpoint and back must restore exactly.
    """
    live(eng)
    eng.speak(eng.translate("Warming up."))   # the first render differs
    base = eng.base_inflection()
    if pristine:
        untouched = bytes(eng.speak(eng.translate(PHRASE)))
        assert untouched, "no audio at all"
        eng.set_inflection(50)
        assert bytes(eng.speak(eng.translate(PHRASE))) == untouched, (
            "the middle of the slider is not the voice Apple shipped")
    else:
        eng.set_inflection(50)
        middle = bytes(eng.speak(eng.translate(PHRASE)))
        assert middle, "no audio at all"
        eng.set_inflection(0)
        eng.speak(eng.translate(PHRASE))
        eng.set_inflection(100)
        eng.speak(eng.translate(PHRASE))
        eng.set_inflection(50)
        assert bytes(eng.speak(eng.translate(PHRASE))) == middle, (
            "coming back to the middle did not restore the voice")
    assert abs(eng.current_inflection() - base) <= 0.01, (
        "the midpoint holds %r where the voice's own depth is %r"
        % (eng.current_inflection(), base))


def slider_is_audible(eng, quantised=False):
    """The failure this exists for: a control that changes nothing.

    Bytes rather than a pitch measurement, because that is the actual failure
    mode -- three renders that are identical, which is what an ignored
    selector produces and what the pitch slider shipped as for months.

    `quantised` says the engine has only the two states, so the top of the
    slider is *expected* to match the middle. **The bottom still must not**,
    which is the half of the claim that is worth anything on MacinTalk 2 --
    and the assertion is inverted rather than skipped, so a future engine that
    quietly stops being two-state fails here instead of going unnoticed.
    """
    live(eng)
    out = {}
    for percent in (0, 50, 100):
        eng.set_inflection(percent)
        out[percent] = bytes(eng.speak(eng.translate(PHRASE)))
        assert out[percent], "no audio at inflection %d" % percent
    eng.set_inflection(50)
    assert out[0] != out[50], "flattening the voice changed nothing"
    if quantised:
        assert out[100] == out[50], (
            "this engine's 'pmod' was two-state and now is not -- the driver "
            "and its readme both say otherwise")
    else:
        assert out[100] != out[50], "doubling the modulation changed nothing"


def every_voice_survives_the_ends(eng, voices):
    """Speak at both ends of the slider, on every voice.

    **The reason this is per-voice and not per-engine**: MacinTalk Pro hangs
    forever below a modulation depth that belongs to the *voice*. Bruce hangs
    up to 0.05, Agnes to 0.025 and Victoria only at exactly zero, so a floor
    fitted to the first voice tried would have shipped a synthesizer that
    freezes on the second. Nothing here is a substitute for trying all of them.
    """
    for v in voices:
        assert eng.select(v), "the engine refused voice %r" % v.name
        for percent in (0, 100):
            eng.set_inflection(percent)
            pcm = eng.speak(eng.translate(PHRASE))
            assert pcm, ("%s produced nothing at inflection %d"
                         % (v.name, percent))
        eng.set_inflection(50)
