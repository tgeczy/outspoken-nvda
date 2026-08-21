# -*- coding: utf-8 -*-
"""MacinTalk 2, and the one thing that made it unusable.

Needs the engine and at least two voices, so it skips when `rom/` is empty.
"""
import pytest

#: The driver's midpoint. Rate matters here: it scales how much audio a phrase
#: is worth, and these tests compare lengths.
RATE = 232
PHRASE = "Voice testing, one two three."


@pytest.fixture(scope="module")
def mt2():
    import paths
    import macintalk2
    files, voices = macintalk2.find(paths.roots())
    if len(voices) < 2:
        pytest.skip("need two MacinTalk 2 voices; run tools/extract_rom.py")
    eng = macintalk2.Engine(files, voices, voices[0])
    eng.set_rate(RATE)
    yield eng, voices
    eng.close()


def _say(eng):
    return len(eng.speak(eng.translate(PHRASE)))


def _switch(eng, voice):
    assert eng.select(voice), "the engine refused voice %r" % voice.name
    eng.set_rate(RATE)                   # the driver re-applies it too


def test_returning_to_a_voice_does_not_wreck_it(mt2):
    """Issue #1, and it was never the threading.

    Switching MacinTalk 2 voices left NVDA buzzing, and the log gave the shape
    of it: the utterance after a switch rendered 33 to 37 seconds of audio for
    a phrase worth one and a half, often hitting `Engine.MAX_BUFFERS`. That
    blob of noise is what was heard.

    Eight *different* voices in a row were always fine. It took going BACK to
    one, because loading a voice rewrites 17.9% of its `ttvi` in place, and the
    host used to hand back the same block forever -- so the second load patched
    the first load's output. `_DetachResource` now restores the original, which
    is what a real Resource Manager does by re-reading the file.

    Ratios rather than absolutes: how much audio a phrase is worth is the
    engine's business and changes with the voice, but a voice must sound the
    same length the second time it is chosen as the first.
    """
    eng, voices = mt2
    a, b = voices[0], voices[1]

    first = _say(eng)
    assert first > 1000, "the first utterance produced almost nothing"

    _switch(eng, b)
    other = _say(eng)
    assert other > 1000

    _switch(eng, a)                      # the revisit: this is what broke
    again = _say(eng)
    assert again == pytest.approx(first, rel=0.5), (
        "revisiting %s rendered %.2f s where it first rendered %.2f s"
        % (a.name, again / 22254.0, first / 22254.0))

    _switch(eng, b)                      # and back again, repeatedly
    assert _say(eng) == pytest.approx(other, rel=0.5)


def test_every_voice_survives_being_chosen_twice(mt2):
    """The whole cast, not just the two the previous test uses.

    Each voice patches its own `ttvi`, so each has its own copy of the bug --
    one voice recovering says nothing about the other nine.
    """
    eng, voices = mt2
    firstTime = {}
    for v in voices:
        _switch(eng, v)
        firstTime[v.name] = _say(eng)
    bad = []
    for v in voices:
        _switch(eng, v)
        n = _say(eng)
        if n > firstTime[v.name] * 1.5:
            bad.append("%s %.2f s -> %.2f s"
                       % (v.name, firstTime[v.name] / 22254.0, n / 22254.0))
    assert not bad, "second visit rendered far more audio: " + "; ".join(bad)


# -- pitch ------------------------------------------------------------------
#
# 'pbas' was wired years ago and then deliberately switched off, because the
# driver was handing it hertz and it is a musical scale: twelve units to the
# octave. Measured here, `tools/probe_pitch.py`: Ben answers 60, -24 gives
# 0.253 of the base frequency against a predicted 0.250, -6 gives 0.722
# against 0.707, +6 gives 1.391 against 1.414.
#
# Shared with MacinTalk Pro's module rather than with a single pitch module,
# because two engines cannot be alive at once. See tests/pitchcheck.py.
import pitchcheck                                              # noqa: E402


def test_it_reports_a_musical_pitch(mt2):
    pitchcheck.sane_base(mt2[0])


def test_it_takes_a_pitch_offset(mt2):
    pitchcheck.takes_the_offset(mt2[0])


def test_the_pitch_slider_is_audible(mt2):
    pitchcheck.slider_is_audible(mt2[0])


def test_the_pitch_offset_does_not_compound_across_voices(mt2):
    """The near miss this design has to keep avoiding.

    `set_pitch` works from the voice's own 'pbas', which it asks the engine
    for once and remembers. If that question were ever asked *after* an offset
    had been applied, the answer would be our own offset read back as though
    it were the voice's -- and the pitch would climb another octave every time
    the user changed voice.

    It is safe because taking a voice resets the channel's pitch to that
    voice's own: Ben reads 60, the top of the slider makes it 72, selecting
    Votron makes it 38, and coming back to Ben makes it 60 again. So the cache
    is dropped exactly when the channel is pristine. That is a property of the
    engine rather than of our code, so it is measured rather than assumed.
    """
    eng, voices = mt2
    pitchcheck.live(eng)
    if len(voices) < 2:
        pytest.skip("need two MacinTalk 2 voices")
    a, b = voices[0], voices[1]

    assert eng.select(a)
    natural = eng.current_pitch()
    for _round in range(3):
        eng.set_pitch(120)
        assert eng.select(b)
        eng.set_pitch(120)
        assert eng.select(a)
        eng.set_pitch(120)
        assert abs(eng.current_pitch() - (natural + 12.0)) < 0.01, (
            "an octave above %.1f drifted to %.3f"
            % (natural, eng.current_pitch()))
    eng.set_pitch(0)


# -- inflection ------------------------------------------------------------
#
# **This engine's 'pmod' has two states and not a scale.** Anything above zero
# is stored as 100.000: 6.25, 12.5, 25 and 50 all read back as 100 and render
# to the same bytes. So `quantised=True` below is the engine's answer and not
# a tolerance -- see `macintalk2.set_inflection`.
import inflectioncheck                                         # noqa: E402


def test_it_reports_the_voices_own_modulation(mt2):
    inflectioncheck.sane_base(mt2[0])


def test_it_takes_the_inflection_setting(mt2):
    inflectioncheck.takes_the_setting(mt2[0], quantised=True)


def test_the_inflection_slider_is_audible(mt2):
    inflectioncheck.slider_is_audible(mt2[0], quantised=True)


def test_the_middle_of_the_inflection_slider_changes_nothing(mt2):
    inflectioncheck.the_midpoint_changes_nothing(mt2[0])


def test_every_voice_speaks_at_both_ends_of_the_inflection_slider(mt2):
    eng, voices = mt2
    inflectioncheck.every_voice_survives_the_ends(eng, voices)
