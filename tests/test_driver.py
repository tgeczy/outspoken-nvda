# -*- coding: utf-8 -*-
"""The NVDA-facing driver: queueing, cancelling, and latency.

Every bug in this file's subject reached Tomi before it reached a test, which
is why the tests exist. Each one names the symptom it is guarding against.
"""
import time

import pytest


def _settle(player, target_bytes, timeout=8.0):
    """Wait for `target_bytes` of audio to reach the player.

    Bytes, not feed() calls: audio is delivered in slices so that a blocking
    feed cannot park the worker, which makes the call count a property of the
    chunk size rather than of how much was spoken.
    """
    t0 = time.perf_counter()
    while player.bytes < target_bytes and time.perf_counter() - t0 < timeout:
        time.sleep(0.01)
    return player.bytes


def _warm(driver):
    """Force the driver to build its engine, and hand it back.

    There can only be one. The host DLL is a single CPU with global state, so
    constructing a second Engine resets the first -- which is how an earlier
    version of this file measured its expectations against an engine it had
    just invalidated.
    """
    driver.speak(["warm up"])
    t0 = time.perf_counter()
    while driver._engine is None and time.perf_counter() - t0 < 10.0:
        time.sleep(0.01)
    assert driver._engine is not None, "the engine never started"
    time.sleep(0.6)                      # let the warm-up audio finish
    return driver._engine


def _expected_bytes(eng, texts):
    """How much audio those texts are worth, from the engine that will speak
    them. 16-bit output, so two bytes per 8-bit sample."""
    return sum(len(eng.speak(eng.translate(t))) for t in texts) * 2


def test_check_requires_the_engine():
    import outspoken
    import rom
    assert outspoken.SynthDriver.check() == rom.usable()


def test_two_voices(driver):
    voices = driver._get_availableVoices()
    assert list(voices) == ["male", "female"]


def test_queued_speech_is_not_dropped(driver, rom_files):
    """cancel() used to drain the queue, which raced the speak() that follows
    almost every cancel: the new item was thrown away and the key was silent.
    Heard as "typing fast does not speak every letter"."""
    letters = list("abcdefghijkl")
    eng = _warm(driver)
    want = _expected_bytes(eng, letters)
    driver._player.bytes = 0
    for c in letters:
        driver.speak([c])
    got = _settle(driver._player, want, timeout=25.0)
    assert got >= want * 0.98, "%d of %d bytes of audio" % (got, want)


def test_cancel_then_speak_survives(driver, rom_files):
    """NVDA's pattern for a keystroke. The speech queued after a cancel must
    always be spoken."""
    for c in "mnopqrst":
        driver.cancel()
        driver.speak([c])
        assert _settle(driver._player, driver._player.bytes + 1, timeout=3.0)
        time.sleep(0.02)


def test_latency_is_small(driver, rom_files):
    """Synthesis is 4-30 ms. Anything approaching a tenth of a second here is
    the driver getting in its own way -- blocking on playback, or restarting
    the output stream."""
    worst = 0.0
    for c in "abcdefgh":
        driver.cancel()
        before = driver._player.bytes
        t0 = time.perf_counter()
        driver.speak([c])
        while driver._player.bytes == before and time.perf_counter() - t0 < 3.0:
            time.sleep(0.001)
        worst = max(worst, time.perf_counter() - t0)
        time.sleep(0.05)
    assert worst < 0.30, "worst latency %.0f ms" % (worst * 1000)


def test_cancel_does_not_restart_the_stream_for_nothing(driver, rom_files):
    """NVDA calls cancel() before nearly every speak(), including when nothing
    is sounding. Each stop() tears the output stream down and the next feed()
    pays to start it again -- which a long utterance absorbs and a short one
    does not. That is why typing echo lagged while sentences were fine."""
    driver.speak(["a sentence long enough to still be playing"])
    _settle(driver._player, 1, timeout=5.0)
    before = driver._player.stops
    for _ in range(10):
        driver.cancel()                  # nothing is queued after the first
        time.sleep(0.01)
    assert driver._player.stops - before <= 1, \
        "%d stops for 10 cancels" % (driver._player.stops - before)


def test_terminate_is_clean(rom_files):
    import outspoken
    d = outspoken.SynthDriver()
    d.speak(["hello"])
    time.sleep(0.2)
    d.terminate()
    time.sleep(0.2)
    assert not d._worker.is_alive()


def test_index_commands_are_reported(driver, rom_files):
    import speech.commands
    import synthDriverHandler
    before = synthDriverHandler.synthIndexReached.count
    driver.speak([speech.commands.IndexCommand(7), "hello"])
    t0 = time.perf_counter()
    while (synthDriverHandler.synthIndexReached.count == before
           and time.perf_counter() - t0 < 3.0):
        time.sleep(0.01)
    assert synthDriverHandler.synthIndexReached.count > before


def test_never_goes_permanently_silent(driver, rom_files):
    """The worst failure this driver has had: it stopped speaking and stayed
    that way.

    A generation counter stamped items when queued and compared them when
    rendered, so a cancel arriving in that window made an item stale. In real
    use it reached a state where every item was stale and never recovered --
    615 utterances spoken, then 194 discarded unheard, silence until NVDA was
    restarted.

    Thirty cancel-then-speak cycles, which is a few seconds of ordinary typing.
    Audio must still be arriving at the end, not just at the start.
    """
    _warm(driver)
    first_half = last_half = 0
    for i, c in enumerate("abcdefghijklmnopqrstuvwxyzabcd"):
        driver.cancel()
        before = driver._player.bytes
        driver.speak([c])
        got = _settle(driver._player, before + 1, timeout=3.0) - before
        if i < 15:
            first_half += got
        else:
            last_half += got
        time.sleep(0.02)
    assert last_half > 0, "went silent partway through and stayed silent"
    assert last_half > first_half * 0.5, \
        "audio dwindled: %d bytes early, %d late" % (first_half, last_half)


def test_cancel_leaves_the_driver_usable(driver, rom_files):
    """Cancelling with nothing queued, repeatedly, must not wedge anything."""
    _warm(driver)
    for _ in range(20):
        driver.cancel()
    before = driver._player.bytes
    driver.speak(["still here"])
    assert _settle(driver._player, before + 1, timeout=5.0) > before


def test_typing_like_nvda_does(driver, rom_files):
    """Simulate NVDA's actual pattern for typed characters.

    For every keystroke NVDA cancels, speaks one character, and -- because the
    sequence ends with EndUtteranceCommand -- does not send the next until
    synthDoneSpeaking arrives. Nothing else in this file models that pacing,
    which is why several latency bugs reached Tomi before they reached a test.

    Asserts what a user would notice: every keystroke produces sound, and none
    of them takes long enough to feel like a delay.
    """
    import synthDriverHandler
    done = synthDriverHandler.synthDoneSpeaking
    _warm(driver)
    latencies, spoke = [], 0
    for c in "abcdefghijklmnopqrst":
        done.arm()
        driver.cancel()
        before = driver._player.bytes
        t0 = time.perf_counter()
        driver.speak([c])
        got = _settle(driver._player, before + 1, timeout=3.0)
        if got > before:
            spoke += 1
            latencies.append(time.perf_counter() - t0)
        done.wait(timeout=3.0)           # NVDA waits here before the next key
    assert spoke == 20, "only %d of 20 keystrokes produced audio" % spoke
    latencies.sort()
    median = latencies[len(latencies) // 2]
    assert median < 0.15, "median keystroke latency %.0f ms" % (median * 1000)
    assert latencies[-1] < 0.60, "worst keystroke latency %.0f ms" % (
        latencies[-1] * 1000)
