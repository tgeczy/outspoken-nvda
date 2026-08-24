# -*- coding: utf-8 -*-
"""The end of every utterance was being spoken one utterance late.

Reported by Tyler, with the exact recipe: the original outSPOKEN voice,
rate 65, read the number 4 -- it cuts off at the end.

The driver fills a sound buffer, hands it over with `bufferCmd` when it is
full, and switches to the other one. At the end of speech it is normally
part-way through a buffer, and it neither shortens that buffer nor hands it
over: it leaves `globals[$0C]` pointing at where it stopped, and the *next*
utterance's `SetupA3` reads that stale pointer and applies it to the new
utterance's first buffer.

Which is why every utterance after the first begins with a short buffer whose
length is exactly the previous utterance's missing tail. The buffers are wiped
before each utterance, so what arrived at the front of the next one was silence
of precisely the right length -- the fault was paying for itself in dead air.

Whether it cost anything audible depended on where the word happened to end
relative to a 3870-sample boundary, which is why it took a particular voice at
a particular rate saying a particular number for anyone to hear it. Above about
686 wpm the whole word fits in one buffer, nothing is ever handed over, and the
top of the rate slider was completely silent.
"""
import pytest

import engine as eng_mod


#: Rate slider positions, and what the driver maps them to.
FAST = 65                       # Tyler's, 348 wpm
TOP = 100                       # 900 wpm, which used to be silent


def _cliff(pcm):
    """Peak deviation in the samples just before `_tidy`'s appended silence.

    A natural ending has decayed towards 0x80; a cut stops at whatever the
    waveform happened to be doing. Before the fix this was 83 at rate 65.
    """
    tail = eng_mod._TAIL
    if len(pcm) <= tail + 200:
        return 0
    return max((abs(b - 128) for b in pcm[-tail - 200:-tail]), default=0)


@pytest.fixture
def sp(driver, rom_files):
    """The 1984 engine, on the original voice."""
    driver._set_voice("male")
    return driver, driver._ensureEngine()


def _say(driver, eng, text, rate):
    driver._set_rate(rate)
    driver._applySettings(eng)
    return eng.speak(eng.translate(text))


def test_the_number_four_is_not_cut_off(sp):
    """Tyler's report, as a test."""
    driver, eng = sp
    pcm = _say(driver, eng, "4", FAST)
    assert pcm, "nothing rendered"
    assert _cliff(pcm) < 40, (
        "the utterance still ends at amplitude %d, which is a cut rather "
        "than a decay" % _cliff(pcm))


@pytest.mark.parametrize("rate", [40, 50, 60, 64, 65, 70, 80, 90, 100])
@pytest.mark.parametrize("text", ["4", "3", "hello there"])
def test_no_rate_ends_an_utterance_mid_waveform(sp, rate, text):
    """The general property, since "4 at 65" was only where it was noticed.

    Every rate and both shapes of text: an utterance ends by decaying, not by
    stopping. Nine rates times three texts, because the fault depends on where
    the audio lands relative to a buffer boundary and any single case is luck.
    """
    driver, eng = sp
    pcm = _say(driver, eng, text, rate)
    assert pcm, "%r rendered nothing at rate %d" % (text, rate)
    assert _cliff(pcm) < 40, (
        "%r at rate %d ends at amplitude %d" % (text, rate, _cliff(pcm)))


@pytest.mark.parametrize("rate", [90, 100])
def test_the_top_of_the_rate_range_is_not_silent(sp, rate):
    """**The same bug, all of it rather than the end of it.**

    Above ~686 wpm a short word fits inside a single buffer, which was never
    handed over, so the fastest sixth of the slider produced nothing at all.
    """
    driver, eng = sp
    pcm = _say(driver, eng, "4", rate)
    assert pcm and len(pcm) > eng_mod._LEAD + eng_mod._TAIL + 1000, (
        "rate %d produced %d samples, which is padding and no speech"
        % (rate, len(pcm) if pcm else 0))
    assert max(abs(b - 128) for b in pcm) > 20, "rate %d is silent" % rate


def test_a_faster_rate_is_a_shorter_utterance(sp):
    """It was not monotonic before, and that was the visible symptom.

    Duration bottomed out around rate 64 and then *grew*, because what was
    being measured was where a buffer boundary fell rather than how fast the
    engine was speaking.
    """
    driver, eng = sp
    lengths = [len(_say(driver, eng, "hello there", r))
               for r in (40, 50, 60, 70, 80, 90, 100)]
    assert lengths == sorted(lengths, reverse=True), lengths


def test_nothing_is_appended_where_nothing_was_missing(sp):
    """The harvest must be exact, not generous.

    At slower rates the word finishes inside a buffer that is handed over
    anyway, and the leftover is the engine's own quiet hiss. Taking it by
    scanning for where the silence starts would add up to 175 ms of that to
    every utterance -- which is the padding `_tidy` exists to remove. The
    length comes from the stop pointer instead, so these lengths are the ones
    this test recorded before the fix existed.
    """
    driver, eng = sp
    assert len(_say(driver, eng, "4", 40)) == 9471
    assert len(_say(driver, eng, "4", 50)) == 7740
    assert len(_say(driver, eng, "4", 60)) == 6500


def test_the_harvested_tail_is_the_continuation(sp):
    """A wrong offset would join two unrelated samples.

    Compared against the typical sample-to-sample step inside the same
    waveform: the join has to be no more of a jump than the audio already
    makes on its own.
    """
    driver, eng = sp
    h = eng.h
    _say(driver, eng, "4", FAST)
    captured = bytes(h.pcm)
    tail = eng._last_buffer()
    assert tail, "nothing was held back at rate %d, so this proves nothing" % FAST
    steps = sorted(abs(captured[i + 1] - captured[i])
                   for i in range(len(captured) - 400, len(captured) - 1))
    median = steps[len(steps) // 2]
    assert abs(tail[0] - captured[-1]) <= 6 * max(1, median), (
        "the join steps by %d where the waveform's median step is %d"
        % (abs(tail[0] - captured[-1]), median))
