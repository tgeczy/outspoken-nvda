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


def test_it_is_always_offered_and_refuses_rather_than_going_silent(monkeypatch):
    """Selectable, and then it refuses -- which is not the same as silent.

    The synthesizer is always offered now, so that choosing it produces an
    explanation instead of an absence nobody could account for: NVDA catches a
    driver that will not load, falls back, and speech never stops. What must
    never happen is the other failure -- loading successfully and then saying
    nothing -- so `__init__` has to raise, and it has to tell the user first.
    """
    import outspoken
    assert outspoken.SynthDriver.check() is True

    monkeypatch.setattr(outspoken, "_whyNot", lambda: ["no engine, for a test"])
    told = []
    monkeypatch.setattr(outspoken, "_explainLater", told.append)
    with pytest.raises(Exception):
        outspoken.SynthDriver()
    assert told, "it refused to load and told the user nothing"


def test_the_1984_voice_ids_never_change(driver):
    """NVDA persists the voice id, so renaming these resets every existing
    user's voice on upgrade. Labels may change freely; these two may not."""
    voices = driver._get_availableVoices()
    assert list(voices)[:2] == ["male", "female"]
    # Everything since is prefixed by its engine. gala: is MacinTalk Pro,
    # which reaches this list only on a machine that has it extracted.
    assert all(v.startswith(("mtk2:", "gala:")) for v in list(voices)[2:])


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
        #: What actually reached the engine, so a test can ask what was said
        #: and at what pitch rather than only which thread asked.
        self.spoken = []
        self.pitches = []
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
        self._note("set_voice")          # the 1984 engine's absolute hertz

    def set_pitch(self, tenths):
        self._note("set_pitch")          # what the Speech Manager engines take
        self.pitches.append(tenths)

    def translate(self, text):
        return text

    def speak(self, text):
        self._note("speak")
        self.spoken.append(text)
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


def test_every_wx_name_we_use_actually_exists_in_wxpython():
    """A misspelt wx constant is invisible until a user sends in a log.

    `YES_NO_CANCEL` is real in wxWidgets' C++ API and absent from wxPython.
    The start-up dialog asked for it, so `_ask` raised AttributeError every
    time it ran, and that dialog had never once appeared in any release of
    either add-on. It presented as nothing happening -- which is also what a
    missing add-on, a suppressed reminder and a mistimed thread all look like,
    so it was blamed on each of those in turn before a user's log named it.

    wxPython is not installed here and should not have to be: this reads the
    source and checks every `wx.NAME` against the ones wxPython really has.
    Add to the set when a genuinely new one is needed -- deliberately, which is
    the whole point of it being a list.
    """
    import os
    import re

    known = {
        # message box styles and answers
        "OK", "CANCEL", "YES", "NO", "YES_NO", "OK_DEFAULT", "NO_DEFAULT",
        "ICON_INFORMATION", "ICON_WARNING", "ICON_ERROR", "ICON_QUESTION",
        "CENTRE", "CENTER",
        # scheduling
        "CallAfter", "CallLater",
        # menus, if a future version grows one
        "ID_ANY", "EVT_MENU", "Menu", "MenuItem",
    }

    here = os.path.dirname(os.path.abspath(__file__))
    root = os.path.join(os.path.dirname(here), "addon")
    assert os.path.isdir(root), root

    used = {}
    scanned = 0
    for dirpath, _dirs, names in os.walk(root):
        for n in names:
            if not n.endswith(".py"):
                continue
            scanned += 1
            with open(os.path.join(dirpath, n), encoding="utf-8") as f:
                for name in re.findall(r"\bwx\.([A-Za-z_][A-Za-z0-9_]*)",
                                       f.read()):
                    used.setdefault(name, n)

    # The first version of this test matched nothing at all and passed
    # vacuously, which is worse than not having it: an escaping mistake had
    # turned the \b in the pattern into a literal backspace byte. So prove it
    # is looking at something before trusting what it says.
    assert scanned, "scanned no Python files under %s" % root
    assert used, "found no wx names at all -- the pattern is broken"

    unknown = {k: v for k, v in used.items() if k not in known}
    assert not unknown, (
        "these wx names are not in the allowed set. Check they exist in "
        "wxPython -- YES_NO_CANCEL does not -- then add them here: %r"
        % unknown)


# --------------------------------------------------------- MacinTalk Pro ---
#
# The driver tests above run against whatever is staged into the fake config
# directory, which is MacinTalk 1 and 2. Pro is large -- the engine plus one
# voice is well over a megabyte -- so rather than copy it, these point the
# add-on's own search at the repository's `rom/` folder. That still exercises
# the real path: `_catalogue`, `_sync`, the rebuild on a voice change, and the
# worker thread.


@pytest.fixture
def pro_driver(monkeypatch):
    """A driver that can see MacinTalk Pro, or a skip."""
    import paths
    import rom
    import outspoken
    import macintalkpro
    monkeypatch.setattr(rom, "search_roots", lambda: paths.roots())
    if not macintalkpro.usable(paths.roots()):
        pytest.skip("MacinTalk Pro is not extracted; run tools/extract_rom.py")
    d = outspoken.SynthDriver()
    yield d
    d.terminate()


def _pro_voices(driver):
    return [v for v in driver._get_availableVoices() if v.startswith("gala:")]


def test_pro_voices_reach_nvda_and_all_of_them_speak(pro_driver):
    """Every Pro voice listed has to make a sound.

    They were deliberately kept out of this list until 2026-08-20, when the
    asynchronous lexicon read and `_FixRatio` were finally served and Tomi
    heard all three. A voice in the list that says nothing is the failure this
    project treats as worse than not listing it."""
    voices = _pro_voices(pro_driver)
    assert voices, "MacinTalk Pro is present but no gala: voice is listed"
    for vid in voices:
        pro_driver._set_voice(vid)
        assert _spoken(pro_driver, "Testing one two three.", timeout=20.0), \
            "%s was listed and produced no audio" % vid


def test_pro_survives_being_interrupted(pro_driver):
    """A screen reader cancels far more often than it finishes.

    Interruption is where every audio fault in this project has lived, and
    Pro's `stop()` deliberately never touches the emulator: cancel() runs on
    NVDA's main thread while the worker may be mid-render, and two threads
    stepping one emulated CPU corrupts it."""
    voices = _pro_voices(pro_driver)
    if not voices:
        pytest.skip("no MacinTalk Pro voice")
    pro_driver._set_voice(voices[0])
    long = ("This is a long sentence that will be interrupted before it "
            "finishes, which is what a screen reader does constantly.")
    for _ in range(5):
        pro_driver.speak([long])
        time.sleep(0.02)
        pro_driver.cancel()
    assert _spoken(pro_driver, "Still here.", timeout=20.0), \
        "it went silent after being interrupted five times"


def test_changing_pro_voice_rebuilds_and_still_speaks(pro_driver):
    """Pro holds ONE voice: the host has 64 resource slots, Pro takes 50 and a
    voice another ten, so MacinTalk 2's register-them-all trick cannot carry
    over and a voice change means a rebuild. Doing that after a cancel is the
    ordinary case -- NVDA cancels before it speaks the confirmation."""
    voices = _pro_voices(pro_driver)
    if len(voices) < 2:
        pytest.skip("only one MacinTalk Pro voice extracted")
    for vid in voices:
        pro_driver.speak(["something long enough to still be rendering"])
        time.sleep(0.01)
        pro_driver.cancel()
        pro_driver._set_voice(vid)
        assert _spoken(pro_driver, "Switched.", timeout=20.0), \
            "silent after switching to %s" % vid
        # The rebuild happens on the worker, and audio left over from the
        # utterance we just cancelled can reach the player before it lands,
        # so wait for the engine rather than reading it the instant audio
        # appears. An earlier version of this test raced exactly there.
        want = vid.split(":", 1)[1]
        t0 = time.perf_counter()
        while time.perf_counter() - t0 < 20.0:
            eng = pro_driver._engine
            if eng is not None and getattr(eng.voice, "name", None) == want:
                break
            time.sleep(0.01)
        else:
            pytest.fail("the engine never rebuilt on %s" % vid)


def test_crossing_between_pro_and_the_older_engines(pro_driver):
    """osp_init() resets the emulator's globals, so two engines cannot be live
    at once and crossing has to close the old one from the worker thread. Pro
    is the first engine with a 68020 and a self-advancing clock, both of which
    the others must not inherit."""
    voices = _pro_voices(pro_driver)
    if not voices:
        pytest.skip("no MacinTalk Pro voice")
    for vid in ("male", voices[0], "male", voices[-1], "male"):
        pro_driver._set_voice(vid)
        assert _spoken(pro_driver, "Crossing.", timeout=20.0), \
            "silent on %s" % vid


# -- NVDA's speech sequence -------------------------------------------------
#
# The driver kept only IndexCommand and rendered every string as its own
# utterance. Three faults came out of that, and the sibling add-ons fixed all
# three on 2026-08-18; this is the backport, and these are its guards.


def _drain(driver, seq, timeout=5.0):
    """Speak a whole sequence and wait for the audio to reach the player."""
    before = driver._player.bytes
    driver.speak(seq)
    t0 = time.perf_counter()
    while (driver._player.bytes <= before
           and time.perf_counter() - t0 < timeout):
        time.sleep(0.005)
    time.sleep(0.15)                     # let the rest of the sequence land
    return driver._player.bytes > before


def test_nvda_is_told_which_commands_we_want(stubbed):
    """**A command NVDA is not told about is never sent.**

    That is precisely how "capital pitch change percentage" managed to do
    nothing at any value: the driver had no way to know it had been asked,
    because it had never declared it could be.
    """
    import speech.commands
    driver, _built, _a, _b = stubbed
    assert speech.commands.PitchCommand in driver.supportedCommands
    assert speech.commands.BreakCommand in driver.supportedCommands
    assert speech.commands.IndexCommand in driver.supportedCommands


def test_a_pitch_command_reaches_the_engine(stubbed):
    """Capital pitch change, end to end.

    NVDA marks a capital by wrapping it in a PitchCommand carrying an offset
    on its own 0-100 scale, then another with 0 to put it back. Both have to
    arrive as real pitch changes around the letter, or the setting is inert.
    """
    import speech.commands as cmd
    driver, built, _a, _b = stubbed
    assert _drain(driver, ["plain"])
    eng = built[0]
    del eng.pitches[:]
    assert _drain(driver, [cmd.PitchCommand(offset=30), "A",
                           cmd.PitchCommand(offset=0), " apple"])
    assert len(eng.pitches) >= 2, eng.pitches
    # The raised letter, then back to the user's own setting. 50 is the
    # middle of NVDA's scale and means the voice as recorded, so the offset
    # must move away from 0 tenths and then return to it.
    assert eng.pitches[0] > eng.pitches[-1], eng.pitches
    assert eng.pitches[-1] == 0, eng.pitches


def test_the_pitch_offset_is_clamped_with_the_users_setting(stubbed):
    """A user already at 90 who is asked for another 30 gets the top.

    Clamped together rather than separately, so the request is never out of
    range and never wraps.
    """
    driver, _built, _a, _b = stubbed
    driver._pitch = 90
    assert driver._pitchTenths(30) == driver._pitchTenths(100)
    driver._pitch = 10
    assert driver._pitchTenths(-50) == driver._pitchTenths(-100)
    driver._pitch = 50
    assert driver._pitchTenths(0) == 0


def test_adjacent_strings_become_one_utterance(stubbed):
    """A speech sequence is not a list of utterances.

    NVDA hands over the pieces of a line -- text, a link, more text -- as
    plain adjacent strings, and rendering each alone gave every fragment the
    falling intonation of a finished sentence. Two testers reported it on the
    sibling add-ons within a minute of each other as "it pauses between the
    text and the link".
    """
    driver, built, _a, _b = stubbed
    assert _drain(driver, ["one"])
    eng = built[0]
    del eng.spoken[:]
    assert _drain(driver, ["Home", " ", "link"])
    assert len(eng.spoken) == 1, eng.spoken
    assert eng.spoken[0] == "Home link"


def test_joining_does_not_run_words_together(stubbed):
    """Join with a space only where neither side has one.

    Otherwise "link" + "Home" reaches the engine as "linkHome", and a naive
    join puts a double space into text that already had one.
    """
    driver, built, _a, _b = stubbed
    assert _drain(driver, ["one"])
    eng = built[0]
    del eng.spoken[:]
    assert _drain(driver, ["link", "Home"])
    assert eng.spoken == ["link Home"], eng.spoken
    del eng.spoken[:]
    assert _drain(driver, ["link ", "Home"])
    assert eng.spoken == ["link Home"], eng.spoken


def test_an_index_does_not_split_the_utterance(stubbed):
    """NVDA puts one at the START of every say-all line.

    It has already decided through `speakWithoutPauses` that those lines
    belong together, so splitting at the index undoes that decision and hands
    the engine a fragment ending in nothing -- heard as a full stop in the
    middle of a sentence, at exactly the wrapped line boundaries.
    """
    import speech.commands as cmd
    driver, built, _a, _b = stubbed
    assert _drain(driver, ["one"])
    eng = built[0]
    del eng.spoken[:]
    assert _drain(driver, [cmd.IndexCommand(1), "the first line",
                           cmd.IndexCommand(2), " and the second"])
    assert len(eng.spoken) == 1, eng.spoken
    assert eng.spoken[0] == "the first line and the second"


def test_a_break_command_becomes_silence(stubbed):
    """NVDA asking for a pause in so many words.

    Dropped silently until now, so the one place a pause was actually wanted
    was the one place it did not happen.
    """
    import outspoken
    import speech.commands as cmd
    driver, built, _a, _b = stubbed
    assert _drain(driver, ["one"])
    before = driver._player.bytes
    assert _drain(driver, ["a", cmd.BreakCommand(time=200), "b"])
    grew = driver._player.bytes - before
    quiet = len(outspoken._silence16(200))
    assert quiet > 0
    # Two utterances plus the gap. The stub returns a fixed 800 bytes of 8-bit
    # audio each time, which is 1600 once widened.
    assert grew >= 2 * 1600 + quiet, (grew, quiet)


def test_a_cancel_stops_a_render_already_in_flight(stubbed):
    """The hazard streaming introduces, and the guard against it.

    An engine that hands audio over as it renders can go on feeding a
    cancelled utterance for as long as the render lasts -- which for MacinTalk
    3 is the best part of a second on a long sentence. So the sink refuses
    once the cancel generation has moved, and the engine stops.

    Deliberately NOT the generation counter rule 3 forbids: this is read when
    the worker picks the utterance up, so a fresh item can never begin stale.
    The last assertion is the one that matters -- the driver must still speak
    afterwards.
    """
    driver, _built, _a, _b = stubbed
    assert _spoken(driver, "warm up")

    gen = driver._cancels
    refused = []

    def sink(chunk):
        if driver._cancels != gen:
            refused.append(chunk)
            return False
        return True

    assert sink(b"x") is True
    driver.cancel()
    assert sink(b"y") is False, "the sink kept accepting after a cancel"
    assert refused == [b"y"]
    # And the driver is not wedged: the next utterance must still be spoken.
    assert _spoken(driver, "still here")
