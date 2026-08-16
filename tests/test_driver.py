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
    assert outspoken.SynthDriver.check() == bool(outspoken._catalogue())


def test_the_1984_voice_ids_never_change(driver):
    """NVDA persists the voice id, so renaming these resets every existing
    user's voice on upgrade. Labels may change freely; these two may not."""
    voices = driver._get_availableVoices()
    assert list(voices)[:2] == ["male", "female"]
    assert all(v.startswith("mtk2:") for v in list(voices)[2:])


def test_every_listed_voice_can_be_selected(driver):
    """A voice in the list that cannot be chosen is worse than one absent.

    Goes through _set_voice/_get_voice rather than the `voice` property: the
    fake SynthDriver here is a plain object, so `driver.voice = x` would just
    set an attribute and read it straight back, and the test would pass
    without touching the driver at all.
    """
    for vid in driver._get_availableVoices():
        driver._set_voice(vid)
        assert driver._get_voice() == vid


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


def test_cancel_stops_even_when_nothing_is_queued(driver, rom_files):
    """Cancelling repeatedly with nothing to say must be harmless.

    This test used to assert the opposite -- that cancel avoided touching the
    player when the driver thought no audio was outstanding -- as a way of
    dodging output-stream restarts. That was a hypothesis, and honouring it
    broke interruption outright, because "no audio outstanding" was decided by
    whether the worker was busy rather than by whether sound was playing.
    Restarts are a performance question; interrupting is a correctness one.
    """
    _warm(driver)
    for _ in range(10):
        driver.cancel()
        time.sleep(0.01)
    before = driver._player.bytes
    driver.speak(["still here"])
    assert _settle(driver._player, before + 1, timeout=5.0) > before


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


def test_cancel_always_stops_the_player(driver, rom_files):
    """Interrupting is the whole job of cancel().

    This was once gated on a flag meant to avoid needless stream restarts, but
    the flag tracked the WORKER being busy rather than the player having audio.
    The worker goes idle 50 ms after the last item while sound is still
    playing, so a keystroke arriving later found the gate shut and cancelled
    nothing: speech continued after Tab was released, and "copy" queued behind
    "178777 characters selected" instead of cutting it off.
    """
    _warm(driver)
    driver.speak(["a sentence long enough that it is certainly still playing"])
    _settle(driver._player, 1, timeout=5.0)
    before = driver._player.stops
    driver.cancel()
    assert driver._player.stops == before + 1, "cancel did not stop the player"

    # And again once the worker has gone idle, which is the case that broke.
    driver.speak(["another long sentence, spoken while we wait a while"])
    _settle(driver._player, 1, timeout=5.0)
    time.sleep(0.4)                      # worker idles; audio still playing
    before = driver._player.stops
    driver.cancel()
    assert driver._player.stops == before + 1, \
        "cancel did nothing once the worker had gone idle"


# -- issue #1: only the worker may touch the emulator -----------------------
#
# MacinTalk 2 buzzed and popped after a voice switch, and stayed that way for
# every later utterance until the synthesizer was reloaded. The cause was a
# component call -- SetSpeechInfo('cvox') -- issued from `_set_voice` on NVDA's
# main thread while the worker was inside `speak()`. Two threads stepping one
# emulated 68000 corrupts it, so what breaks is the CPU rather than the
# utterance, which is why it never recovered.
#
# These use a stub engine rather than the real one: what is being asserted is
# *which thread* makes each call, and that is a property of the driver alone.


class _StubVoice(object):
    def __init__(self, name, vid):
        self.name, self.id, self.creator = name, vid, "mtk2"


class _StubEngine(object):
    """Records the thread every call arrives on, and nothing else."""

    number_mode = "words"

    def __init__(self, files=None, allvoices=None, voice=None):
        import threading
        self._threading = threading
        self.voice = voice
        self.calls = []                  # [(name, thread ident)]
        self.closed = False

    def _note(self, what):
        self.calls.append((what, self._threading.get_ident()))

    def select(self, voice):
        self._note("select")
        self.voice = voice
        return True

    def set_rate(self, rate):
        self._note("set_rate")

    def set_voice(self, hz):
        self._note("set_voice")

    def translate(self, text):
        return text

    def speak(self, text):
        self._note("speak")
        time.sleep(0.05)                 # long enough to be raced
        return b"\x80\x90" * 400

    def stop(self):
        self._note("stop")               # allowed from the main thread

    def close(self):
        self._note("close")
        self.closed = True


@pytest.fixture
def stubbed(monkeypatch):
    """A driver whose MacinTalk 2 engine is a stub, with two voices."""
    import sys
    import types
    import outspoken

    a, b = _StubVoice("Alpha", 1), _StubVoice("Beta", 2)
    built = []

    def _engine(files, allvoices, voice=None):
        eng = _StubEngine(files, allvoices, voice)
        built.append(eng)
        return eng

    fake = types.ModuleType("macintalk2")
    fake.Engine = _engine
    fake.find = lambda roots: ({}, [a, b])
    fake.usable = lambda roots: True
    monkeypatch.setitem(sys.modules, "macintalk2", fake)

    d = outspoken.SynthDriver()
    # Set the catalogue directly. `_catalogue()` caches into this, so the
    # driver never scans the ROM folder and the test does not need one.
    d._voiceCatalogue = [("mtk2:Alpha", "Alpha (MacinTalk 2)", "mtk2", a),
                         ("mtk2:Beta", "Beta (MacinTalk 2)", "mtk2", b)]
    d._voiceId = "mtk2:Alpha"
    try:
        yield d, built, a, b
    finally:
        d.terminate()


def _spoken(driver, text, timeout=5.0):
    """Speak one thing and wait for it to reach the player."""
    before = driver._player.bytes
    driver.speak([text])
    t0 = time.perf_counter()
    while (driver._player.bytes <= before
           and time.perf_counter() - t0 < timeout):
        time.sleep(0.005)
    return driver._player.bytes > before


def test_settings_never_drive_the_engine_from_the_main_thread(stubbed):
    """Issue #1. Every engine call must land on the worker.

    `stop()` is the one exception and is deliberately safe: `.sp` writes a
    single byte into emulated memory and MacinTalk 2 does nothing at all.
    """
    import threading
    driver, built, _a, _b = stubbed
    main = threading.get_ident()

    assert _spoken(driver, "first"), "the stub engine never spoke"
    # Switch voice and change the rate while a render is in flight, which is
    # the window the bug needed.
    driver.speak(["something long enough to still be rendering"])
    driver._set_voice("mtk2:Beta")
    driver._set_rate(80)
    driver._set_pitch(70)
    driver.cancel()
    assert _spoken(driver, "second")

    assert built, "no engine was ever built"
    offenders = [(what, eng) for eng in built for what, tid in eng.calls
                 if tid == main and what != "stop"]
    assert not offenders, \
        "these ran on NVDA's main thread: %s" % [w for w, _ in offenders]


def test_the_engine_is_closed_by_the_worker(stubbed):
    """`close()` is a component call *and* a DLL unload.

    Doing it from `terminate()` on the main thread could unmap the host
    library while the worker was still executing inside it -- the same race as
    the voice switch, with a much worse failure than buzzing.
    """
    import threading
    driver, built, _a, _b = stubbed
    main = threading.get_ident()
    assert _spoken(driver, "hello")
    driver.terminate()
    assert built[0].closed, "the engine was never closed"
    closes = [tid for what, tid in built[0].calls if what == "close"]
    assert closes and main not in closes, \
        "close() ran on NVDA's main thread"


def test_a_voice_change_survives_the_cancel_that_follows_it(stubbed):
    """The reason the change is reconciled and not queued.

    NVDA's own flow for a settings change is: apply it, cancel, then speak the
    confirmation. A voice change queued as an item on the speech queue is
    thrown away by that cancel -- `cancel()` drains the queue -- so the
    confirmation is spoken in the OLD voice and nothing ever converges, since
    only `_set_voice` would have applied it.
    """
    driver, built, _a, b = stubbed
    assert _spoken(driver, "in the first voice")
    driver._set_voice("mtk2:Beta")
    driver.cancel()                      # exactly what NVDA does next
    assert _spoken(driver, "confirmation")
    assert built[0].voice is b, \
        "the confirmation was spoken in the old voice"


def test_a_voice_change_needs_no_utterance_of_its_own(stubbed):
    """Switching voice must not speak, and must not cost a rebuild.

    All ten MacinTalk 2 voices are registered at once, so a switch is one
    SetSpeechInfo('cvox'). Rebuilding would reset the emulator's globals and
    take the best part of a second.
    """
    driver, built, _a, _b = stubbed
    assert _spoken(driver, "one")
    fed = driver._player.fed
    driver._set_voice("mtk2:Beta")
    time.sleep(0.2)
    assert driver._player.fed == fed, "changing voice produced audio"
    assert _spoken(driver, "two")
    assert len(built) == 1, "the engine was rebuilt to change voice"
    assert [w for w, _ in built[0].calls].count("select") == 1
