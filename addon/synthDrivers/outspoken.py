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
4. *Only the worker touches the emulator.* There is one 68000 and it has no
   lock. Anything that drives it -- a component call, opening an engine,
   closing one -- happens on the worker thread and nowhere else. NVDA calls
   `_set_voice`, `_set_rate` and `terminate` on its main thread while the
   worker may be inside `speak()`, and two threads stepping one CPU corrupts
   it: buzzing that survives every later utterance (issue #1), and for
   `close()` a DLL unloaded out from under running code.

   Settings therefore *record what the user asked for* and the worker
   *reconciles* before each utterance. Not events on the speech queue --
   `cancel()` drains that queue, and NVDA cancels between changing a setting
   and speaking the confirmation of it, so a queued voice change would be
   eaten and the confirmation spoken in the old voice. Comparing state
   converges even when an individual change is missed.

The shape that satisfies the first three is the shape the Amiga Narrator
add-on already had: a queue, a worker, one feed call per utterance, and a
cancel that empties the queues and stops the player.
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
        if macintalk2.usable(rom.search_roots()):
            files, mt2 = macintalk2.find(rom.search_roots())
            for v in mt2:
                out.append(("mtk2:" + v.name, "%s (MacinTalk 2)" % v.name,
                            "mtk2", v))
    except Exception:
        log.debug("outSPOKEN: MacinTalk 2 unavailable", exc_info=True)
    try:
        import macintalk3
        # The 1994 68k engine, run as 68k. `usable` was False until it made a
        # sound somebody heard, the same gate MacinTalk Pro went through --
        # see macintalk3.SPEAKS, open since 2026-08-21.
        if macintalk3.usable(rom.search_roots()):
            _d, mt3 = macintalk3.find(rom.search_roots())
            for v in mt3:
                out.append(("mtk3:" + v.name, "%s (MacinTalk 3)" % v.name,
                            "mtk3", v))
    except Exception:
        log.debug("outSPOKEN: MacinTalk 3 unavailable", exc_info=True)
    try:
        import macintalkpro
        # `usable` was False until MacinTalk Pro actually made a sound --
        # see macintalkpro.SPEAKS, open since 2026-08-20. Opening, taking a
        # voice and running the synthesis modules was never the same thing.
        if macintalkpro.usable(rom.search_roots()):
            _d, pro = macintalkpro.find(rom.search_roots())
            for v in pro:
                out.append(("gala:" + v.name, "%s (MacinTalk Pro)" % v.name,
                            "gala", v))
    except Exception:
        log.debug("outSPOKEN: MacinTalk Pro unavailable", exc_info=True)
    return out


def _whyNot():
    """-> [] when the synthesizer can run, or the reasons it cannot.

    Two quite different failures land here and look identical from outside: an
    emulator DLL that will not load, and a ROM folder that is short a file.
    """
    reasons = []
    dllPath = "(not resolved)"
    try:
        import ctypes
        import osp
        # Importing `osp` only works out a path; the DLL is what actually has
        # to load, and it is the half that can be wrong.
        #
        # A 32-bit NVDA loading a 64-bit build raises "[WinError 193] %1 is not
        # a valid Win32 application", and before this was checked it happened
        # *after* the user had selected the synthesizer: it appeared in the
        # list, took the selection, and then never spoke.
        dllPath = osp.DLL
        ctypes.CDLL(dllPath)
    except Exception as e:
        log.debug("outSPOKEN: the emulator will not load", exc_info=True)
        reasons.append("the emulator will not load: %s" % e)
        reasons.append("DLL: %s" % dllPath)
        return reasons
    # Either engine is enough. Requiring `.sp` would hide MacinTalk 2 from a
    # user who extracted only that, which their disk image decides, not us.
    if not _catalogue():
        reasons.append("no engine has everything it needs:")
        try:
            reasons.extend("  " + ln for ln in rom.explain()[1])
        except Exception as e:
            reasons.append("  could not describe the ROM folder: %s" % e)
    return reasons


_MISSING = (
    "outSPOKEN cannot start, because the engine is not there yet.\n\n"
    "This add-on ships no part of MacinTalk. You supply it from your own copy "
    "and put the extracted files into:\n\n"
    "%s\n\n"
    "The extract_rom.py tool in the project repository will pull them out of a "
    "disk image for you. NVDA's log has the full list of what was found and "
    "what was missing.\n\n"
    "Open that folder now?"
)


def _explainLater(folder):
    """Show the engine-missing dialog once NVDA has finished failing.

    Never straight from `__init__`: a modal dialog there would stall the
    synthesizer switch with speech half torn down. Queued instead, so it
    arrives after NVDA has fallen back to the previous synthesizer -- which it
    always does, so the user is never stranded without speech.

    It lands on top of NVDA's own "Could not load the outspoken synthesizer"
    box rather than after it, because that box runs a nested event loop which
    dispatches this. That ordering is the right way round: ours is the one with
    something to act on.
    """
    try:
        import wx
        import gui
    except ImportError:
        return

    def show():
        try:
            answer = gui.messageBox(_MISSING % folder, "outSPOKEN",
                                    wx.YES_NO | wx.ICON_INFORMATION)
            if answer == wx.YES:
                os.makedirs(folder, exist_ok=True)
                os.startfile(folder)
        except Exception:
            log.error("outSPOKEN: could not show the engine dialog",
                      exc_info=True)
    wx.CallAfter(show)


def _sameVoice(a, b):
    """Match on creator and id, never on object identity.

    The driver's Voice comes from its own scan of the ROM folder, so it is
    never the same instance the engine is holding -- an identity test is
    always false, and on MacinTalk 2 that silently gave every user whichever
    voice happened to sort first. It spoke, so nothing looked broken.
    """
    if a is None or b is None:
        return False
    return (a.creator, a.id) == (b.creator, b.id)


#: The engine is useful from about 60 to 900 -- a letter takes 0.30 s at 150,
#: 0.18 s at 250, 0.07 s at 400. Geometric, so the midpoint is a comfortable
#: 232 instead of spending most of the slider in the slow half.
_RATE_MIN, _RATE_MAX = 60.0, 900.0

#: How far the pitch slider reaches either side of the voice's own pitch, in
#: semitones. An octave each way, which is as far as any of these stay
#: recognisable, and the same span the Tiger and Leopard add-ons use so that
#: one setting means one thing across all of them.
#:
#: Both engines run out before the slider does, and harmlessly: MacinTalk 2
#: renders identically from 'pbas' 72 upward, MacinTalk Pro from about 69,
#: which is a wasted top end rather than a fault. Measured with
#: `tools/probe_pitch.py`.
_PITCH_SEMITONES = 12


def _silence16(ms):
    """A `BreakCommand`'s pause, as 16-bit signed silence at the output rate.

    Zero is silence in signed 16-bit, so this is just the right number of
    zeroed bytes -- no need to go through the engine's 8-bit unsigned 0x80.
    """
    return bytes(2 * int(OUT_RATE * max(0, int(ms)) / 1000))


class SynthDriver(SynthDriver):
    name = "outspoken"
    description = "MacinTalk (outSPOKEN, 1984)"

    supportedSettings = (
        SynthDriver.VoiceSetting(),
        SynthDriver.RateSetting(),
        SynthDriver.PitchSetting(),
        SynthDriver.VolumeSetting(),
        # **Kept in the list for MacinTalk 1's two voices, which cannot obey
        # it.** NVDA does rebuild both the settings panel and the settings
        # ring when the voice changes, so hiding this on those two would work
        # -- but `initSettings` freezes the config spec from whatever
        # `supportedSettings` says at start-up, so a driver that begins on
        # `male` would leave `inflection` out of the spec for the session and
        # write an unvalidated key on the way out. A control that does nothing
        # for two voices in thirty-four is the smaller wart, and the readme
        # names them.
        SynthDriver.InflectionSetting(),
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
    #: **A command must be listed here or NVDA will not send it at all**,
    #: which is how "capital pitch change percentage" managed to do nothing
    #: whatever it was set to: the driver had no way to know it had been asked.
    supportedCommands = {
        speech.commands.IndexCommand,
        speech.commands.BreakCommand,
        speech.commands.PitchCommand,
        speech.commands.VolumeCommand,
        speech.commands.RateCommand,
    }
    supportedNotifications = {synthIndexReached, synthDoneSpeaking}

    @classmethod
    def check(cls):
        """Always offer the synthesizer, and explain on selection if it cannot
        run.

        This used to hide itself unless it could speak, reasoning that one
        which appears in the list and then says nothing is worse than one that
        is absent. That is still true -- but it describes a driver that *loads*
        and then produces no audio, which is what the 32-bit DLL mismatch used
        to do. It does not describe one that refuses to load: NVDA catches
        that, falls back to the previous synthesizer, and speech never stops.

        What hiding cost was every route to an explanation. All NVDA writes is
        "Synthesizer 'outspoken' doesn't pass the check, excluding from list",
        and the two failures behind it -- a DLL of the wrong bitness, and two
        of three ROM files -- are indistinguishable from outside. The start-up
        dialog was supposed to cover that, and on at least one machine it never
        appears at all.

        So be present and say why, every time it is chosen, without depending
        on catching anyone during start-up.
        """
        return True

    def __init__(self):
        super().__init__()
        problems = _whyNot()
        if problems:
            log.warning("outSPOKEN cannot start:\n  %s" % "\n  ".join(problems))
            _explainLater(rom.config_dir())
            raise RuntimeError("outSPOKEN has no engine to run")
        self._rate, self._pitch = 50, 50
        self._volume, self._inflection = 100, 50
        #: The conversion tables the worker is rendering with. Set per flush
        #: rather than per setting change, so a `VolumeCommand` inside a
        #: sequence and the user's own slider go through the same path.
        self._gain = self._gainTables(100)
        self._numberWords = True
        self._engineRate = 0
        self._voiceCatalogue = None
        self._voiceId = None
        cat = self._catalogue()
        if cat:
            self._voiceId = cat[0][0]
        self._engine = None
        self._engineKind = None
        self._engineError = None
        self._voiceRefused = None
        self._stopped = False
        self._audioOut = False           # is there audio worth interrupting?
        #: Bumped by every cancel. A render already in flight compares it with
        #: what it started under and stops feeding when they differ.
        #:
        #: **This is not the generation counter rule 3 forbids.** That one
        #: stamped items when they were *queued* and compared when they were
        #: rendered, so a cancel arriving in between made an item stale before
        #: it had ever been looked at -- and the driver reached a state where
        #: every item was stale and never recovered. This is read when the
        #: worker *picks up* an utterance, which is necessarily after any
        #: cancel that preceded it, so a fresh item can never start stale. It
        #: only ever abandons the one utterance being rendered right now.
        self._cancels = 0
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

    #: One entry per volume, built on demand. Four engines and a slider the
    #: user drags, so this is a handful of entries at most.
    _GAINS = {}

    @classmethod
    def _gainTables(cls, volume):
        """The 8-to-16 widening with `volume` already folded into it.

        -> (low bytes or None, high bytes), both `bytes.translate` tables.

        **Volume is applied here rather than in the engines, and the reason is
        arithmetic.** Every one of the four hands back 8-bit unsigned, so
        attenuating inside an engine would quantise 256 levels down to
        whatever the slider left of them -- volume 25 would be six bits and
        audibly grainy. Scaling after the widening keeps all 256 whatever the
        slider says. It also works the same everywhere, including MacinTalk 1,
        whose 1984 driver has no volume control at all: four csCodes, and none
        of them is amplitude (docs/driver-api.md).

        **There is no boost, and that is measured rather than assumed.** The
        sibling Leopard driver needed per-voice normalisation; this one does
        not, for two reasons that both had to be checked:

        * *There is no headroom.* Eleven of MacinTalk 3's nineteen voices and
          two of Pro's three already peak at 127 or 128 of a possible 128.
          Nothing here can be turned up; Leopard's fix was only available
          because its `[[volm]]` goes above 1.0 into float samples.
        * *The default is not the quiet one.* Leopard's real complaint was
          that Alex sat 8 dB below Bruce and was also the default. Measured
          across all thirty-four voices at rate 200, the everyday ones span
          about 5 dB -- Ben 15.1 RMS to the 1984 Female's 27.0 -- and the
          voice this driver starts on, the 1984 Male at 19.8, is in the upper
          half of it. Whisper's 8.6 is the voice doing what it is named for.

        So the slider only ever attenuates, and nothing can clip that did not
        clip before.

        **At 100 the tables are the ones the driver has always used**: a zero
        low byte, and the sample with its top bit flipped. `None` rather than
        256 zeroes so full volume costs exactly one `translate` as it did.
        """
        volume = max(0, min(100, int(volume)))
        if volume not in cls._GAINS:
            if volume >= 100:
                cls._GAINS[volume] = (None, cls._FLIP)
            else:
                g = volume / 100.0
                lo, hi = bytearray(256), bytearray(256)
                for b in range(256):
                    v = int(round((b - 128) * 256 * g))
                    v = max(-32768, min(32767, v))
                    lo[b] = v & 0xFF
                    hi[b] = (v >> 8) & 0xFF
                cls._GAINS[volume] = (bytes(lo), bytes(hi))
        return cls._GAINS[volume]

    def _to16(self, pcm8):
        lo, hi = self._gain
        out = bytearray(len(pcm8) * 2)
        out[1::2] = pcm8.translate(hi)
        if lo is not None:
            out[0::2] = pcm8.translate(lo)
        return bytes(out)

    # -- NVDA interface ----------------------------------------------------
    def speak(self, speechSequence):
        # What NVDA actually sent, when someone has turned debug logging on.
        # Every "it pauses in the middle of a sentence" so far has been about
        # where the sequence was divided, and that is invisible from this side
        # without a log or a guess.
        if log.isEnabledFor(log.DEBUG):
            shape = []
            for item in speechSequence:
                shape.append(repr(item[:200]) if isinstance(item, str)
                             else type(item).__name__)
            log.debug("outSPOKEN: sequence %s" % " | ".join(shape))
        items = []
        for item in speechSequence:
            if isinstance(item, str):
                items.append(("text", item))
            elif isinstance(item, speech.commands.IndexCommand):
                items.append(("index", item.index))
            elif isinstance(item, speech.commands.BreakCommand):
                # NVDA asking for a pause in so many words. Dropped silently
                # until now, so the one place a pause was actually wanted was
                # the one place it did not happen.
                items.append(("break", item.time))
            elif isinstance(item, speech.commands.PitchCommand):
                # **How NVDA marks a capital letter.** An offset on its own
                # 0-100 pitch scale, 0 meaning the user's setting again. This
                # is "capital pitch change percentage", and dropping it is why
                # that setting did nothing at any value.
                items.append(("pitch", item.offset))
            elif isinstance(item, speech.commands.VolumeCommand):
                # Same shape as the pitch one: an offset on the user's own
                # 0-100 scale, 0 meaning their setting again.
                items.append(("volume", item.offset))
            elif isinstance(item, speech.commands.RateCommand):
                items.append(("rate", item.offset))
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
        # Before draining, so a render in flight sees it as early as possible:
        # MacinTalk 3 takes the best part of a second over a long sentence,
        # and without this the whole of an abandoned utterance still arrives.
        self._cancels += 1
        for q in (self._queue, self._audioQueue):
            while True:
                try:
                    q.get_nowait()
                except queue.Empty:
                    break
        # Read it once. The worker may close and clear it at any moment, and
        # `if self._engine: self._engine.stop()` can be None by the second half.
        eng = self._engine
        if eng is not None:
            # `stop()` is the ONE engine call this thread may make, and each
            # engine decides what it can safely do here. `.sp` writes a single
            # byte into emulated memory; MacinTalk 2 has no equivalent and
            # deliberately does nothing, because a component call would drive
            # the 68000 while the worker is also driving it. See
            # macintalk2.Engine.stop. Both are no-ops on a closed engine.
            eng.stop()
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
        """Stop the threads, then let the worker close the engine on its way out.

        Closing it from here is the same race as issue #1 and a worse one:
        `close()` is a component call *and* unloads the host DLL, so a worker
        still inside `speak()` would be executing code that had just been
        unmapped. The worker owns the engine for the whole of its life,
        including the end of it -- see `_run`.
        """
        self._stopped = True
        self.cancel()
        self._queue.put(None)
        self._audioQueue.put(None)
        # **This used to say "15-150 ms with room to spare", and then two
        # slower engines arrived.** MacinTalk Pro renders at about 17x
        # realtime and MacinTalk 3 at 23x, so a long post can take well over a
        # second -- and switching synthesizer in the middle of one produced
        # "the worker did not stop; leaving the engine open" in Tomi's log.
        #
        # The real fix is above: `_stopped` is set before this, and a
        # streaming render checks it every piece, so it abandons the utterance
        # within a buffer or so rather than finishing it. The longer wait is
        # for the engines that cannot stream -- MacinTalk 1 renders a whole
        # utterance inside one CPU call with nothing to interrupt.
        #
        # If it still does not come back it is wedged, and leaking one engine
        # is far better than unloading a DLL underneath a running thread.
        self._worker.join(timeout=5.0)
        if self._worker.is_alive():
            log.warning("outSPOKEN: the worker did not stop; "
                        "leaving the engine open")
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

    def _get_volume(self):
        return self._volume

    def _set_volume(self, value):
        """Takes effect on the next utterance, deliberately.

        Nothing is re-scaled in flight: the tables are chosen per flush, so
        what is already in the audio queue keeps the volume it was rendered
        at. Changing a setting must never cut off what is being spoken, and
        NVDA speaks the new value the moment the slider moves.
        """
        self._volume = max(0, min(100, int(value)))

    def _get_inflection(self):
        return self._inflection

    def _set_inflection(self, value):
        self._inflection = max(0, min(100, int(value)))

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
        """Record the choice. Applying it is the worker's job -- see `_sync`.

        This runs on NVDA's MAIN thread. It used to call `select()` here, which
        for MacinTalk 2 is a component call, and crossing between engines it
        called `close()`, which is a component call plus a DLL unload -- both
        while the worker might be inside `speak()`. That is issue #1: buzzing
        and popping that persists for every later utterance, because what is
        corrupted is the emulated CPU rather than one utterance.

        So nothing here touches the engine. Assigning `_voiceId` is a single
        reference store, which is atomic, and the worker reconciles against it
        before it renders anything.
        """
        want = self._entry(value)
        if want is None or want[0] == self._voiceId:
            return
        self._voiceId = want[0]
        # A different voice deserves a fresh attempt: the last one may have
        # failed for reasons that were about that voice and not this one.
        self._engineError = None

    def _baseHz(self):
        e = self._entry()
        if e is not None and e[2] == "sp":
            return e[3]
        return 110

    def _pitchTenths(self, adj=0):
        """NVDA's 0-100 as tenths of a semitone either side of the voice's own
        pitch. 50 is the voice exactly as it was recorded.

        An offset rather than an absolute, because every voice has a pitch of
        its own -- Votron sits at 38 and Mariel at 61, more than an octave
        apart -- so an absolute scale would put the middle of the slider
        somewhere different for each one.

        `adj` is what NVDA has asked for on top of the user's setting, on its
        own 0-100 scale: a `PitchCommand` carrying the "capital pitch change
        percentage". Clamped into 0-100 together, so a user already at 90 who
        asks for another 30 gets the top rather than an out-of-range request.
        """
        pitch = min(100, max(0, self._pitch + adj))
        return int(round((pitch - 50) * _PITCH_SEMITONES * 10 / 50.0))

    def _applySettings(self, eng, adj=0, radj=0):
        rate = min(100, max(0, self._rate + radj))
        self._engineRate = int(
            _RATE_MIN * (_RATE_MAX / _RATE_MIN) ** (rate / 100.0))
        eng.set_rate(self._engineRate)
        # Inflection has no command of its own -- NVDA has no InflectionCommand
        # -- so unlike rate and pitch there is nothing to offset it by.
        # MacinTalk 1 takes this and does nothing with it; see engine.py.
        eng.set_inflection(self._inflection)
        tenths = self._pitchTenths(adj)
        e = self._entry()
        if e is not None and e[2] == "sp":
            # 1984 has no voice apart from its pitch and nothing to ask, so
            # the offset goes on the base this voice is named for. The engine
            # takes hertz directly and clamps itself at 65 and 500.
            eng.set_voice(self._baseHz() * 2.0 ** (tenths / 120.0))
        else:
            # The others are Speech Manager components, and 'pbas' is a
            # musical scale: twelve units to the octave, with 60 at middle C.
            # They are asked what the voice's own is and told to move from it.
            eng.set_pitch(tenths)

    # -- the threads -------------------------------------------------------
    #
    # Everything from here down runs on the worker, and everything that drives
    # the 68000 lives here for that reason. See rule 4 at the top of the file.

    def _sync(self):
        """Make the live engine match the voice the user has chosen. -> engine.

        Called before every utterance. It compares state rather than replaying
        events, so it is idempotent, it converges no matter when the user's
        change lands, and -- unlike an item queued behind the speech -- it
        cannot be swallowed by the `cancel()` that NVDA issues before it speaks
        the confirmation of a settings change.
        """
        entry = self._entry()
        if entry is None:
            return self._ensureEngine()          # reports the missing ROM
        if self._engine is not None and self._engineKind != entry[2]:
            # Crossing between engines. osp_init() resets the emulator's
            # globals, so two cannot be live at once: the old one has to go,
            # and it has to go from this thread.
            self._closeEngine()
        elif (self._engine is not None and entry[2] == "gala"
                and not _sameVoice(getattr(self._engine, "voice", None),
                                   entry[3])):
            # MacinTalk Pro holds ONE voice, not all of them: the host has 64
            # resource slots, Pro itself takes 50 and a voice another ten, so
            # a second will not fit. Changing voice means rebuilding, which is
            # the same thing crossing between engines already does.
            self._closeEngine()
        eng = self._ensureEngine()
        if eng is None or entry[2] not in ("mtk2", "mtk3"):
            return eng                           # `.sp` has one voice per id
        want, cur = entry[3], getattr(eng, "voice", None)
        if cur is not None and (cur.creator, cur.id) == (want.creator, want.id):
            return eng
        # MacinTalk 2 and MacinTalk 3 change voice in place: every voice they
        # have is already registered, so this is one SetSpeechInfo('cvox')
        # rather than a rebuild. MacinTalk 3 fits all nineteen because a
        # formant voice is tiny -- 45 of the host's 64 resource slots for the
        # engine and every voice together. See the Engine.select of each.
        if eng.select(want):
            self._voiceRefused = None
        elif self._voiceRefused != entry[0]:
            # A refusal leaves the previous voice in place, which is the right
            # failure -- speaking in the wrong voice beats silence. Said once
            # and not once per utterance, because we will try again for each.
            self._voiceRefused = entry[0]
            log.warning("outSPOKEN: the engine refused voice %r" % entry[0])
        return eng

    def _closeEngine(self):
        """Retire the live engine. Worker thread only, or after it has stopped.

        `close()` unloads the host DLL as well as closing the component, which
        is why this may not happen while a render might be in flight.
        """
        eng, self._engine = self._engine, None
        self._engineKind = None
        self._engineError = None
        self._voiceRefused = None
        if eng is None:
            return
        try:
            eng.close()
        except Exception:
            log.error("outSPOKEN: closing the engine failed", exc_info=True)

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
            elif kind == "mtk3":
                import macintalk3
                folder, allv = macintalk3.find(rom.search_roots())
                self._engine = macintalk3.Engine(folder, allv, payload)
            elif kind == "gala":
                import macintalkpro
                folder, allv = macintalkpro.find(rom.search_roots())
                self._engine = macintalkpro.Engine(folder, allv, payload)
            else:
                found, _missing = rom.find()
                import engine as engine_mod
                self._engine = engine_mod.Engine(found)
            self._engine.number_mode = (
                "words" if self._numberWords else "digits")
            self._engineKind = kind
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
        """Render queued speech, and close the engine on the way out.

        The close belongs here rather than in `terminate()` because it is the
        one place that can be sure no render is in flight -- there is no other
        thread to be inside `speak()`. `finally`, so it also happens if the
        loop dies of something unforeseen.
        """
        try:
            self._render()
        finally:
            self._closeEngine()

    def _render(self):
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
            eng = self._sync()
            if eng is None:
                synthDoneSpeaking.notify(synth=self)
                continue
            try:
                #: What NVDA has asked us to add to the user's pitch, volume
                #: and rate for the text that follows. 0 means the user's own
                #: setting. Pitch is the one NVDA core sends -- it is how a
                #: capital letter is marked -- but all three arrive the same
                #: way and cost the same to honour.
                adj = vadj = radj = 0
                #: Text collected since the last flush. **A speech sequence is
                #: not a list of utterances**: NVDA hands over the pieces of a
                #: line -- text, a link, more text -- as separate strings, and
                #: rendering each alone gave every fragment the falling
                #: intonation and final lengthening of a finished sentence.
                #: Measured on the sibling add-ons, splitting one line cost
                #: 163 ms across two joins, and none of it is silence to trim.
                run = []
                #: Indexes seen since the last flush, reported just before the
                #: audio around them. **An index must not force a split**:
                #: NVDA puts one at the *start* of every line during say-all,
                #: having already decided through `speakWithoutPauses` that
                #: those lines belong together, so splitting there undoes that
                #: decision and puts a full stop mid-sentence.
                indexes = []
                for kind, value in items:
                    if self._stopped:
                        break
                    if kind == "text":
                        run.append(value)
                        continue
                    if kind == "index":
                        indexes.append(value)
                        continue
                    if self._flush(eng, run, adj, vadj, radj, indexes,
                                   queuedAt):
                        pending = True
                    if kind == "break":
                        self._audioOut = True
                        self._audioQueue.put(_silence16(value))
                        pending = True
                    elif kind == "pitch":
                        adj = value
                    elif kind == "volume":
                        vadj = value
                    elif kind == "rate":
                        radj = value
                if not self._stopped:
                    if self._flush(eng, run, adj, vadj, radj, indexes,
                                   queuedAt):
                        pending = True
                for index in indexes:           # nothing left to speak
                    synthIndexReached.notify(synth=self, index=index)
                del indexes[:]
            except Exception:
                log.error("outSPOKEN: speech failed", exc_info=True)

    def _flush(self, eng, run, adj, vadj, radj, indexes, queuedAt):
        """Render everything collected so far as ONE utterance. -> spoke?

        Joins with a space only where neither side has one, or "link" + "Home"
        would reach the engine as "linkHome".

        The indexes collected since the last flush are reported immediately
        *before* this audio rather than splitting it. That matches what they
        mean: NVDA's say-all index is the `lineReached` callback, placed at the
        start of a line -- "we have just started speaking this" -- and it is
        also what asks for the next line, so reporting it early keeps the
        pipeline fed rather than starving it.
        """
        text = ""
        for piece in run:
            if text and not text[-1].isspace() and piece[:1] and \
                    not piece[0].isspace():
                text += " "
            text += piece
        del run[:]
        for index in indexes:
            synthIndexReached.notify(synth=self, index=index)
        del indexes[:]
        if not text.strip():
            return False
        # Pitch is re-applied per flush because NVDA's capital-letter offset
        # changes *within* a sequence, and the engines take it as a setting
        # rather than as part of the text. Rate and inflection ride along.
        self._applySettings(eng, adj, radj)
        # Volume never reaches an engine: it is folded into the widening that
        # every chunk goes through on the way to the player. Chosen here so
        # that a `VolumeCommand` part-way through a sequence applies to the
        # audio it precedes and to nothing already queued.
        self._gain = self._gainTables(self._volume + vadj)
        t0 = time.perf_counter()
        phonemes = eng.translate(text)
        t1 = time.perf_counter()

        # **Stream when the engine can.** MacinTalk 3 renders at about 24x
        # realtime against MacinTalk 2's 157x, so a long sentence took the
        # best part of a second before a sample of it could be played -- heard
        # as the engine being laggy in a way the others are not. Given a sink
        # it hands each piece over as it is rendered, and the first sound
        # arrives after about 30 ms instead of 540.
        #
        # `gen` is read HERE, when this utterance starts, which is necessarily
        # after any cancel that preceded it -- so an utterance can never begin
        # already stale. See the note on `_cancels`.
        gen = self._cancels
        fed = [0]

        def sink(chunk):
            if self._stopped or self._cancels != gen:
                return False                # cancelled: stop rendering
            self._audioOut = True
            self._audioQueue.put(self._to16(chunk))
            fed[0] += len(chunk)
            return True

        # Declared rather than discovered. Catching TypeError around the call
        # would also swallow one raised *inside* a render and then quietly
        # speak the whole utterance twice.
        if getattr(eng, "STREAMS", False):
            pcm = eng.speak(phonemes, sink=sink)
        else:
            pcm = eng.speak(phonemes)
        t2 = time.perf_counter()
        if fed[0]:
            self._nSpoken += 1
            pcm = None                      # already handed over, piece by piece
        elif not pcm:
            self._nEmpty += 1
            self._report()
            return False
        else:
            self._nSpoken += 1
            self._audioOut = True
            self._audioQueue.put(self._to16(pcm))
        total = (time.perf_counter() - queuedAt) * 1000
        if total >= 60:
            log.info(
                "outSPOKEN: %.0f ms for %r -> %r "
                "(wait %.0f, translate %.0f, synth %.0f;"
                " %.2f s audio, rate %d, pitch %+d, volume %d,"
                " inflection %d)"
                % (total, text[:24], phonemes[:40],
                   (t0 - queuedAt) * 1000, (t1 - t0) * 1000,
                   (t2 - t1) * 1000,
                   (fed[0] if pcm is None else len(pcm)) / 22254.5454,
                   self._engineRate, adj,
                   max(0, min(100, self._volume + vadj)), self._inflection))
        return True
