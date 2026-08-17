# -*- coding: utf-8 -*-
"""MacinTalk Pro as an engine the driver can drive, alongside `.sp` and 2.

Same shape as `macintalk2.py` -- `translate`, `speak`, `stop`, `set_rate`,
`set_voice`, `close` -- because `outspoken.py` should not have to know which
engine it is talking to.  Pro is a `ttsc` Component Manager component like
MacinTalk 2, with the identical selector map, so the glue carries over
unchanged.  What does NOT carry over is everything about how it finds its
data, and each of these was earned:

* **It requires a 68020.**  `gtse 1 +$282` tests a record built from Gestalt
  and rejects a 68000 or a 68010 outright, writing synthOpenFailed before it
  reads a single resource.  We run it as a 68040, because the modules it
  loads for synthesis contain F-line coprocessor instructions.
* **It is addressed by NAME, not by id.**  The engine is modules called
  `*TTS`, `*Wave`, `*Snd`, `*Lex`, `*Cmd`, `*XPh`, `*XAl`, `*PhX`, `*AlX`,
  `*WvX`, plus `EnglPhon` and `EnglAllo`; a voice is `EnglMBruceData`,
  `EnglMBruceCode`, `EnglMBruce` and `EnglMBruceWave`.  An extraction without
  names is an engine that cannot start.
* **It reads its own files.**  Its 572,928-byte lexicon is in its data fork,
  and a voice's ~800 KB of units is in that voice file's resource fork, which
  it reads by walking the resource map and seeking -- never loading it into a
  Handle.  So both forks are registered, and the host publishes each file's
  real map at `TopMapHndl`.
* **The same type and id mean different things in different files.**  `gtsg 0`
  is 1,032 bytes in the engine and 110 in a voice, and `gtsi 128` is in both,
  so every resource is tagged with the file it came from.

Everything about the component protocol is in docs/macintalk2-components.md;
the Pro-specific findings are in the commit history from 2026-08-16.
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

#: Redrawn for Pro rather than inherited.  One voice's unit database is larger
#: than MacinTalk 2's entire heap, and `osp_add_resource` copies into emulated
#: RAM.  RAM is 16 MB with the host's magic pages from 0x00F00000, so there is
#: room -- but MacinTalk 2's TEXT_BUF at 0x195000 would sit inside this heap,
#: which is why none of its constants are reused.
CODE = 0x00040000
HEAP = 0x00080000
HEAP_SIZE = 0x00A00000                 # 10 MB, ends at 0x00A80000
STACK = 0x00C00000
TEXT_BUF = 0x00C10000
VOICE_SPEC = 0x00C20000
STATUS_BUF = 0x00C20100
PARAM_BUF = 0x00C20200

CPUFLAG, RESERR, MEMERR = 0x012F, 0x0A60, 0x0220

#: The classic Macintosh rate, as `.sp` and MacinTalk 2 also render at.
NATIVE_RATE = 22254

#: Standard component selectors.
OPEN, CLOSE = -1, -2
#: Component selectors, read off the jump table at `gtse 1 +$BC`: the argument
#: byte count each stub loads into d6 is 4, 12, 4, 8, 8 for 0, 1, 2, 5, 6.
STATUS, SPEAK, STOP, GET_INFO, SET_INFO = 0, 1, 2, 5, 6

#: Speech Manager selectors, from Apple's Speech.h.
SO_CURRENT_VOICE = 0x63766F78          # 'cvox'
SO_RATE = 0x72617465                   # 'rate'

#: The component descriptor is the Component Manager's own bookkeeping and is
#: never asked of the Resource Manager.
NOT_A_RESOURCE = ("thng",)

#: Not resources at all -- the forks themselves and the extractor's index.
NOT_A_BIN = ("datafork.bin", "rsrcfork.bin", "resources.tsv", "names.tsv")

REQUIRED = ("gtse_1.bin",)

#: **Set to True only when Pro has actually produced audio.**
#:
#: The engine opens, takes a voice and runs its synthesis modules, but nothing
#: has come out of it yet. Listing a voice that then says nothing is the one
#: failure this project treats as worse than not listing it at all -- see
#: `SynthDriver.check` -- so the voices stay out of NVDA's list until a probe
#: has written a WAV somebody has heard.
#:
#: `py -3 tools/probe_pro_speak.py "Hello" Bruce` is the gate.
SPEAKS = False

#: What the extractor writes beside the `.bin`s: type, id, map entry, name.
INDEX_FILE = "resources.tsv"

_LIVE = []


def _fixed(x):
    """A Fixed 16.16, which is how the Speech Manager passes rate and pitch."""
    return int(round(x * 65536.0)) & 0xFFFFFFFF


def _signed(v):
    return v - 0x100000000 if v & 0x80000000 else v


def engine_dir(roots):
    for root in roots:
        d = os.path.join(root, "macintalkpro")
        if os.path.isdir(d) and os.path.isfile(os.path.join(d, "gtse_1.bin")):
            return d
    return None


def read_index(folder):
    """-> {(type, id): (map entry, mac name)} from `resources.tsv`.

    A Mac resource name is not a file name -- Pro's modules are `*TTS`,
    `*Wave`, `*Lex` -- so the index lives beside the binaries rather than in
    them, and carries the map offset `RsrcMapEntry` answers with.
    """
    out = {}
    p = os.path.join(folder, INDEX_FILE)
    if not os.path.isfile(p):
        return out
    try:
        with open(p, encoding="utf-8") as fh:
            for line in fh:
                row = line.rstrip("\r\n")
                if row.startswith("#") or "\t" not in row:
                    continue
                rtype, rid, entry, nm = row.split("\t", 3)
                out[(rtype, int(rid))] = (int(entry), nm)
    except (OSError, ValueError):
        return {}
    return out


def _split(name):
    """`gtse_1.bin` -> ('gtse', 1).  None for anything that is not one."""
    stem, ext = os.path.splitext(name)
    if ext.lower() != ".bin" or "_" not in stem:
        return None
    rtype, rest = stem.rsplit("_", 1)
    try:
        return rtype, int(rest)
    except ValueError:
        return None


def find(roots):
    """-> (engine folder, [Voice]) for whatever of MacinTalk Pro is installed.

    Empty rather than raising when it is absent: a user with only the older
    engines must still get a working synthesizer.
    """
    d = engine_dir(roots)
    if not d:
        return None, []
    found, _bad = voicelib.installed("gala", roots=roots, speakable=True)
    return d, found


def usable(roots):
    """Also checks the DLL can do components and files.

    A stale osp_host.dll is easy to end up with, because NVDA holds it open
    while the synthesizer is loaded. Listing Pro voices against a binary that
    cannot serve them would offer voices guaranteed to be silent.
    """
    if not SPEAKS:
        return False
    d, found = find(roots)
    if not (d and found):
        return False
    try:
        lib = ctypes.CDLL(osp.DLL)
        return (hasattr(lib, "osp_add_component")
                and hasattr(lib, "osp_add_file")
                and hasattr(lib, "osp_map_entry"))
    except Exception:
        return False


class Engine(object):
    """One open MacinTalk Pro, with one voice registered.

    **One voice, not all of them.** The host holds 64 resources; Pro is 50
    once `thng` is set aside and a voice is another ten, so a second voice
    does not fit. MacinTalk 2's trick of registering every voice and switching
    with one SetSpeechInfo does not carry over -- changing voice here means
    rebuilding, which the driver already does when crossing between engines.
    """

    number_mode = "words"

    def __init__(self, engine_folder, allvoices, voice=None):
        for old in _LIVE:
            old._dead = True
        _LIVE.append(self)
        self._dead = False
        self.voices = list(allvoices)
        self.voice = voice or self.voices[0]
        self._rate = None

        d = engine_folder
        code = open(os.path.join(d, "gtse_1.bin"), "rb").read()

        h = self.h = osp.Host()
        # Before anything is loaded: Open reads Gestalt('proc') and refuses a
        # 68000 or a 68010, and the synthesis modules use F-line instructions.
        h.set_cpu(osp.Host.CPU_68040)
        # **Pro waits on the clock.** It reads low-memory Ticks ($016A)
        # directly and compares it with a deadline, so without one advancing
        # its SpeakBuffer never returns. Off for the other engines on purpose:
        # `.sp` is time-sensitive and a self-advancing clock makes the same
        # sentence render differently twice.
        h.auto_ticks(True)
        h.load(CODE, code)
        h.heap(HEAP, HEAP_SIZE)
        h.mem_traps(True)
        h.w8(CPUFLAG, 0)
        h.w16(RESERR, 0)
        h.w16(MEMERR, 0)

        # The forks first, because a resource is tagged with the file it came
        # from and the file has to exist to be tagged with.
        folders = [("engine", d, "MacinTalk Pro"),
                   ("voice", self.voice.folder, self.voice.name)]
        where = {}
        for label, folder, macname in folders:
            dat = rf = b""
            pd = os.path.join(folder, "datafork.bin")
            pr = os.path.join(folder, "rsrcfork.bin")
            if os.path.isfile(pd):
                dat = open(pd, "rb").read()
            if os.path.isfile(pr):
                rf = open(pr, "rb").read()
            if not dat and not rf:
                raise RuntimeError(
                    "%s has neither fork -- re-run tools/extract_rom.py"
                    % macname)
            h.add_file(macname, dat, rf)
            where[label] = len(where)

        ttvd_id = None
        named = 0
        for label, folder, _macname in folders:
            index = read_index(folder)
            fidx = where[label]
            for name in sorted(os.listdir(folder)):
                if name in NOT_A_BIN:
                    continue
                got = _split(name)
                if not got:
                    continue
                rtype, rid = got
                if rtype in NOT_A_RESOURCE:
                    continue
                data = open(os.path.join(folder, name), "rb").read()
                hnd = h.add_resource(rtype, rid, data, fidx)
                # Pro finds its modules by name and asks RsrcMapEntry where
                # each sits before reading it out of the file, so both ride on
                # the Handle we just got back.
                entry, mac = index.get((rtype, rid), (0, ""))
                if entry:
                    h.map_entry(hnd, entry)
                if mac and h.name_resource(hnd, mac):
                    named += 1
                if rtype == "ttvd" and label == "voice":
                    ttvd_id = rid
        if not named:
            raise RuntimeError(
                "no resource names -- re-run tools/extract_rom.py; MacinTalk "
                "Pro finds its modules by name and cannot start without them")
        if ttvd_id is None:
            raise RuntimeError("%s has no ttvd" % self.voice.name)

        # Which file the voice lives in, and it matters: Pro asks the Speech
        # Manager for the voice's FSSpec and then OPENS it.
        h.add_voice(self.voice.creator, self.voice.id, ttvd_id, where["voice"])

        # thng 128: type 'ttsc', subtype 0, manufacturer 'gala'.
        comp = h.add_component("ttsc", b"\0\0\0\0", "gala", CODE)
        self.chan = h.open_instance(comp)

        h.set_reg(osp.A7, STACK)
        h.set_reg(osp.SR, 0x2700)
        h.defer_callbacks(True)

        reason, result = h.component_call(self.chan, OPEN, [self.chan],
                                          max_instr=200_000_000)
        if reason != 1 or result != 0:
            raise RuntimeError("MacinTalk Pro Open returned %d (stop %d)"
                               % (_signed(result), reason))
        if not self.select(self.voice):
            raise RuntimeError("MacinTalk Pro would not take voice %s"
                               % self.voice.name)

    # -- set-up ------------------------------------------------------------
    def select(self, voice):
        """Switch voice: SetSpeechInfo('cvox', VoiceSpec).

        Only the voice this Engine was built with can be selected -- see the
        class note about the 64-resource ceiling -- so this exists to be
        called once at start-up and to say no otherwise.
        """
        if self._dead:
            return False
        if (voice.creator, voice.id) != (self.voice.creator, self.voice.id):
            return False
        creator = voice.creator.encode("mac-roman", "replace")
        self.h.w32(VOICE_SPEC,
                   int.from_bytes(creator[:4].ljust(4, b" "), "big"))
        self.h.w32(VOICE_SPEC + 4, voice.id)
        return self._set_info(SO_CURRENT_VOICE, VOICE_SPEC) == 0

    def _set_info(self, selector, arg):
        """SetSpeechInfo(selector, ptr-or-value) -> OSErr."""
        if self._dead:
            return -1
        reason, result = self.h.component_call(
            self.chan, SET_INFO, [selector, arg], max_instr=200_000_000)
        return _signed(result) if reason == 1 else -1

    # -- settings ----------------------------------------------------------
    def _fixed_arg(self, value):
        """Put a Fixed somewhere and return its address.

        **SetSpeechInfo takes a pointer for every selector**, the scalar ones
        included; passing the value directly has the engine dereference it.
        That cost a day on MacinTalk 2 and the lesson transfers unchanged.
        """
        self.h.w32(PARAM_BUF, _fixed(value))
        return PARAM_BUF

    def set_rate(self, rate):
        """Words per minute, which is what the Speech Manager's 'rate' is."""
        self._rate = rate
        self._set_info(SO_RATE, self._fixed_arg(rate))

    def set_voice(self, pitch_hz):
        """Named for `.sp`, where a voice *is* a pitch. Inert here until
        'pbas' has been measured on Pro the way it was on MacinTalk 2 -- and
        that one misbehaved, so measure before wiring a slider."""
        return

    def read_settings(self):
        return {"rate": self._rate}

    # -- speaking ----------------------------------------------------------
    #: Punctuation the engine pronounces as a word, which has to go because
    #: NVDA has already named whatever the user asked to hear. Copied from
    #: MacinTalk 2 as a starting point; measure it for Pro before trusting it.
    SPOKEN_PUNCTUATION = "()[]{}<>@#$%^&*+=/\\|~`\"_"

    def translate(self, text):
        """Pro has its own front end, so this only prepares the text."""
        if self.number_mode in ("words", "digits"):
            text = numwords.normalise(
                text, spell_out=(self.number_mode == "digits"))
        for ch in self.SPOKEN_PUNCTUATION:
            if ch in text:
                text = text.replace(ch, " ")
        return text

    def busy(self):
        """SpeechStatusInfo.outputBusy, which is how it says it is done."""
        if self._dead:
            return False
        for off in (0, 4, 8):
            self.h.w32(STATUS_BUF + off, 0)
        reason, _r = self.h.component_call(self.chan, STATUS, [STATUS_BUF],
                                           max_instr=20_000_000)
        if reason != 1:
            return False
        return bool(self.h.r8(STATUS_BUF)) or bool(self.h.r32(STATUS_BUF + 2))

    #: A ceiling on one utterance, in buffers. Reaching it means the engine is
    #: producing without ever finishing, and the right answer is to stop and
    #: say so rather than hand NVDA a minute of noise.
    MAX_BUFFERS = 400

    def speak(self, text):
        """-> 8-bit unsigned PCM at NATIVE_RATE, trailing silence trimmed.

        Asynchronous, exactly as MacinTalk 2 is: SpeakBuffer returns when the
        first buffer is queued, and the rest exists only because the host
        keeps being the Sound Manager afterwards.
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
                break
            if not self.busy():
                break
        else:
            from logHandler import log
            log.warning("MacinTalk Pro: utterance hit the %d buffer ceiling"
                        % self.MAX_BUFFERS)
        pcm = h.pcm
        h.run_callbacks(max_rounds=64)
        h.pcm_reset()
        return _trim(pcm)

    def stop(self):
        """Deliberately does not touch the emulator. Read macintalk2.stop
        before "fixing" this: cancel() runs on NVDA's main thread while the
        worker may be inside speak(), and two threads stepping one emulated
        CPU corrupts it."""
        return

    def close(self):
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


#: 8-bit unsigned silence, which the engine clears its buffers to.
_SILENT = 0x80


def _trim(pcm, keep=1200, lead=220):
    """Drop the silence at both ends, leaving a little at each.

    The leading silence is the one that is felt -- see macintalk2._trim, where
    priming the double buffer cost a third of a second before every typed
    character.
    """
    if not pcm:
        return pcm
    n = len(pcm)
    start = 0
    while start < n and pcm[start] == _SILENT:
        start += 1
    if start >= n:
        return b""
    end = n
    while end > start and pcm[end - 1] == _SILENT:
        end -= 1
    return pcm[max(0, start - lead):min(n, end + keep)]
