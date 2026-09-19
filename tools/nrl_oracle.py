# -*- coding: utf-8 -*-
"""Diff the host's letter-to-sound interpreter against nrl.py, the reference.

`osp_nrl.c` runs the user's `RULZ` and `DICT` tables so that the 1984 engine
can be driven from C; `nrl.py` is what it was ported from and stays the
specification.  This feeds both the same text over a corpus that leans on
every context symbol the notation has, and diffs the output byte for byte:
translation, respelling, and the lone-letter reading.

    py -3 tools/nrl_oracle.py [path/to/osp_host.dll | libosp_host.so]

Exit status is the number of disagreements.  **Needs the user's rule
tables** (`RULZ_1129.bin`, and `DICT_-4048.bin` when present), which are
never in the repository or on a build server -- so unlike the numbers
oracle this one runs here and on the Linux box, not in CI, and exits 0 with
a note when it cannot find them.

As with the numbers oracle, the comparison is on MacRoman-representable
text: the Python sees Unicode and the driver encodes afterwards, the host
sees the bytes.  Respelling keeps unmatched characters, and for a MacRoman
letter whose uppercase has no MacRoman spelling the two sides cannot agree
on what to keep, so respelling is compared directly only on ASCII text and
end to end -- respelled, then translated -- on the rest.
"""
import ctypes
import os
import string
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(ROOT, "addon", "synthDrivers", "_outspoken"))
sys.path.insert(0, HERE)
import nrl                                                    # noqa: E402
import paths                                                  # noqa: E402


def load(path=None):
    """The host library with the NRL calls bound. -> ctypes library"""
    if path is None:
        import osp
        path = osp.DLL
    lib = ctypes.CDLL(path)
    for name in ("osp_nrl_load", "osp_nrl_load_dictionary"):
        f = getattr(lib, name)
        f.argtypes = [ctypes.c_char_p, ctypes.c_int]
        f.restype = ctypes.c_int
    lib.osp_nrl_unload.argtypes = []
    lib.osp_nrl_rule_count.argtypes = [ctypes.c_int]
    lib.osp_nrl_rule_count.restype = ctypes.c_int
    for name in ("osp_nrl_translate", "osp_nrl_respell", "osp_nrl_letter_name"):
        f = getattr(lib, name)
        f.argtypes = [ctypes.c_char_p, ctypes.c_int, ctypes.c_char_p, ctypes.c_int]
        f.restype = ctypes.c_int
    return lib


def _call(fn, text):
    raw = text.encode("mac_roman", "replace")
    cap = len(raw) * 16 + 64
    for _ in range(2):
        buf = ctypes.create_string_buffer(cap)
        n = fn(raw, len(raw), buf, cap)
        if n == -1:
            raise MemoryError("host ran out of memory")
        if n == -2:
            raise RuntimeError("host has no rule table loaded")
        if n <= cap:
            return buf.raw[:n].decode("mac_roman")
        cap = n
    raise RuntimeError("host kept asking for a bigger buffer")


def host_translate(lib, text):
    return _call(lib.osp_nrl_translate, text)


def host_respell(lib, text):
    return _call(lib.osp_nrl_respell, text)


def host_letter_name(lib, text):
    return _call(lib.osp_nrl_letter_name, text)


def representable(text):
    return text.encode("mac_roman", "replace").decode("mac_roman")


def corpus(rules, dictionary):
    """Text that exercises every symbol in the notation."""
    out = []
    # Every focus in the rule file as a word of its own, and with its
    # contexts around it where they are literal.  The file's own whole-word
    # assertions are among these.
    for b in rules.buckets:
        for left, focus, right, _phon in b:
            out.append(focus)
            out.append(left.strip("#:^+.&@%? ") + focus + right.strip("#:^+.&@%? "))
    if dictionary is not None:
        for b in dictionary.buckets:
            for _left, focus, _right, repl in b:
                out.append(focus)
                out.append(repl)
                out.append("the %s button" % focus.lower())
    # Every letter, upper and lower, alone and doubled; every digit.
    for c in string.ascii_lowercase:
        out += [c, c.upper(), c + c, c + "."]
    out += list(string.digits) + ["2026", "1st", "2nd", "3rd", "4th", "10th",
                                  "3.14", "12:30", "1,234", "50%", "$5", "#1"]
    # Every bigram, which is what stresses the context symbols; and a spread
    # of trigrams, so the ':' and '#' runs see three letters.
    letters = string.ascii_lowercase
    out += [a + b for a in letters for b in letters]
    tri = [a + b + c for a in letters for b in letters for c in letters]
    out += tri[::7]
    # Words the tests name, and words the rules were written for.
    out += ["select", "complete", "macintalk", "softvoice", "amiga", "search",
            "dialog", "cancel", "into", "in", "a", "the", "are", "aren't",
            "able", "table", "carry", "arrow", "hello", "world", "welcome",
            "outspoken", "berkeley", "systems", "nineteen", "eighty", "four",
            "through", "thought", "though", "tough", "cough", "enough",
            "rule", "ruler", "nation", "station", "special", "ocean", "sugar",
            "measure", "pleasure", "vision", "question", "onion", "union",
            "singing", "ringer", "finger", "anger", "danger", "changed",
            "phoneme", "phonemes", "character", "chemistry", "machine",
            "yes", "yellow", "yacht", "gym", "rhythm", "why", "quick", "queue",
            "knight", "know", "gnome", "psalm", "pneumonia", "wrist", "write",
            "one", "once", "only", "onto", "unto", "upon", "over", "under",
            "button", "checkbox", "list", "link", "edit", "combo", "menu",
            "unavailable", "selected", "not", "checked", "collapsed", "expanded"]
    # Suffix handling: every suffix on stems where it matters.
    for stem in ("select", "complet", "lov", "hat", "mak", "walk", "talk", "danc"):
        for suf in ("", "e", "ed", "es", "er", "ing", "ely"):
            out.append(stem + suf)
    # Punctuation, apostrophes, hyphens, spacing, case.
    out += [".", ",", ";", ":", "!", "?", "'", '"', "-", "(", ")", "...",
            "it's", "don't", "won't", "o'brien", "rock'n'roll", "'quoted'",
            "well-known", "re-enter", "x-ray", "end.", "end...", "wait!",
            "really?", "yes, no; maybe: so", "  two  spaces  ", "\ttab\t",
            "new\nline", "UPPER CASE", "MiXeD cAsE", "MacinTalk", "outSPOKEN",
            "NVDA", "SAPI5", "H2O", "mp3", "utf8", "COM1", "3M", "",
            " ", "   ", "a b c", "A. B. C.", "e.g.", "i.e.", "U.S.A.",
            "http://example.com/path?q=1&b=2", "file.txt", "user@example.com",
            "C:/Users/Tomi", "1 of 3", "page 2 of 10", "OK", "Cancel",
            "Save As", "Untitled - Notepad", "Search results", "link visited",
            "twenty five", "one thousand two hundred thirty four point five zero",
            "veinticinco", "minus seven", "large number"]
    # MacRoman: accented letters, the three that uppercase to two ASCII
    # letters, symbols -- alone, and between letters, and every high byte.
    out += ["caf\xe9", "na\xefve", "stra\xdfe", "\xdfa", "a\xdf", "\ufb01le",
            "\ufb02ow", "\ufb01", "\ufb02", "r\xe9sum\xe9", "\xc6r\xf8", "\xbfQu\xe9?",
            "5\xb0", "\xa35", "\u2022bullet", "\xb5s", "\u03c9"]
    for b in range(0x80, 0x100):
        ch = bytes([b]).decode("mac_roman")
        out += [ch, "a" + ch + "b", "the" + ch + " end", ch + "s", "s" + ch]
    return out


def run(lib, verbose=True, limit=20):
    """Every case, every call. -> number of disagreements"""
    rulz = paths.find("RULZ_1129.bin")
    if not rulz:
        if verbose:
            print("RULZ not present; nothing to compare (run tools/extract_rom.py)")
        return 0
    rules = nrl.Rules(open(rulz, "rb").read())
    data = open(rulz, "rb").read()
    if lib.osp_nrl_load(data, len(data)) != 0:
        print("host refused RULZ")
        return 1
    dictionary = None
    dpath = paths.find("DICT_-4048.bin")
    if dpath:
        ddata = open(dpath, "rb").read()
        dictionary = nrl.Dictionary(ddata)
        if lib.osp_nrl_load_dictionary(ddata, len(ddata)) != 0:
            print("host refused DICT")
            return 1

    bad = checked = 0

    def diff(kind, text, want, got):
        nonlocal bad
        bad += 1
        if verbose and bad <= limit:
            print("DIFFER  [%s] %r" % (kind, text[:60]))
            print("   python: %r" % want[:200])
            print("   host  : %r" % got[:200])

    n_rules = sum(len(b) for b in rules.buckets)
    if lib.osp_nrl_rule_count(0) != n_rules:
        diff("rule count", "RULZ", str(n_rules), str(lib.osp_nrl_rule_count(0)))
    if dictionary is not None:
        n_dict = sum(len(b) for b in dictionary.buckets)
        if lib.osp_nrl_rule_count(1) != n_dict:
            diff("rule count", "DICT", str(n_dict), str(lib.osp_nrl_rule_count(1)))

    for text in corpus(rules, dictionary):
        rep = representable(text)
        ascii_only = all(ord(c) < 128 for c in rep)

        want = nrl.translate(rep, rules)
        got = host_translate(lib, text)
        checked += 1
        if got != want:
            diff("translate", text, want, got)

        if dictionary is not None:
            if ascii_only:
                want = nrl.respell(rep, dictionary)
                got = host_respell(lib, text)
                checked += 1
                if got != want:
                    diff("respell", text, want, got)
            want = nrl.translate(nrl.respell(rep, dictionary), rules)
            got = host_translate(lib, host_respell(lib, text))
            checked += 1
            if got != want:
                diff("respell+translate", text, want, got)

        if len(rep) == 1:
            want = nrl.letter_name(rep, rules)
            got = host_letter_name(lib, text)
            checked += 1
            if got != want:
                diff("letter_name", text, want, got)

    lib.osp_nrl_unload()
    if verbose:
        print("%d cases, %d disagreement(s)" % (checked, bad))
    return bad


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else None
    try:
        lib = load(path)
    except OSError as e:
        raise SystemExit("cannot load the host library (%s); run build.sh "
                         "or build_linux.sh" % e)
    return run(lib)


if __name__ == "__main__":
    sys.exit(main())
