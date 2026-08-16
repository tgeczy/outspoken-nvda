# -*- coding: utf-8 -*-
"""NVDA synthesizer driver for MacinTalk (1984).

The engine is real 68000 code from January 1984, run under Musashi inside
NVDA's own process. No bridge is needed: the emulator is 64-bit native, so
unlike a 32-bit DLL this never touches SynthDriverProxy32.

The engine is not shipped. It is read from a folder the user fills from their
own copy -- see `_outspoken/rom.py` and `tools/extract_rom.py`.

**Three rules this file exists to obey.** Each was learned by breaking it:

1. *Never block the render loop on playback.* `WavePlayer.feed()` blocks until
   the device has room, for as long as the audio lasts. With that in the render
   loop, `synthDoneSpeaking` could not be reported until an utterance had
   finished being pushed -- and NVDA paces what it sends on that notification,
   so every keystroke arrived a letter late. Feeding has its own thread.
2. *Hand the player a whole utterance at a time.* Slicing created a holding
   area where rendered audio waited to be discarded: 367 of 435 utterances were
   thrown away before reaching the device in one session, heard as words cut in
   half and as silence lasting a dozen keystrokes.
3. *Discard work by draining, never by stamping it.* A generation counter
   compared at render time froze the driver silent -- 615 utterances spoken,
   then 194 consecutive items discarded unheard, with no recovery. Permanently
   silent is a far worse failure than occasionally speaking something stale.

The shape that satisfies all three is the shape the Amiga Narrator add-on
already had: a queue, a worker, one feed call per utterance, and a cancel that
empties the queues and stops the player.
"""
import os
import queue
import sys
import threading
import time

import nvwave
import speech.commands
from logHandler import log
from autoSettingsUtils.driverSetting import BooleanDriverSetting
from synthDriverHandler import (SynthDriver, VoiceInfo, synthDoneSpeaking,
                                synthIndexReached)

_HERE = os.path.dirname(__file__)
_ENGINE_DIR = os.path.join(_HERE, "_outspoken")
if _ENGINE_DIR not in sys.path:
    sys.path.insert(0, _ENGINE_DIR)

import rom                                                    # noqa: E402

#: The rate the driver writes into its own SoundHeader, rounded. The error is
#: 0.002%. Declaring a "nicer" 22050 without resampling would run the voice
#: 0.93% flat, and resampling properly would cost a per-sample Python loop on
#: every utterance. WASAPI resamples in shared mode anyway.
OUT_RATE = 22254

#: Only the two table-0 voices are offered. `$3A` selects a second formant
#: table (docs/driver-api.md); the result is thin and chipmunk-like rather than
#: the Amiga narrator's documented robotic mode, which has not been found.
_VOICES = [("male", "Male", 110), ("female", "Female", 250)]


def _catalogue():
    """Every voice the user can actually run, across every engine present.

    Built, never hardcoded. Which engines a user has depends entirely on what
    they extracted from their own disk image, so a fixed list would offer
    voices that cannot speak -- and a synthesizer that lists a voice and then
    says nothing is worse than one that does not list it.

    -> [(id, label, kind, payload)] where `kind` picks the engine module and
    `payload` is whatever that module needs: a base pitch for `.sp`, a Voice
    record for MacinTalk 2.
    """
    out = []
    found, _missing = rom.find()
    if all(n in found for n in rom.REQUIRED) and "RULZ_1129.bin" in found:
        # `male` and `female`, unprefixed, because NVDA persists the voice id
        # and these two shipped first. Renaming them to `sp:male` would silently
        # reset the voice of every existing user on upgrade. Labels are free to
        # change; ids are not.
        for vid, label, hz in _VOICES:
            out.append((vid, "%s (MacinTalk 1)" % label, "sp", hz))
    try:
        import macintalk2
        if not macintalk2.usable(rom.search_roots()):
            return out
        files, mt2 = macintalk2.find(rom.search_roots())
        for v in mt2:
            out.append(("mtk2:" + v.name, "%s (MacinTalk 2)" % v.name,
                        "mtk2", v))
    except Exception:
        log.debug("outSPOKEN: MacinTalk 2 unavailable", exc_info=True)
    return out

#: The engine is useful from about 60 to 900 -- a letter takes 0.30 s at 150,
#: 0.18 s at 250, 0.07 s at 400. Geometric, so the midpoint is a comfortable
#: 232 instead of spending most of the slider in the slow half.
_RATE_MIN, _RATE_MAX = 60.0, 900.0


class SynthDriver(SynthDriver):
    name = "outspoken"
    description = "MacinTalk (outSPOKEN, 1984)"

    supportedSettings = (
        SynthDriver.VoiceSetting(),
        SynthDriver.RateSetting(),
        SynthDriver.PitchSetting(),
        # The 1984 rules cannot count: `RULZ` bucket 26 holds the ten digit
        # names and nothing else, so `30` is spoken "three zero". Reading them
        # as words is an addition on our side, and it is offered rather than
        # imposed -- digit by digit is genuinely easier to follow for phone
        # numbers, version strings and long identifiers, and some users want the
        # engine exactly as it was.
        BooleanDriverSetting(
            "numberWords",
            _("Read &numbers as words (thirty, not three zero)"),
            defaultVal=True,
        ),
    )
    supportedCommands = {speech.commands.IndexCommand}
    supportedNotifications = {synthIndexReached, synthDoneSpeaking}

    @classmethod
    def check(cls):
        """Only offer the synthesizer when it can actually speak.

        One that appears in the list and then says nothing is worse than one
        that is absent. Discoverability does not suffer: the global plugin
        explains the empty ROM folder at start-up either way.
        """
        try:
            import osp                                        # noqa: F401
        except Exception:
            return False
        # Either engine is enough. Requiring `.sp` would hide MacinTalk 2 from
        # a user who extracted only that, which their disk image decides, not
        # us.
        return bool(_catalogue())

    def __init__(self):
        super().__init__()
        self._rate, self._pitch = 50, 50
        self._numberWords = True
        self._engineRate = 0
        self._voiceCatalogue = None
        self._voiceId = None
        cat = self._catalogue()
        if cat:
            self._voiceId = cat[0][0]
        self._engine = None
        self._engineError = None
        self._stopped = False
        self._audioOut = False           # is there audio worth interrupting?
        self._nSpoken = self._nEmpty = 0
        self._lastReport = 0.0
        self._queue = queue.Queue()
        self._audioQueue = queue.Queue()
        self._player = self._makePlayer()
        self._feeder = threading.Thread(target=self._feed,
                                        name="outspoken-feed", daemon=True)
        self._feeder.start()
        self._worker = threading.Thread(target=self._run, name="outspoken",
                                        daemon=True)
        self._worker.start()

    # -- audio -------------------------------------------------------------
    def _makePlayer(self):
        """Build a WavePlayer across NVDA config generations.

        2025.1 removed config.conf["speech"]["outputDevice"] in favour of
        config.conf["audio"]["outputDevice"]. Each attempt has to be a
        callable: building the argument dicts up front would evaluate every
        config lookup before the first try block could catch anything.
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

    #: 8-bit unsigned to 16-bit signed is "subtract 128, scale by 256", which
    #: in little-endian means a zero low byte and the sample with its top bit
    #: flipped. Both steps run at C speed; the obvious per-sample loop costs
    #: ~80k Python iterations an utterance and is felt as latency.
    _FLIP = bytes(b ^ 0x80 for b in range(256))

    @classmethod
    def _to16(cls, pcm8):
        out = bytearray(len(pcm8) * 2)
        out[1::2] = pcm8.translate(cls._FLIP)
        return bytes(out)

    # -- NVDA interface ----------------------------------------------------
    def speak(self, speechSequence):
        items = []
        for item in speechSequence:
            if isinstance(item, str):
                items.append(("text", item))
            elif isinstance(item, speech.commands.IndexCommand):
                items.append(("index", item.index))
        self._queue.put((items, time.perf_counter()))

    def cancel(self):
        """Discard what is queued and stop what is sounding.

        Runs on NVDA's MAIN thread, which is also the thread that turns
        application typedCharacter events into speech. Anything slow here
        stalls those events, and they then arrive in a batch with the next
        keystroke -- which is what "press space three times and hear
        'space space space I'" looks like. So it is timed.
        """
        t0 = time.perf_counter()
        for q in (self._queue, self._audioQueue):
            while True:
                try:
                    q.get_nowait()
                except queue.Empty:
                    break
        if self._engine:
            # Each engine decides what it can safely do from this thread.
            # `.sp` writes one byte into emulated memory; MacinTalk 2 has no
            # equivalent and does nothing, because a component call here would
            # drive the 68000 while the worker is also driving it. See
            # macintalk2.Engine.stop.
            self._engine.stop()
        # ALWAYS stop the player. This was once gated on a flag meant to avoid
        # restarting the output stream needlessly -- but that flag tracked the
        # worker being busy, not the player having audio, and the worker goes
        # idle 50 ms after the last item while the sound is still playing. So
        # by the time a keystroke arrived the gate was shut and cancel did
        # nothing: speech carried on after Tab was released, and "copy" queued
        # up behind "178777 characters selected" instead of interrupting it.
        #
        # Interrupting is the entire job of cancel(). The stream-restart cost
        # was a hypothesis; the broken interruption was observed.
        self._audioOut = False
        tStop = time.perf_counter()
        try:
            self._player.stop()
        except Exception:
            pass
        now = time.perf_counter()
        if (now - t0) * 1000 >= 15:
            log.info("outSPOKEN: cancel took %.0f ms on the main thread "
                     "(player.stop %.0f ms)"
                     % ((now - t0) * 1000, (now - tStop) * 1000))

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

    def _get_numberWords(self):
        return self._numberWords

    def _set_numberWords(self, value):
        """Takes effect on the next utterance, not this one.

        The engine reads `number_mode` while translating, and translation
        happens on the worker thread, so there is nothing to flush here -- but
        also nothing to interrupt. Changing a setting must never cut off what
        is currently being spoken.
        """
        self._numberWords = bool(value)
        if self._engine is not None:
            self._engine.number_mode = "words" if value else "digits"

    def _get_availableVoices(self):
        from collections import OrderedDict
        out = OrderedDict()
        for vid, label, _kind, _payload in self._catalogue():
            out[vid] = VoiceInfo(vid, label, language="en")
        return out

    def _catalogue(self):
        """Cached, because NVDA asks for the voice list very often and this
        walks the ROM folder and parses every `ttvd` it finds."""
        if self._voiceCatalogue is None:
            self._voiceCatalogue = _catalogue()
        return self._voiceCatalogue

    def _entry(self, vid=None):
        vid = vid or self._voiceId
        for e in self._catalogue():
            if e[0] == vid:
                return e
        cat = self._catalogue()
        return cat[0] if cat else None

    def _get_voice(self):
        return self._voiceId

    def _set_voice(self, value):
        want = self._entry(value)
        if want is None or want[0] == self._voiceId:
            return
        was = self._entry()
        self._voiceId = want[0]
        if self._engine is None:
            return
        if was is not None and was[2] == want[2] == "mtk2":
            # Same engine: MacinTalk 2 can change voice in place, because all
            # of them are already registered. See macintalk2.Engine.select.
            if self._engine.select(want[3]):
                self._applySettings(self._engine)
                return
        if was is not None and was[2] == want[2] == "sp":
            self._applySettings(self._engine)
            return
        # Crossing between engines. osp_init() resets the emulator's globals,
        # so two cannot be live at once -- the old one has to go first.
        try:
            self._engine.close()
        except Exception:
            pass
        self._engine = None
        self._engineError = None

    def _baseHz(self):
        e = self._entry()
        if e is not None and e[2] == "sp":
            return e[3]
        return 110

    def _applySettings(self, eng):
        self._engineRate = int(
            _RATE_MIN * (_RATE_MAX / _RATE_MIN) ** (self._rate / 100.0))
        eng.set_rate(self._engineRate)
        eng.set_voice(self._baseHz() * (0.5 + self._pitch / 100.0))

    # -- the threads -------------------------------------------------------
    def _ensureEngine(self):
        if self._engine is not None or self._engineError is not None:
            return self._engine
        entry = self._entry()
        if entry is None:
            self._engineError = "ROM not present"
            log.warning("outSPOKEN: no engine available.\n" + rom.describe())
            return None
        kind, payload = entry[2], entry[3]
        try:
            if kind == "mtk2":
                import macintalk2
                files, allv = macintalk2.find(rom.search_roots())
                self._engine = macintalk2.Engine(files, allv, payload)
            else:
                found, _missing = rom.find()
                import engine as engine_mod
                self._engine = engine_mod.Engine(found)
            self._engine.number_mode = (
                "words" if self._numberWords else "digits")
        except Exception:
            self._engineError = "engine failed to start"
            log.error("outSPOKEN: %s engine failed to start" % kind,
                      exc_info=True)
        return self._engine

    def _report(self):
        now = time.perf_counter()
        if now - self._lastReport < 2.0:
            return
        self._lastReport = now
        log.info("outSPOKEN: spoken %d, rendered-empty %d"
                 % (self._nSpoken, self._nEmpty))

    def _feed(self):
        """Push each utterance to the player in one call, on its own thread.

        Nothing is discarded here. cancel() empties this queue itself and stops
        the player, which flushes the device and releases a blocking feed at
        the same time.
        """
        while not self._stopped:
            item = self._audioQueue.get()
            if item is None:
                return
            try:
                self._player.feed(item)
            except Exception:
                log.error("outSPOKEN: feeding audio failed", exc_info=True)

    def _run(self):
        """Render queued speech and hand the audio to the feeder."""
        pending = False
        while not self._stopped:
            try:
                # Poll while audio is outstanding so new speech is picked up at
                # once; block outright when there is nothing to wait for.
                item = (self._queue.get(timeout=0.05) if pending
                        else self._queue.get())
            except queue.Empty:
                self._audioOut = False
                synthDoneSpeaking.notify(synth=self)
                pending = False
                continue

            if item is None:
                return
            items, queuedAt = item
            eng = self._ensureEngine()
            if eng is None:
                synthDoneSpeaking.notify(synth=self)
                continue
            try:
                self._applySettings(eng)
                for kind, value in items:
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
                    self._nSpoken += 1
                    self._audioOut = True
                    self._audioQueue.put(self._to16(pcm))
                    pending = True
                    total = (time.perf_counter() - queuedAt) * 1000
                    if total >= 60:
                        log.info(
                            "outSPOKEN: %.0f ms for %r -> %r "
                            "(wait %.0f, translate %.0f, synth %.0f;"
                            " %.2f s audio, rate %d)"
                            % (total, value[:24], phonemes[:40],
                               (t0 - queuedAt) * 1000, (t1 - t0) * 1000,
                               (t2 - t1) * 1000,
                               len(pcm) / 22254.5454, self._engineRate))
            except Exception:
                log.error("outSPOKEN: speech failed", exc_info=True)
