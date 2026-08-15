# -*- coding: utf-8 -*-
"""NVDA synthesizer driver for MacinTalk (1984).

The engine is real 68000 code from January 1984, run under Musashi inside
NVDA's own process. No bridge is needed: the emulator is 64-bit native, so
unlike a 32-bit DLL this never touches SynthDriverProxy32.

The engine is not shipped. It is read from a `rom/` folder the user fills from
their own copy -- see `_outspoken/rom.py` and `tools/extract_rom.py`.
"""
import os
import queue
import sys
import threading
import time

import nvwave
import speech.commands
from logHandler import log
from synthDriverHandler import (SynthDriver, VoiceInfo, synthDoneSpeaking,
                                synthIndexReached)

_HERE = os.path.dirname(__file__)
_ENGINE_DIR = os.path.join(_HERE, "_outspoken")
if _ENGINE_DIR not in sys.path:
    sys.path.insert(0, _ENGINE_DIR)

import rom                                                    # noqa: E402

#: The rate the driver writes into its own SoundHeader, rounded to an integer.
#: The error is 0.002%. Resampling to a "nicer" 22050 would cost a per-sample
#: Python loop on every utterance, and simply *declaring* 22050 without
#: resampling would run the voice 0.93% flat -- audible as a wrong pitch on a
#: voice people remember. WASAPI resamples in shared mode anyway.
OUT_RATE = 22254

#: Only the two table-0 voices are offered. `$3A` selects a second formant
#: table (docs/driver-api.md) and the result is thin and chipmunk-like rather
#: than the Amiga narrator's documented robotic mode, which has not been found.
_VOICES = [("male", "Male", 110), ("female", "Female", 250)]

#: How much audio to hand the player at a time, in bytes of 16-bit mono.
#: Small enough that a blocking feed() cannot park the worker for long, large
#: enough not to spend all day in ctypes.
_RATE_MIN, _RATE_MAX = 60.0, 900.0

_CHUNK_MS = 60
_CHUNK_BYTES = int(OUT_RATE * _CHUNK_MS / 1000.0) * 2


class SynthDriver(SynthDriver):
    name = "outspoken"
    description = "MacinTalk (outSPOKEN, 1984)"

    supportedSettings = (
        SynthDriver.VoiceSetting(),
        SynthDriver.RateSetting(),
        SynthDriver.PitchSetting(),
    )
    supportedCommands = {speech.commands.IndexCommand}
    supportedNotifications = {synthIndexReached, synthDoneSpeaking}

    @classmethod
    def check(cls):
        """Only offer the synthesizer when it can actually speak.

        A synthesizer that appears in the list and then says nothing is worse
        than one that is absent, so this requires the engine. Discoverability
        does not suffer: the global plugin explains the empty ROM folder at
        start-up, whether or not the synthesizer is selectable.

        `RULZ` counts as required. Without it the engine still runs, but only
        on phonemes -- and nothing sends a screen reader phonemes.
        """
        try:
            import osp                                        # noqa: F401
        except Exception:
            return False
        return rom.usable()

    def __init__(self):
        super().__init__()
        self._rate, self._pitch = 50, 50
        self._engineRate = 0
        self._voiceId = "male"
        self._engine = None
        self._engineError = None
        self._stopped = False
        self._queue = queue.Queue()
        self._gen = 0
        self._audioOut = False           # is there audio worth interrupting?
        self._player = self._makePlayer()
        # Pushing audio is separated from rendering it, because
        # WavePlayer.feed() blocks until the device has room -- for as long as
        # the audio lasts. With that inside the render loop, synthDoneSpeaking
        # could not be reported until the whole utterance had been pushed, and
        # NVDA paces what it sends next on exactly that notification. Measured:
        # 162 ms between NVDA calling speak() and this driver being handed the
        # next letter, with our own queue wait at zero.
        # Counters, reported when they move. Silence and truncation both look
        # like "nothing happened" from outside, and neither shows up in a
        # latency measurement -- so count the places speech can be discarded.
        self._nSpoken = 0
        self._nStaleItem = 0             # dropped before rendering
        self._nStaleAudio = 0            # rendered, then dropped before playing
        self._nEmpty = 0                 # rendered to nothing
        self._lastReport = 0.0
        self._audioQueue = queue.Queue()
        self._feeder = threading.Thread(target=self._feed, name="outspoken-feed",
                                        daemon=True)
        self._feeder.start()
        self._worker = threading.Thread(target=self._run, name="outspoken",
                                        daemon=True)
        self._worker.start()

    # -- audio -------------------------------------------------------------
    def _makePlayer(self):
        """Build a WavePlayer across NVDA config generations.

        2025.1 removed config.conf["speech"]["outputDevice"] in favour of
        config.conf["audio"]["outputDevice"]. Each attempt must be a callable:
        building the argument dicts up front would evaluate every config lookup
        before the first try block could catch anything.
        """
        import config
        base = dict(channels=1, samplesPerSec=OUT_RATE, bitsPerSample=16)
        try:
            from nvwave import AudioPurpose
            purpose = {"purpose": AudioPurpose.SPEECH}
        except Exception:
            purpose = {}

        def modern():
            return nvwave.WavePlayer(
                outputDevice=config.conf["audio"]["outputDevice"],
                **base, **purpose)

        def legacy():
            return nvwave.WavePlayer(
                outputDevice=config.conf["speech"]["outputDevice"], **base)

        def default():
            return nvwave.WavePlayer(**base, **purpose)

        def bare():
            return nvwave.WavePlayer(1, OUT_RATE, 16)

        last = None
        for attempt in (modern, legacy, default, bare):
            try:
                return attempt()
            except Exception as e:
                last = e
        raise last

    # -- NVDA interface ----------------------------------------------------
    def speak(self, speechSequence):
        items = []
        for item in speechSequence:
            if isinstance(item, str):
                items.append(("text", item))
            elif isinstance(item, speech.commands.IndexCommand):
                items.append(("index", item.index))
        # Stamped with the current generation. NVDA calls cancel() and then
        # speak() in quick succession, so anything queued from here must
        # survive that cancel -- see below.
        self._queue.put((self._gen, items, time.perf_counter()))

    def cancel(self):
        """Drop everything queued before now, and stop what is sounding.

        Deliberately does NOT drain the queue. Draining races against the
        speak() that follows almost every cancel: if the drain lands after the
        new item was queued, the new speech is thrown away and the keystroke is
        silent. That was heard as "typing fast does not speak every letter".

        Bumping a generation counter cannot race. Items carry the generation
        they were queued under, the worker skips any that are stale, and the
        speak() after this one picks up the new value and survives.
        """
        self._gen += 1
        if self._engine:
            self._engine.stop()          # one byte; safe across threads
        # Only touch the player when it actually has something to interrupt.
        # NVDA calls cancel() before nearly every speak(), including when
        # nothing is sounding, and each stop() tears the output stream down so
        # the next feed() pays to start it again. A long utterance absorbs that
        # once; a short one is almost entirely start-up cost, which is why
        # typing echo lagged while whole sentences did not.
        if self._audioOut:
            self._audioOut = False
            try:
                self._player.stop()
            except Exception:
                pass

    def pause(self, switch):
        try:
            self._player.pause(switch)
        except Exception:
            pass

    def terminate(self):
        self._stopped = True
        self.cancel()
        if self._engine:
            self._engine.close()         # stop it touching the emulator
        self._queue.put(None)
        self._audioQueue.put(None)
        try:
            self._player.close()
        except Exception:
            pass

    # -- settings ----------------------------------------------------------
    def _get_rate(self):
        return self._rate

    def _set_rate(self, value):
        self._rate = max(0, min(100, int(value)))

    def _get_pitch(self):
        return self._pitch

    def _set_pitch(self, value):
        self._pitch = max(0, min(100, int(value)))

    def _get_availableVoices(self):
        from collections import OrderedDict
        out = OrderedDict()
        for vid, label, _hz in _VOICES:
            out[vid] = VoiceInfo(vid, label, language="en")
        return out

    def _get_voice(self):
        return self._voiceId

    def _set_voice(self, value):
        if any(v[0] == value for v in _VOICES):
            self._voiceId = value

    def _baseHz(self):
        for vid, _label, hz in _VOICES:
            if vid == self._voiceId:
                return hz
        return 110

    def _applySettings(self, eng):
        """Map NVDA's 0-100 sliders onto the driver's own ranges.

        Rate is geometric rather than linear. The engine is useful from about
        60 to 900 -- measured, a letter takes 0.30 s at 150, 0.18 s at 250 and
        0.07 s at 400 -- and a linear map spends most of the slider in the slow
        half where nobody lives. This puts a comfortable 230 at the midpoint
        and still reaches genuinely fast at the top, which is where a lot of
        screen-reader users sit.
        """
        self._engineRate = int(
            _RATE_MIN * (_RATE_MAX / _RATE_MIN) ** (self._rate / 100.0))
        eng.set_rate(self._engineRate)
        base = self._baseHz()
        factor = 0.5 + (self._pitch / 100.0)                   # 0.5x .. 1.5x
        eng.set_voice(base * factor)

    # -- the worker --------------------------------------------------------
    def _ensureEngine(self):
        if self._engine is not None or self._engineError is not None:
            return self._engine
        found, missing = rom.find()
        if any(n not in found for n in rom.REQUIRED) or \
                "RULZ_1129.bin" not in found:
            self._engineError = "ROM not present"
            log.warning("outSPOKEN: engine not available.\n" + rom.describe())
            return None
        try:
            import engine as engine_mod
            self._engine = engine_mod.Engine(found)
        except Exception:
            self._engineError = "engine failed to start"
            log.error("outSPOKEN: engine failed to start", exc_info=True)
        return self._engine

    #: 8-bit unsigned -> 16-bit signed is exactly "subtract 128, scale by 256",
    #: which in little-endian means the low byte is zero and the high byte is
    #: the sample with its top bit flipped. Both steps below run at C speed;
    #: the obvious per-sample loop costs ~80k Python iterations per utterance
    #: and is felt as latency in a screen reader.
    _FLIP = bytes(b ^ 0x80 for b in range(256))

    @classmethod
    def _to16(cls, pcm8):
        out = bytearray(len(pcm8) * 2)
        out[1::2] = pcm8.translate(cls._FLIP)
        return bytes(out)

    def _report(self):
        """Say what is being discarded, at most once a second."""
        now = time.perf_counter()
        if now - self._lastReport < 1.0:
            return
        self._lastReport = now
        log.info("outSPOKEN: spoken %d, dropped-before-render %d, "
                 "dropped-before-play %d, rendered-empty %d"
                 % (self._nSpoken, self._nStaleItem, self._nStaleAudio,
                    self._nEmpty))

    def _feed(self):
        """Push PCM to the player, in slices, until told otherwise.

        Runs on its own thread so a blocking feed never delays rendering or
        the completion notification. The generation check is what makes a
        cancel drop audio that has been rendered but not yet pushed.
        """
        while not self._stopped:
            item = self._audioQueue.get()
            if item is None:
                return
            gen, data = item
            if gen != self._gen:
                self._nStaleAudio += 1
                self._report()
                continue
            for off in range(0, len(data), _CHUNK_BYTES):
                if gen != self._gen:
                    self._nStaleAudio += 1
                    self._report()
                    break
                try:
                    self._player.feed(data[off:off + _CHUNK_BYTES])
                except Exception:
                    log.error("outSPOKEN: feeding audio failed", exc_info=True)
                    break

    def _run(self):
        """Render queued speech, and wait for playback only when idle.

        Two rules earned by listening to it get things wrong:

        * **Never block while work is outstanding.** Synthesis takes 4-30 ms;
          playback takes seconds. Waiting for the audio after every utterance
          made each keystroke queue behind the sound of the one before it, so
          a letter could arrive half a second late. The wait now happens only
          after a short quiet period with nothing left to render.
        * **Skip stale work rather than dropping fresh work.** Every item
          carries the generation it was queued under; cancel() bumps that
          counter. Anything older is discarded here, where it is safe, instead
          of by draining the queue in cancel(), which raced the speak() that
          follows it and swallowed keystrokes.
        """
        pending = False                 # audio has been fed but not waited on
        while not self._stopped:
            try:
                # Poll briefly while audio is outstanding so a new keystroke is
                # picked up at once; block outright when there is nothing to
                # wait for.
                item = self._queue.get(timeout=0.05) if pending                     else self._queue.get()
            except queue.Empty:
                # Report completion as soon as everything is rendered and fed,
                # WITHOUT waiting for the audio to drain.
                #
                # Waiting is the theoretically right answer and it cost half a
                # second of interactive latency, because NVDA paces what it
                # sends on this notification: with a 0.5 s letter, everything
                # afterwards ran 0.5 s late. The Amiga Narrator add-on, which
                # drives the same kind of engine acceptably, does not wait
                # either.
                #
                # The risk this takes on is the one that add-on has: NVDA can
                # decide the utterance is finished and cancel the tail. Two
                # things make that less likely here than there -- the engine's
                # trailing padding is trimmed, so the audio ends when the
                # speech does, and cancel() only stops the player when there is
                # something to interrupt.
                self._audioOut = False
                synthDoneSpeaking.notify(synth=self)
                pending = False
                continue

            if item is None:
                return
            gen, items, queuedAt = item
            if gen != self._gen:
                self._nStaleItem += 1   # cancelled before we got to it
                self._report()
                continue

            eng = self._ensureEngine()
            if eng is None:
                synthDoneSpeaking.notify(synth=self)
                continue
            try:
                self._applySettings(eng)
                for kind, value in items:
                    if gen != self._gen:
                        break
                    if kind == "index":
                        synthIndexReached.notify(synth=self, index=value)
                        continue
                    t0 = time.perf_counter()
                    phonemes = eng.translate(value)
                    t1 = time.perf_counter()
                    pcm = eng.speak(phonemes)
                    t2 = time.perf_counter()
                    if not pcm:
                        self._nEmpty += 1
                        self._report()
                        continue
                    if gen != self._gen:
                        self._nStaleAudio += 1
                        self._report()
                        continue
                    self._nSpoken += 1
                    self._audioOut = True
                    # Feed in slices, not in one lump.
                    #
                    # WavePlayer.feed() blocks until the device has room, so
                    # handing it a whole utterance parks this thread for most
                    # of that utterance's duration -- measured at 394 ms for a
                    # single letter and 1671 ms for one sentence. Nothing else
                    # can be picked up meanwhile, which is felt as lag while
                    # typing and as keystrokes being skipped outright.
                    #
                    # Slicing bounds the block to one slice and gives a place
                    # to notice that newer speech has arrived, so a keystroke
                    # abandons the rest of the previous utterance instead of
                    # waiting it out.
                    data = self._to16(pcm)
                    self._audioQueue.put((gen, data))
                    t3 = time.perf_counter()
                    # Latency is the thing users report and the thing that is
                    # hardest to guess at, so measure the whole path -- from
                    # NVDA handing us the text to the audio being fed -- and
                    # say where it went. Only when it is slow enough to notice,
                    # so a normal log stays quiet.
                    total = (t3 - queuedAt) * 1000
                    # Time to the FIRST slice is what the listener experiences
                    # as latency; time to the last is only how long the worker
                    # was busy. Reporting the wrong one of those sent me after
                    # feed() when the audio may already have been sounding.
                    if total >= 60:
                        log.info(
                            "outSPOKEN: %.0f ms for %r -> %r "
                            "(wait %.0f, translate %.0f, synth %.0f;"
                            " %.2f s audio, rate %d)"
                            % (total, value[:24], phonemes[:40],
                               (t0 - queuedAt) * 1000, (t1 - t0) * 1000,
                               (t2 - t1) * 1000,
                               len(pcm) / 22254.5454, self._engineRate)
                            + " engine holds pitch=%d rate=%d" % eng.read_settings())
                    pending = True
            except Exception:
                log.error("outSPOKEN: speech failed", exc_info=True)
