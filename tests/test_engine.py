# -*- coding: utf-8 -*-
"""The emulator wrapper.

These need the engine, so they skip when `rom/` is empty.
"""
import pytest


@pytest.fixture(scope="module")
def eng(rom_files):
    import engine
    return engine.Engine(rom_files)


def test_starts_and_speaks(eng):
    pcm = eng.speak(eng.translate("hello"))
    assert len(pcm) > 1000
    assert max(pcm) > 0x90 and min(pcm) < 0x70      # real waveform, not silence


def test_short_utterances_are_deterministic(eng):
    """The parser reads past the text it is given.

    "IY4" alone rendered nothing and returned after 408 instructions, and when
    it did not bail it read whatever the previous, longer utterance had left in
    the buffer -- heard as a letter that would not speak, and as fragments of
    another word bleeding onto the end of a short one.
    """
    first = eng.speak(eng.translate("e"))
    assert len(first) > 1000, "the letter E must render"
    for noise in ("echotalk", "select", "a very much longer utterance indeed"):
        eng.speak(eng.translate(noise))
        again = eng.speak(eng.translate("e"))
        assert again == first, "E changed after speaking %r" % noise


def test_every_letter_renders(eng):
    import string
    for c in string.ascii_lowercase:
        pcm = eng.speak(eng.translate(c))
        assert len(pcm) > 500, "letter %r produced %d samples" % (c, len(pcm))


def test_empty_input_is_silent_not_an_error(eng):
    assert eng.speak("") == b""
    assert eng.speak("   ") == b""


def test_stop_before_speak_does_not_kill_the_new_utterance(eng):
    """`speak()` clears the flag on entry, so a stop that arrives while the
    engine is idle affects nothing.

    That is the behaviour NVDA needs: it calls cancel() immediately before
    almost every speak(), and the speech that follows must survive. Asserting
    the opposite is how this test was first written, and it was wrong.
    """
    full = eng.speak(eng.translate("the quick brown fox jumps over the lazy dog"))
    eng.stop()
    assert eng.speak(eng.translate("the quick brown fox jumps over the lazy dog")) == full


def test_stop_halts_an_utterance_in_flight_and_recovers(eng):
    """The real cancel path: one byte, written from another thread while the
    frame loop is running. A synthesizer that cannot be interrupted twice is no
    use in a screen reader, so it must still speak fully afterwards."""
    import threading
    import time
    text = eng.translate("the quick brown fox jumps over the lazy dog. " * 4)
    full = eng.speak(text)
    out = {}
    t = threading.Thread(target=lambda: out.setdefault("pcm", eng.speak(text)))
    t.start()
    time.sleep(0.004)                    # let the frame loop get going
    eng.stop()
    t.join(20)
    assert not t.is_alive()
    assert len(out["pcm"]) < len(full), "the stop flag did not interrupt"
    assert eng.speak(text) == full, "the engine did not recover"


def test_pitch_changes_the_fundamental(eng):
    """110 Hz and 250 Hz are the two voices, and they must actually differ."""
    def f0(pcm, sr=22254.5454):
        d = [b - 128 for b in pcm]
        best = max(range(0, max(1, len(d) - 2048), 512),
                   key=lambda i: sum(abs(x) for x in d[i:i + 2048]))
        s = d[best:best + 2048]
        lo, hi = int(sr / 400), int(sr / 60)
        return sr / max((sum(s[k] * s[k + lag]
                             for k in range(0, len(s) - lag, 2)), lag)
                        for lag in range(lo, hi))[1]
    eng.set_voice(110)
    male = f0(eng.speak(eng.translate("the lazy dog")))
    eng.set_voice(250)
    female = f0(eng.speak(eng.translate("the lazy dog")))
    eng.set_voice(110)
    assert female > male * 1.5, "male %.0f Hz, female %.0f Hz" % (male, female)


def test_synthesis_is_fast(eng):
    """4-30 ms measured. If this ever creeps into the hundreds, latency in the
    driver is the engine's fault after all -- and until then it is not."""
    import time
    t0 = time.perf_counter()
    for _ in range(5):
        eng.speak(eng.translate("notification chevron button"))
    per = (time.perf_counter() - t0) / 5 * 1000
    assert per < 250, "%.0f ms per utterance" % per


def test_no_audio_bleeds_in_from_the_previous_utterance(eng):
    """The engine reuses two sound buffers for every utterance and hands over
    a whole buffer's declared length, so anything the new utterance has not
    reached yet is still the previous one's speech.

    Measured before the fix: "space" was 9,761 samples alone and 14,982 after
    "a" -- and the 5,221 difference is almost exactly the 5,001 samples "a"
    takes. Heard as one utterance running into the next, and as latency,
    because the first thing you hear is what you asked for last time.
    """
    alone = eng.speak(eng.translate("space"))
    for before in ("a", "hello there", "the quick brown fox jumps over", "z"):
        eng.speak(eng.translate(before))
        after = eng.speak(eng.translate("space"))
        assert after == alone, \
            "'space' changed after %r: %d vs %d samples" % (
                before, len(after), len(alone))
