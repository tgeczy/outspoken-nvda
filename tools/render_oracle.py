# -*- coding: utf-8 -*-
"""The render oracle: Python-driven and C-driven speech, byte for byte.

2.0 moves the driving of every engine out of the Python modules and into the
host.  The Python stays as the specification, and this is how the port is
held to it: the same fixed sequence of utterances and settings is played
through `engine.py`, `macintalk2.py`, `macintalk3.py` and `macintalkpro.py`
on one side and through the host's `osp_engine_*` calls on the other, and
every render must come out identical -- the 8-bit PCM after each module's own
tidying, which is what the driver hands to the player.

**Sequences, not single renders.**  Every hard bug these modules have had was
one utterance leaking into the next: a stale stop pointer, a buffer not
wiped, a restated final buffer, a pitch cache not cleared on a voice switch.
So each engine instance plays a script -- short, long, a letter, nothing, the
short one again, settings changed and restored, another voice, the first
voice back -- and the whole script has to agree.

**Two processes.**  Both sides load the same library, and the library is one
global CPU.  The Python side renders in one process and the C side in
another; this process only diffs.

**A frozen third reference.**  Both sides share the host core, so a change
that shifts emulation moves both equally and the diff stays zero.
`tests/baseline/renders.json` holds the sequence's hashes as the 1.2.x binary
rendered them (hashes only; no engine data), and the Python side must match
it too.  `--freeze` rewrites it, deliberately, and says so.

    py -3 tools/render_oracle.py                 # both sides, smoke script, diff
    py -3 tools/render_oracle.py --full mtk2     # plus the full grid for one engine
    py -3 tools/render_oracle.py --expect sp     # an engine the host must provide
    py -3 tools/render_oracle.py --freeze        # Python side -> the baseline
    py -3 tools/render_oracle.py --side python --out f.json   # one side, one process

Exit status is the number of disagreements.  Needs the engine data, so it
runs here and on the Linux box, never in CI.  `OSP_HOST_DLL` names the
library, as everywhere.
"""
import argparse
import hashlib
import json
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
ADDON = os.path.join(ROOT, "addon", "synthDrivers", "_outspoken")
BASELINE = os.path.join(ROOT, "tests", "baseline", "renders.json")

# The add-on's modules first, then tools/: whichever is inserted last wins.
sys.path.insert(0, HERE)
sys.path.insert(0, ADDON)

import paths                                                  # noqa: E402
import voices as voicelib                                     # noqa: E402

# ---- what gets spoken --------------------------------------------------------

SHORT = "Hello."
LONG_EN = ("Welcome to outSPOKEN. In 1984 the Macintosh introduced itself, and "
           "it spoke: 1,234 words at 25.5 percent, on the 3rd try, for $9.99. "
           "Does it still work? Yes -- it does, every time.")
LONG_ES = ("Bienvenido. En 1996 Apple regal\xf3 este sintetizador: 1,234 "
           "palabras al 25.5 por ciento, en el 3er intento, por $9.99. "
           "\xbfFunciona todav\xeda? S\xed, funciona.")
MEDIUM_EN = "The quick brown fox jumps over 12 lazy dogs."
MEDIUM_ES = "El veloz murci\xe9lago hind\xfa com\xeda feliz cardillo y kiwi, 12 veces."
LETTER = "e"
LETTER_A = "a"
NUMBERS = "The 3rd of 1,234 items, at 25.5%, minus -7."
PUNCT = "(x64) - test! \"quoted\" a/b [c] {d} <e> @f #g $h ~i _j"
DIGITS_TEXT = "2024 items"

#: The full grid, for the engine being ported.
RATES = (60, 120, 180, 300, 500, 900)
PITCHES = (-120, -60, 0, 60, 120)
INFLECTIONS = (0, 50, 100)
GRID_TEXTS = (SHORT, MEDIUM_EN, NUMBERS)
GRID_TEXTS_ES = (SHORT, MEDIUM_ES, NUMBERS)

DEFAULT_RATE, DEFAULT_PITCH, DEFAULT_INFLECTION = 180, 0, 50


# ---- what to open: the voice references ----------------------------------------

class VoiceRef(object):
    """One voice as an engine wants it named and found."""
    def __init__(self, creator, vid, name, folder=None, files=None, hz=None):
        self.creator, self.id, self.name = creator, vid, name
        self.folder, self.files, self.hz = folder, dict(files or {}), hz

    @property
    def key(self):
        return "%s:%s" % (self.creator, self.name)


class EngineRef(object):
    """Everything an engine instance needs opened, as explicit paths.

    Filled from the Python modules' own `find()` today and from the host's
    catalogue later; the engine calls never walk a directory themselves.
    """
    def __init__(self, kind, files=None, folder=None, voices=(), spanish=False):
        self.kind, self.files = kind, dict(files or {})
        self.folder, self.voices, self.spanish = folder, list(voices), spanish

    @property
    def label(self):
        v = self.voices[0] if self.voices else None
        return self.kind if self.kind != "pro" else "pro/%s" % (v.creator if v else "?")

    def manifest(self, first):
        """The text the host's osp_engine_open takes: one key=value a line."""
        out = ["engine=%s" % self.kind]
        if self.folder:
            out.append("folder=%s" % self.folder)
        for name, path in sorted(self.files.items()):
            out.append("file:%s=%s" % (name, path))
        for v in self.voices:
            out.append("voice=%s\t%d\t%s\t%s" % (v.creator, v.id, v.name, v.folder or ""))
            for kind, path in sorted(v.files.items()):
                out.append("vfile:%s=%s" % (kind, path))
        out.append("select=%s\t%d" % (first.creator, first.id))
        return "\n".join(out) + "\n"


def engine_refs():
    """-> [EngineRef] for whatever engine data this machine has."""
    roots = paths.roots()
    refs = []

    files = {}
    for n in ("DRVR_1030.bin", "TALK_1001.bin", "RULZ_1129.bin", "DICT_-4048.bin"):
        p = paths.find(n)
        if p:
            files[n] = p
    if "DRVR_1030.bin" in files and "TALK_1001.bin" in files:
        refs.append(EngineRef("sp", files, voices=[
            VoiceRef("sp", 110, "Male", hz=110), VoiceRef("sp", 250, "Female", hz=250)]))

    import macintalk2
    f2, v2 = macintalk2.find(roots)
    if f2 and v2:
        want = dict((n, f2[n]) for n in macintalk2.REQUIRED)
        for rtype, rid in macintalk2.TABLES:
            n = "%s_%d.bin" % (rtype, rid)
            if n in f2:
                want[n] = f2[n]
        refs.append(EngineRef("mtk2", want, voices=[
            VoiceRef(v.creator, v.id, v.name, v.folder, v.files) for v in v2]))

    import macintalk3
    d3, v3 = macintalk3.find(roots)
    if d3 and v3:
        refs.append(EngineRef("mtk3", folder=d3, voices=[
            VoiceRef(v.creator, v.id, v.name, v.folder, v.files) for v in v3]))

    import macintalkpro
    _d, vp = macintalkpro.find(roots)
    for v in vp:
        folder = macintalkpro.engine_dir(roots, v.creator)
        if folder:
            refs.append(EngineRef("pro", folder=folder, spanish=(v.creator == "cami"),
                                  voices=[VoiceRef(v.creator, v.id, v.name, v.folder, v.files)]))
    return refs


# ---- the two sides --------------------------------------------------------------

class PythonSide(object):
    """The reference: the add-on's own engine modules."""
    name = "python"

    def __init__(self):
        self.eng = None
        self.kind = None
        self.ref = None

    def open(self, ref, first):
        self.ref, self.kind = ref, ref.kind
        roots = paths.roots()
        if ref.kind == "sp":
            import engine
            self.eng = engine.Engine(ref.files)
        elif ref.kind == "mtk2":
            import macintalk2
            files, allv = macintalk2.find(roots)
            self.eng = macintalk2.Engine(files, allv, self._match(allv, first))
        elif ref.kind == "mtk3":
            import macintalk3
            folder, allv = macintalk3.find(roots)
            self.eng = macintalk3.Engine(folder, allv, self._match(allv, first))
        elif ref.kind == "pro":
            import macintalkpro
            _f, allv = macintalkpro.find(roots)
            v = self._match(allv, first)
            self.eng = macintalkpro.Engine(ref.folder, allv, v)
        else:
            raise ValueError(ref.kind)
        self.eng.number_mode = "words"
        return True

    @staticmethod
    def _match(allv, ref):
        for v in allv:
            if v.creator == ref.creator and v.id == ref.id:
                return v
        raise KeyError(ref.key)

    def select(self, voice):
        if self.kind == "sp":
            self.eng.set_voice(voice.hz)
            return True
        if self.kind == "pro":
            return True
        return bool(self.eng.select(self._match(self.eng.voices, voice)))

    def set_rate(self, wpm):
        self.eng.set_rate(wpm)

    def set_pitch(self, tenths, voice):
        if self.kind == "sp":
            self.eng.set_voice(voice.hz * 2.0 ** (tenths / 120.0))
        else:
            self.eng.set_pitch(tenths)

    def set_inflection(self, percent):
        self.eng.set_inflection(percent)

    def set_numbers(self, mode):
        self.eng.number_mode = mode

    def translate(self, text):
        return self.eng.translate(text)

    def speak(self, prepared):
        return self.eng.speak(prepared)

    def close(self):
        if self.eng is not None:
            self.eng.close()
        self.eng = None


class HostSide(object):
    """The port: the host's osp_engine_* calls over ctypes."""
    name = "host"
    NOT_PORTED = -100

    def __init__(self):
        import ctypes
        import osp
        self.ct = ctypes
        self.lib = ctypes.CDLL(os.environ.get("OSP_HOST_DLL") or osp.DLL)
        L = self.lib
        try:
            L.osp_engine_open.argtypes = [ctypes.c_char_p]
            L.osp_engine_open.restype = ctypes.c_int
            L.osp_engine_error.restype = ctypes.c_char_p
            L.osp_engine_select.argtypes = [ctypes.c_char_p, ctypes.c_int]
            L.osp_engine_select.restype = ctypes.c_int
            L.osp_engine_set_rate.argtypes = [ctypes.c_int]
            L.osp_engine_set_pitch.argtypes = [ctypes.c_int]
            L.osp_engine_set_voice_hz.argtypes = [ctypes.c_double]
            L.osp_engine_set_inflection.argtypes = [ctypes.c_int]
            L.osp_engine_set_numbers.argtypes = [ctypes.c_int]
            L.osp_engine_translate.argtypes = [ctypes.c_char_p, ctypes.c_int,
                                               ctypes.c_char_p, ctypes.c_int]
            L.osp_engine_translate.restype = ctypes.c_int
            L.osp_engine_speak.argtypes = [ctypes.c_char_p, ctypes.c_int]
            L.osp_engine_speak.restype = ctypes.c_int
            L.osp_engine_pcm.argtypes = [ctypes.c_char_p, ctypes.c_int]
            L.osp_engine_pcm.restype = ctypes.c_int
            self.available = True
        except AttributeError:
            self.available = False
        self.kind = None

    def open(self, ref, first):
        if not self.available:
            return False
        self.kind = ref.kind
        m = ref.manifest(first).encode("utf-8")
        r = self.lib.osp_engine_open(m)
        if r == self.NOT_PORTED:
            return False
        if r != 0:
            raise RuntimeError("osp_engine_open(%s): %d %s"
                               % (ref.label, r, self.lib.osp_engine_error()))
        self.lib.osp_engine_set_numbers(1)
        return True

    def select(self, voice):
        if self.kind == "sp":
            self.lib.osp_engine_set_voice_hz(float(voice.hz))
            return True
        if self.kind == "pro":
            return True
        return bool(self.lib.osp_engine_select(voice.creator.encode("mac_roman"), voice.id))

    def set_rate(self, wpm):
        self.lib.osp_engine_set_rate(wpm)

    def set_pitch(self, tenths, voice):
        if self.kind == "sp":
            self.lib.osp_engine_set_voice_hz(voice.hz * 2.0 ** (tenths / 120.0))
        else:
            self.lib.osp_engine_set_pitch(tenths)

    def set_inflection(self, percent):
        self.lib.osp_engine_set_inflection(percent)

    def set_numbers(self, mode):
        self.lib.osp_engine_set_numbers({None: 0, "words": 1, "digits": 2}[mode])

    def _text_call(self, fn, raw):
        cap = len(raw) * 16 + 64
        for _ in range(2):
            buf = self.ct.create_string_buffer(cap)
            n = fn(raw, len(raw), buf, cap)
            if n < 0:
                raise RuntimeError("host text call failed: %d" % n)
            if n <= cap:
                return buf.raw[:n]
            cap = n
        raise RuntimeError("host kept asking for a bigger buffer")

    def translate(self, text):
        raw = text.encode("mac_roman", "replace")
        return self._text_call(self.lib.osp_engine_translate, raw)

    def speak(self, prepared):
        n = self.lib.osp_engine_speak(prepared, len(prepared))
        if n < 0:
            raise RuntimeError("osp_engine_speak failed: %d" % n)
        buf = self.ct.create_string_buffer(max(n, 1))
        got = self.lib.osp_engine_pcm(buf, n)
        return buf.raw[:got]

    def close(self):
        if self.available:
            self.lib.osp_engine_close()


# ---- the script ------------------------------------------------------------------

def _digest(pcm):
    return [len(pcm), hashlib.md5(pcm).hexdigest()]


def play(side, ref, full):
    """One engine instance through the whole script. -> {voice key: [[label, len, md5]...]}"""
    first = ref.voices[0]
    if not side.open(ref, first):
        return None
    out = {}
    n = [0]

    def say(voice, what, text):
        n[0] += 1
        prepared = side.translate(text)
        pcm = side.speak(prepared)
        label = "%03d %s" % (n[0], what)
        out.setdefault(voice.key, []).append([label] + _digest(pcm))
        # The prepared text travels too: for `.sp` it is the phoneme string,
        # and a translation difference is easier to read than a PCM one.
        rec = prepared if isinstance(prepared, bytes) else prepared.encode("mac_roman", "replace")
        out[voice.key][-1].append(hashlib.md5(rec).hexdigest()[:8])

    def defaults(voice):
        side.set_rate(DEFAULT_RATE)
        side.set_pitch(DEFAULT_PITCH, voice)
        side.set_inflection(DEFAULT_INFLECTION)
        side.set_numbers("words")

    long_text = LONG_ES if ref.spanish else LONG_EN
    medium = MEDIUM_ES if ref.spanish else MEDIUM_EN

    try:
        v0 = first
        side.select(v0)
        defaults(v0)
        say(v0, "short", SHORT)
        say(v0, "long", long_text)
        say(v0, "letter", LETTER)
        say(v0, "empty", "")
        say(v0, "short again", SHORT)
        say(v0, "letter a", LETTER_A)
        say(v0, "numbers", NUMBERS)
        say(v0, "punctuation", PUNCT)
        side.set_numbers("digits");  say(v0, "digits mode", DIGITS_TEXT)
        side.set_numbers(None);      say(v0, "numbers off", DIGITS_TEXT)
        side.set_numbers("words")
        side.set_rate(300);          say(v0, "rate 300", SHORT)
        side.set_rate(60);           say(v0, "rate 60", SHORT)
        side.set_rate(DEFAULT_RATE)
        side.set_pitch(60, v0);      say(v0, "pitch +60", SHORT)
        side.set_pitch(-60, v0);     say(v0, "pitch -60", SHORT)
        side.set_pitch(0, v0)
        side.set_inflection(0);      say(v0, "inflection 0", SHORT)
        side.set_inflection(100);    say(v0, "inflection 100", SHORT)
        side.set_inflection(DEFAULT_INFLECTION)
        say(v0, "short after settings", SHORT)

        for v in ref.voices[1:]:
            if not side.select(v):
                out.setdefault(v.key, []).append(["000 refused", 0, "-", "-"])
                continue
            defaults(v)
            say(v, "short", SHORT)
            say(v, "medium", medium)
            side.set_pitch(60, v);   say(v, "pitch +60", SHORT)
            side.set_pitch(0, v)

        if len(ref.voices) > 1:
            side.select(v0)
            defaults(v0)
            say(v0, "short, first voice back", SHORT)

        if full:
            texts = GRID_TEXTS_ES if ref.spanish else GRID_TEXTS
            for v in ref.voices:
                side.select(v)
                for rate in RATES:
                    for pitch in PITCHES:
                        for infl in INFLECTIONS:
                            side.set_rate(rate)
                            side.set_pitch(pitch, v)
                            side.set_inflection(infl)
                            for t in texts:
                                say(v, "grid r%d p%+d i%d %r" % (rate, pitch, infl, t[:12]), t)
                defaults(v)
    finally:
        side.close()
    return out


def run_side(side_name, full, refs):
    side = PythonSide() if side_name == "python" else HostSide()
    result = {"side": side_name, "engines": {}, "unavailable": []}
    for ref in refs:
        got = play(side, ref, full == ref.kind or full == "all")
        if got is None:
            result["unavailable"].append(ref.label)
            continue
        result["engines"].setdefault(ref.label, {}).update(got)
        print("%-7s %-9s %3d voices" % (side_name, ref.label,
                                         len(got)), file=sys.stderr)
    return result


# ---- diffing ---------------------------------------------------------------------

def diff(a, b, label_a, label_b, limit=15):
    """-> disagreements between two sides' results, printing the first few."""
    bad = 0
    shown = 0
    for eng, voices in sorted(a["engines"].items()):
        if eng not in b["engines"]:
            continue
        for vkey, steps in sorted(voices.items()):
            other = b["engines"][eng].get(vkey)
            if other is None:
                bad += 1
                print("MISSING %s %s on %s" % (eng, vkey, label_b))
                continue
            for i, step in enumerate(steps):
                o = other[i] if i < len(other) else None
                # Compare the PCM (length and hash); the prepared-text hash is
                # advisory and printed when the PCM differs.
                if o is None or step[1:3] != o[1:3]:
                    bad += 1
                    if shown < limit:
                        shown += 1
                        print("DIFFER  %s %s %s" % (eng, vkey, step[0]))
                        print("   %-7s %s" % (label_a, step[1:]))
                        print("   %-7s %s" % (label_b, o[1:] if o else "absent"))
            if len(other) > len(steps):
                bad += 1
                print("EXTRA   %s %s has %d more steps on %s"
                      % (eng, vkey, len(other) - len(steps), label_b))
    return bad


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--side", choices=("python", "host"))
    ap.add_argument("--out")
    ap.add_argument("--full", help="engine to run the full grid for: sp, mtk2, mtk3, pro, all")
    ap.add_argument("--expect", default="", help="engines the host must provide, comma-separated")
    ap.add_argument("--freeze", action="store_true", help="write the Python side as the baseline")
    ap.add_argument("--no-baseline", action="store_true")
    args = ap.parse_args()

    refs = engine_refs()
    if not refs:
        print("no engine data found; run tools/extract_rom.py")
        return 0

    if args.side:
        result = run_side(args.side, args.full, refs)
        with open(args.out, "w", encoding="utf-8") as fh:
            json.dump(result, fh, indent=1, sort_keys=True)
        return 0

    # Two processes, one per side; this one only diffs.
    tmp = tempfile.mkdtemp(prefix="osp-oracle-")
    results = {}
    for side in ("python", "host"):
        out = os.path.join(tmp, side + ".json")
        cmd = [sys.executable, os.path.abspath(__file__), "--side", side, "--out", out]
        if args.full:
            cmd += ["--full", args.full]
        r = subprocess.run(cmd)
        if r.returncode != 0:
            print("%s side failed (%d)" % (side, r.returncode))
            return 1
        with open(out, encoding="utf-8") as fh:
            results[side] = json.load(fh)

    bad = 0
    py, host = results["python"], results["host"]
    ported = sorted(host["engines"])
    print("host side provides: %s" % (", ".join(ported) or "nothing yet"))
    for eng in host["unavailable"]:
        kind = eng.split("/")[0]
        if kind in [e for e in args.expect.split(",") if e]:
            bad += 1
            print("EXPECTED %s on the host, not provided" % eng)

    bad += diff(py, host, "python", "host")

    if args.freeze:
        os.makedirs(os.path.dirname(BASELINE), exist_ok=True)
        frozen = {"note": "Hashes of the smoke script as the 1.2.x-era host rendered "
                          "it, through the Python engines. No engine data. Rewrite "
                          "only with --freeze, deliberately.",
                  "library": os.path.basename(os.environ.get("OSP_HOST_DLL", "")),
                  "engines": py["engines"]}
        with open(BASELINE, "w", encoding="utf-8") as fh:
            json.dump(frozen, fh, indent=1, sort_keys=True)
        print("baseline frozen: %s" % BASELINE)
    elif not args.no_baseline and os.path.isfile(BASELINE):
        with open(BASELINE, encoding="utf-8") as fh:
            base = json.load(fh)
        # The baseline holds the smoke script only; compare the steps it has.
        trimmed = {"engines": {}}
        for eng, voices in py["engines"].items():
            if eng in base["engines"]:
                trimmed["engines"][eng] = dict(
                    (k, v[:len(base["engines"][eng].get(k, []))])
                    for k, v in voices.items())
        b = diff(base, trimmed, "frozen", "python")
        if b:
            print("%d disagreement(s) between the frozen baseline and the Python side"
                  " -- the host core moved, or the reference did" % b)
        bad += b
        if "engines" in host and host["engines"]:
            trimmed_h = {"engines": {}}
            for eng, voices in host["engines"].items():
                if eng in base["engines"]:
                    trimmed_h["engines"][eng] = dict(
                        (k, v[:len(base["engines"][eng].get(k, []))])
                        for k, v in voices.items())
            bad += diff(base, trimmed_h, "frozen", "host")

    total = sum(len(s) for e in py["engines"].values() for s in e.values())
    print("%d renders on the Python side; %d disagreement(s)" % (total, bad))
    return bad


if __name__ == "__main__":
    sys.exit(main())
