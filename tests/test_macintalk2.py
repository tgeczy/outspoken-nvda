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
