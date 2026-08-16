# -*- coding: utf-8 -*-
"""MacinTalk 2 as an engine the driver can drive, alongside `.sp`.

Same shape as `engine.py` -- `translate`, `speak`, `stop`, `set_rate`,
`set_voice`, `close` -- so `outspoken.py` does not have to know which engine it
is talking to.  Two things genuinely differ and the driver must not assume
otherwise:

* **There is no separate translation step.**  `.sp` speaks phonemes and nothing
  else, so the driver runs the NRL rules itself.  MacinTalk 2 ships its own
  front end and takes English, so `translate` only does the number pass and
  hands the text straight through.
* **Speaking is asynchronous.**  `SpeakBuffer` renders one buffer and returns;
  everything after that arrives because the host keeps answering the Sound
  Manager.  See `speak`.

Only one engine can be live at a time, here as in `engine.py`: `osp_init()`
resets the emulator's global state, so building a second Engine invalidates the
first.  The driver rebuilds when the user picks a voice from another engine.

Everything about the component protocol is in docs/macintalk2-components.md.
"""
import ctypes
import os
import sys

_HERE = os.path.dirname(os.path.abspath(__file__))
if _HERE not in sys.path:
    sys.path.insert(0, _HERE)

import osp                                                    # noqa: E402
import numwords                                               # noqa: E402
import voices as voicelib                                     # noqa: E402

FRONT_BASE = 0x00040000
BACK_BASE = 0x00060000
HEAP = 0x00080000
HEAP_SIZE = 0x000E0000
STACK = 0x00200000
TEXT_BUF = 0x00195000
VOICE_SPEC = 0x00196100
STATUS_BUF = 0x00196200
PARAM_BUF = 0x00196300

CPUFLAG, RESERR, MEMERR = 0x012F, 0x0A60, 0x0220

#: The engine renders at the classic Macintosh rate; the driver resamples
#: nothing, exactly as it does not for `.sp`.
NATIVE_RATE = 22254

#: Standard component selectors, from the -1..-6 table at Cecy 3 +$30.
OPEN, CLOSE = -1, -2
#: Component selectors, identified from their handlers.
STATUS, SPEAK, STOP, SET_INFO = 0, 1, 2, 6

#: Speech Manager selectors, from Apple's Speech.h.
SO_CURRENT_VOICE = 0x63766F78          # 'cvox'
SO_RATE = 0x72617465                   # 'rate'
SO_PITCH_BASE = 0x70626173             # 'pbas'

#: The shared tables, which every voice needs.  `ttsd 2` is optional in
#: principle; in practice every extraction has both.
TABLES = (("ttsr", 1), ("ttsd", 1), ("ttsd", 2),
          ("ttss", 0), ("ttph", 1), ("ttop", 1))

REQUIRED = ("Cecy_1.bin", "Cecy_3.bin")

_LIVE = []


def _fixed(x):
    """A Fixed 16.16, which is how the Speech Manager passes rate and pitch."""
    return int(round(x * 65536.0)) & 0xFFFFFFFF


def find(roots):
    """-> ({name: path}, [Voice]) for whatever of MacinTalk 2 is installed.

    Returns empty rather than raising when it is absent: a user with only the
    1984 engine must still get a working synthesiser, which is the whole point
    of enumerating instead of hardcoding.
    """
    files = {}
    for root in roots:
        if not os.path.isdir(root):
            continue
        for dirpath, _dirs, names in os.walk(root):
            for n in names:
                files.setdefault(n, os.path.join(dirpath, n))
    if not all(r in files for r in REQUIRED):
        return {}, []
    found, _bad = voicelib.installed("mtk2", roots=roots)
    return files, found


def usable(roots):
    """Also checks the DLL can actually do components.

    A stale osp_host.dll is easy to end up with, because NVDA holds it open
    while the synthesizer is loaded and it cannot be replaced in place. Listing
    MacinTalk 2 voices against a binary that cannot run them would offer the
    user voices that are guaranteed to be silent.
    """
    files, found = find(roots)
    if not (files and found):
        return False
    try:
        # Already loaded, so this is the same handle rather than a second copy.
        return hasattr(ctypes.CDLL(osp.DLL), "osp_add_component")
    except Exception:
        return False


class Engine(object):
    """One open MacinTalk 2 front end, with one voice selected."""

    number_mode = "words"

    def __init__(self, files, allvoices, voice=None):
        """Register *every* MacinTalk 2 voice, then select one.

        Rebuilding the emulator to change voice would be the obvious reading of
        the one-engine-at-a-time constraint, but it is not necessary here: the
        voices use distinct resource ids -- which is exactly why each `ttvd`
        names its own `ttvi` and `ttvw` -- so all ten can sit in the resource
        table at once and switching is a single SetSpeechInfo('cvox').

        Ten voices is about 300 KB against a 917 KB heap and 40 of the host's
        64 resource slots, so this is affordable rather than clever.  A rebuild
        is still needed to move between *engines*, since osp_init() resets
        everything.
        """
        for old in _LIVE:
            old._dead = True
        _LIVE.append(self)
        self._dead = False
        self.voices = list(allvoices)
        self.voice = voice or self.voices[0]
        self._rate = None

        h = self.h = osp.Host()
        h.load(FRONT_BASE, open(files["Cecy_3.bin"], "rb").read())
        h.load(BACK_BASE, open(files["Cecy_1.bin"], "rb").read())
        h.heap(HEAP, HEAP_SIZE)
        h.mem_traps(True)
        h.w8(CPUFLAG, 0)
        h.w16(RESERR, 0)
        h.w16(MEMERR, 0)

        for rtype, rid in TABLES:
            path = files.get("%s_%d.bin" % (rtype, rid))
            if path:
                h.add_resource(rtype, rid, open(path, "rb").read())

        # Every voice's three resources, under the ids its own ttvd asks for.
        # A voice that will not load is dropped rather than fatal: one bad
        # extraction must not cost the user the other nine.
        loaded = []
        for v in self.voices:
            try:
                ttvd = None
                for kind, path in sorted(v.files.items()):
                    data = open(path, "rb").read()
                    rid = int(os.path.basename(path).split("_")[1].split(".")[0])
                    h.add_resource(kind, rid, data)
                    if kind == "ttvd":
                        ttvd = rid
                if ttvd is None:
                    continue
                # We are the Speech Manager: GetVoiceInfo('fref') answers with
                # the ttvd id, because that is what the engine opens a voice by.
                h.add_voice(v.creator, v.id, ttvd)
                loaded.append(v)
            except Exception:
                continue
        self.voices = loaded
        if not loaded:
            raise RuntimeError("no MacinTalk 2 voice could be loaded")
        # Match on identity of the *voice*, not of the Python object. The
        # caller's Voice comes from its own scan of the ROM folder, so it is
        # never the same instance as ours -- `not in loaded` was therefore
        # always true, and every first MacinTalk 2 voice silently became
        # whichever one sorted first. It spoke, so nothing looked broken.
        self.voice = self._match(self.voice) or loaded[0]

        fe = h.add_component("ttsc", "mtk2", "mtk2", FRONT_BASE)
        h.add_component("t2be", "t2be", "mtk2", BACK_BASE)
        self.chan = h.open_instance(fe)

        h.set_reg(osp.A7, STACK)
        h.set_reg(osp.SR, 0x2700)
        # MacinTalk 2's callback only refills on its second invocation, so
        # answering it while the engine is still mid-call spends the first one
        # before there is anything for it to be about.
        h.defer_callbacks(True)

        reason, result = h.component_call(self.chan, OPEN, [self.chan],
                                          max_instr=50_000_000)
        if reason != 1 or result != 0:
            raise RuntimeError("MacinTalk 2 Open returned %d (stop %d)"
                               % (_signed(result), reason))
        self.select(self.voice)

    # -- set-up ------------------------------------------------------------
    def _match(self, voice):
        """Our own Voice record for `voice`, matched on creator and id."""
        if voice is None:
            return None
        for v in self.voices:
            if v.creator == voice.creator and v.id == voice.id:
                return v
        return None

    def select(self, voice):
        """Switch voice without rebuilding: SetSpeechInfo('cvox', VoiceSpec).

        -> True if the engine took it. A refusal leaves the previous voice in
        place, which is the right failure: still speaking in the wrong voice
        beats silence.
        """
        if self._dead:
            return False
        voice = self._match(voice) or voice
        creator = voice.creator.encode("mac-roman", "replace")
        self.h.w32(VOICE_SPEC, int.from_bytes(creator[:4].ljust(4, b" "), "big"))
        self.h.w32(VOICE_SPEC + 4, voice.id)
        if self._set_info(SO_CURRENT_VOICE, VOICE_SPEC) != 0:
            return False
        self.voice = voice
        return True

    def _set_info(self, selector, arg):
        """SetSpeechInfo(selector, ptr-or-value) -> OSErr."""
        if self._dead:
            return -1
        reason, result = self.h.component_call(
            self.chan, SET_INFO, [selector, arg], max_instr=50_000_000)
        return _signed(result) if reason == 1 else -1

    # -- settings ----------------------------------------------------------
    def _fixed_arg(self, value):
        """Put a Fixed somewhere and return its address.

        **SetSpeechInfo takes a pointer for every selector**, including the
        scalar ones -- `soRate` wants a `Fixed *`, not a `Fixed`. Passing the
        value directly is not merely ignored: the engine dereferences it, and
        for a rate of 232 that means reading address $00E80000. It does not
        crash, it quietly corrupts, and every utterance afterwards came out at
        6.65 seconds instead of 1.84 regardless of the rate asked for.

        'cvox' worked from the start only because a VoiceSpec is passed by
        address anyway, which hid this until the first scalar setting.
        """
        self.h.w32(PARAM_BUF, _fixed(value))
        return PARAM_BUF

    def set_rate(self, rate):
        """Words per minute, which is what the Speech Manager's 'rate' is."""
        self._rate = rate
        self._set_info(SO_RATE, self._fixed_arg(rate))

    def set_voice(self, pitch_hz):
        """Deliberately does nothing yet. Named for `.sp`, where a voice *is*
        a pitch; here a voice is a whole formant set and pitch is separate.

        `soPitchBase` is wired and reaches the engine, but it does not behave:
        90 Hz and 180 Hz produce byte-identical audio while both drop the peak
        amplitude from 78 to 37. Something about the value or its units is
        wrong, and a pitch slider that quietly halves the volume and changes
        nothing else is worse than one that is honestly inert.

        Left as measured rather than guessed at. The next step is to check
        what the handler at Cecy 3 +$11AE does with 'pbas' specifically,
        the same way 'cvox' was settled.
        """
        return

    def read_settings(self):
        return {"rate": self._rate}

    # -- speaking ----------------------------------------------------------
    #: Punctuation MacinTalk 2 pronounces as a word, which has to go because
    #: NVDA has already named whatever the user asked to hear.
    #:
    #: Measured, not assumed. Speaking "x <c> x" for every punctuation
    #: character and comparing against "x x": `-` and `'` come out *shorter*
    #: than the baseline, `, ; :` add only a pause, and everything below is a
    #: second of extra speech. That is why "(x64)" was read as "left paren open
    #: paren x sixty four right paren close parenthesis" -- NVDA supplied the
    #: names and the engine supplied them again.
    #:
    #: `, . ; : ! ? - '` are kept: they are prosody here, not vocabulary.
    SPOKEN_PUNCTUATION = "()[]{}<>@#$%^&*+=/\\|~`\"_"

    def translate(self, text):
        """MacinTalk 2 has its own front end, so this only prepares the text.

        Whatever this returns is what `speak` is handed, which is how the
        driver can treat both engines alike.
        """
        if self.number_mode in ("words", "digits"):
            text = numwords.normalise(
                text, spell_out=(self.number_mode == "digits"))
        # A space, not nothing: removing the character outright would run the
        # words either side together into one.
        for ch in self.SPOKEN_PUNCTUATION:
            if ch in text:
                text = text.replace(ch, " ")
        return text

    def busy(self):
        """SpeechStatusInfo.outputBusy, which is how the engine says it is done.

        `outputBusy` is byte 0 and `inputBytesLeft` is a long at +2; see the
        status handler at Cecy 3 +$5CE, which fills the struct field for field.
        """
        if self._dead:
            return False
        for off in (0, 4, 8):
            self.h.w32(STATUS_BUF + off, 0)
        reason, _r = self.h.component_call(self.chan, STATUS, [STATUS_BUF],
                                           max_instr=20_000_000)
        if reason != 1:
            return False
        return bool(self.h.r8(STATUS_BUF)) or bool(self.h.r32(STATUS_BUF + 2))

    #: A ceiling on one utterance, in buffers. Nothing legitimate approaches
    #: it -- a long sentence is about thirty -- so reaching it means the engine
    #: is producing without ever finishing, and the right answer is to stop and
    #: say so rather than hand NVDA two minutes of silence.
    MAX_BUFFERS = 400

    def speak(self, text):
        """-> 8-bit unsigned PCM at NATIVE_RATE, trailing silence trimmed.

        SpeakBuffer returns as soon as the first buffer is queued, so the audio
        only exists if the host keeps being the Sound Manager afterwards. Each
        callback installs a deferred task, and *that* renders.

        The stopping condition has to come from the engine, not from the
        callback chain going quiet. Pumping until nothing is pending gave 8.5
        seconds for "Testing 30 items" and, after a few voice changes, 131
        seconds of silence: a real-time synthesiser keeps its channel fed
        whether or not it has anything left to say.
        """
        if self._dead:
            return b""
        raw = text.strip().encode("mac-roman", "replace")
        if not raw:
            return b""
        h = self.h
        h.pcm_reset()
        h.load(TEXT_BUF, raw)
        reason, _res = h.component_call(self.chan, SPEAK,
                                        [TEXT_BUF, len(raw), 0],
                                        max_instr=400_000_000)
        if reason != 1:
            return b""
        while h.buffers_taken < self.MAX_BUFFERS:
            if not h.run_callbacks(max_rounds=8):
                break                       # nothing pending: really finished
            if not self.busy():
                break
        else:
            from logHandler import log
            log.warning("MacinTalk 2: utterance hit the %d buffer ceiling"
                        % self.MAX_BUFFERS)

        # Take the audio *now*, then let the engine settle and throw away
        # whatever that produces.
        #
        # `busy()` going false does not mean the Sound Manager is idle: there
        # can still be a callback pending that belongs to the utterance just
        # finished. Left alone it does not run until the *next* speak() pumps,
        # and then it queues the old buffer, so the previous utterance's tail
        # arrives at the front of the new one -- "type here to search" followed
        # by the "ch" of the item before it. Draining here keeps each utterance
        # to its own audio.
        pcm = h.pcm
        h.run_callbacks(max_rounds=64)
        h.pcm_reset()
        return _trim(pcm)

    def stop(self):
        """Deliberately does not touch the emulator. Read this before "fixing".

        The obvious implementation is StopSpeech(kImmediate) -- selector 2 --
        and it is wrong here, dangerously so. `cancel()` runs on NVDA's **main**
        thread while `speak()` is running on the worker, and a component call
        drives the 68000. Two threads stepping one CPU corrupts its state: it
        sounded like buzzing, utterances ran to six seconds of near-silence for
        the word "button", and the pump hit its buffer ceiling.

        `.sp` can stop from another thread because its stop is a single byte
        written into emulated memory. MacinTalk 2 has no equivalent, so the
        honest answer is not to try.

        Nothing is lost. Rendering an utterance takes 15-150 ms, and cancel's
        real work -- draining the queues and stopping the player -- is what
        actually interrupts. At worst one short buffer finishes rendering into
        a queue that is about to be emptied.
        """
        return

    def close(self):
        """Close the component, then unload the DLL.

        The component gets its own Close first so it can release what it
        allocated; then the library goes, which is what unlocks the file for
        the next build. See osp.Host.close.
        """
        if self._dead:
            return
        self._dead = True
        try:
            _LIVE.remove(self)
        except ValueError:
            pass
        try:
            self.h.component_call(self.chan, CLOSE, [], max_instr=20_000_000)
        except Exception:
            pass
        try:
            self.h.close()
        except Exception:
            pass


def _signed(v):
    return v - 0x100000000 if v & 0x80000000 else v


#: 8-bit unsigned silence. The engine clears its buffers to this before it
#: renders into them, so a partly-used final buffer ends in a run of it.
_SILENT = 0x80


def _trim(pcm, keep=1200, lead=220):
    """Drop the silence at both ends, leaving a little at each.

    **The leading silence is the one that is felt.** MacinTalk 2 primes its
    double buffer before it renders anything, so every utterance began with
    about 0.38 s of nothing -- a third of a second of dead air before each
    typed character, heard as a pause and then the tail of the sound arriving
    late. Trailing silence matters too, since the final buffer is only part
    used, but that one only costs latency before the *next* item.

    `keep` leaves about 50 ms at the end and `lead` about 10 ms at the start,
    because cutting hard on a sample clicks.
    """
    if not pcm:
        return pcm
    n = len(pcm)
    start = 0
    while start < n and pcm[start] == _SILENT:
        start += 1
    if start >= n:
        return b""                      # nothing but silence: say nothing
    end = n
    while end > start and pcm[end - 1] == _SILENT:
        end -= 1
    return pcm[max(0, start - lead):min(n, end + keep)]
