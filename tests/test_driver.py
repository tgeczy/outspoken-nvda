# -*- coding: utf-8 -*-
"""The NVDA-facing driver: queueing, cancelling, and latency.

Every bug in this file's subject reached Tomi before it reached a test, which
is why the tests exist. Each one names the symptom it is guarding against.
"""
import time

import pytest


def _settle(player, target, timeout=8.0):
    t0 = time.perf_counter()
    while player.fed < target and time.perf_counter() - t0 < timeout:
        time.sleep(0.01)
    return player.fed


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
    for c in "abcdefghijkl":
        driver.speak([c])
    assert _settle(driver._player, 12) == 12


def test_cancel_then_speak_survives(driver, rom_files):
    """NVDA's pattern for a keystroke. The speech queued after a cancel must
    always be spoken."""
    for c in "mnopqrst":
        driver.cancel()
        driver.speak([c])
        assert _settle(driver._player, driver._player.fed + 1, timeout=3.0)
        time.sleep(0.02)


def test_latency_is_small(driver, rom_files):
    """Synthesis is 4-30 ms. Anything approaching a tenth of a second here is
    the driver getting in its own way -- blocking on playback, or restarting
    the output stream."""
    worst = 0.0
    for c in "abcdefgh":
        driver.cancel()
        before = driver._player.fed
        t0 = time.perf_counter()
        driver.speak([c])
        while driver._player.fed == before and time.perf_counter() - t0 < 3.0:
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
    _settle(driver._player, 1)
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
