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
    def translate(self, text):
        """MacinTalk 2 has its own front end, so this is only the number pass.

        Returning the text unchanged is deliberate and is why the driver can
        treat both engines alike: whatever `translate` returns is what `speak`
        is handed.
        """
        if self.number_mode in ("words", "digits"):
            text = numwords.normalise(
                text, spell_out=(self.number_mode == "digits"))
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
        return _trim(h.pcm)

    def stop(self):
        """StopSpeech(kImmediate).

        Unlike `.sp` this is a real call rather than a flag poke, so it can
        only run between utterances -- the driver already serialises on its
        worker thread.
        """
        if self._dead:
            return
        try:
            self.h.component_call(self.chan, STOP, [0], max_instr=20_000_000)
        except Exception:
            pass

    def close(self):
        self._dead = True
        try:
            self.h.component_call(self.chan, CLOSE, [], max_instr=20_000_000)
        except Exception:
            pass


def _signed(v):
    return v - 0x100000000 if v & 0x80000000 else v


#: 8-bit unsigned silence. The engine clears its buffers to this before it
#: renders into them, so a partly-used final buffer ends in a run of it.
_SILENT = 0x80


def _trim(pcm, keep=1200):
    """Drop the trailing silence, leaving a short tail.

    The last buffer is only partly filled and the engine pads the rest, so
    without this every utterance carries up to 4096 samples -- nearly a fifth
    of a second -- of nothing at the end. On a screen reader that is felt
    directly as latency between one item and the next.

    `keep` leaves about 50 ms, because cutting hard on the final sample clicks.
    """
    end = len(pcm)
    while end > 0 and pcm[end - 1] == _SILENT:
        end -= 1
    if end == len(pcm):
        return pcm
    return pcm[:min(len(pcm), end + keep)]
