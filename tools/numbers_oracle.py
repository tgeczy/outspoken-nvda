# -*- coding: utf-8 -*-
"""Diff the host's number rules against the Python they were ported from.

`numwords.py` is the reference and stays the reference.  `osp_numbers.c`
exists so that Linux, Android and the SAPI bridge reach the rules NVDA has
always had without an interpreter in the way -- and a port is only worth
having if it is exact, which a page of hand-written assertions cannot
establish for a tokenizer with two lookarounds, a repeating group and a
backtracking quirk.

So generate a corpus that leans on every edge the pattern has, in both
languages and both styles, run both, and diff byte for byte.

    py -3 tools/numbers_oracle.py [path/to/osp_host.dll | libosp_host.so]

Exit status is the number of disagreements, so it gates a build; the Linux
workflow runs it against the .so it just built, and `tests/test_numbers_c.py`
runs it against the DLL here.  It needs no engine data.

**The comparison is on MacRoman-representable text.**  The Python works on
Unicode and the driver encodes afterwards; the host gets MacRoman.  A letter
the encoding cannot carry is `?` by then and no longer a word character, so
the reference is evaluated on the text after that same encoding -- the same
input, not a transport artefact, is what is being compared.
"""
import ctypes
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(ROOT, "addon", "synthDrivers", "_outspoken"))
import numwords                                               # noqa: E402


def load(path=None):
    """The host library with `osp_numbers` bound. -> ctypes library"""
    if path is None:
        import osp
        path = osp.DLL
    lib = ctypes.CDLL(path)
    lib.osp_numbers.argtypes = [ctypes.c_char_p, ctypes.c_int, ctypes.c_int,
                                ctypes.c_int, ctypes.c_char_p, ctypes.c_int]
    lib.osp_numbers.restype = ctypes.c_int
    return lib


def host_normalise(lib, text, spell_out=False, lang="en"):
    raw = text.encode("mac_roman", "replace")
    cap = len(raw) * 24 + 64
    for _ in range(2):
        buf = ctypes.create_string_buffer(cap)
        n = lib.osp_numbers(raw, len(raw), int(spell_out), int(lang == "es"),
                            buf, cap)
        if n < 0:
            raise MemoryError("osp_numbers ran out of memory")
        if n <= cap:
            return buf.raw[:n].decode("mac_roman")
        cap = n
    raise RuntimeError("osp_numbers kept asking for a bigger buffer")


def reference(text, spell_out=False, lang="en"):
    text = text.encode("mac_roman", "replace").decode("mac_roman")
    return numwords.normalise(text, spell_out=spell_out, lang=lang)


def corpus():
    """Every shape the pattern has an opinion about, in both languages."""
    out = []

    # Digit runs either side of every scale boundary and past the limit.
    for n in range(1, 26):
        out.append("1" * n)
        out.append("9" * n)
        out.append("1" + "0" * (n - 1))
        out.append("2" + "0" * (n - 1))
    # The composition rules by value, English and Spanish alike.
    for v in (0, 1, 2, 7, 10, 11, 13, 15, 16, 19, 20, 21, 22, 25, 29, 30, 31,
              32, 40, 42, 55, 99, 100, 101, 115, 121, 199, 200, 201, 500, 501,
              700, 900, 999, 1000, 1001, 1016, 1100, 1999, 2000, 2016, 2001,
              20000, 21000, 21001, 31000, 41000, 99999, 100000, 100001,
              101000, 121000, 200000, 500000, 999999, 1000000, 1000001,
              1001000, 2000000, 21000000, 31000000, 100000000, 101000000,
              999999999, 10 ** 9, 10 ** 9 + 1, 2500000000, 21 * 10 ** 9,
              10 ** 12, 10 ** 12 + 1, 2 * 10 ** 12, 21 * 10 ** 12,
              10 ** 15, 2 * 10 ** 15, 10 ** 18 - 1, 10 ** 18, 10 ** 19):
        out.append(str(v))
        out.append("-" + str(v))
    # Leading zeros.
    out += ["0", "00", "007", "000", "0000000000000000000", "0" * 30 + "1",
            "0" * 18 + "9" * 18, "0" * 5 + "9" * 19]

    # Already grouped, partly grouped, and grouped wrongly.
    out += ["1,234", "12,345", "123,456", "1,234,567", "12,34,567", "1,2,3",
            "1234,567", "1,2345", "1,234,5678", ",123", "123,", "1,234MB",
            "5KB", "1,,234", "1,234,", "1,234,567,890,123,456,789",
            "1,234,567,890,123,456,789,012", "-1,234", "-1,234,567.89",
            "1,234.5", "1,234.5x", "1,234.5.6", "1,234.", "1,234..5"]

    # Decimals: leading zero, no leading zero, long fractions, trailing dot.
    out += ["0.5", "0.75", "0.05", "00.5", "1.5", "10.7", "3.14", "0.", "1.",
            ".5", "0.0", "1.23456789", "0.000001", "1.5x", "1..2", "1.5.",
            "1.999999999999999999999.2", "3.14159265358979323846264338"]

    # Versions and addresses -- the repeating group the trailing lookahead
    # refuses.
    out += ["0.7.3", "1.2.3", "1.2.3.4", "10.20.30", "v0.7.3", "v2.1.3",
            "version 0.7.3", "192.168.0.1", "10.0.0.1:8080"]

    # Signs, including the ones that are not signs.
    out += ["-1", "--1", "a-1", "1-2", "- 1", "-0.5", "-1st", "- -1", "-,1",
            "-1,234,567", "1234567-", "x-5y", "5-", "-"]

    # Ordinals, every suffix, every case, glued and free.
    for n in ("1", "2", "3", "4", "11", "12", "13", "21", "22", "23", "100",
              "101", "111", "112", "1000", "1000000", "1000000000000",
              "9" * 18, "9" * 19, "0", "05"):
        for suf in ("st", "nd", "rd", "th"):
            out.append(n + suf)
    out += ["3RD", "3Rd", "3rdx", "3rd.", "3rd,", "3rd3", "the 3rd", "x3rd",
            "3 rd", "-3rd", "3.5th", "3,000th", "1,234,567th",
            "the 1,234,567th", "21st century", "4th of July"]

    # The lookarounds, from both sides, with every neighbouring class,
    # including MacRoman letters and MacRoman non-letters.
    for left in ("", " ", "a", "Z", "9", ",", ".", "(", "-", "_", "\xe9",
                 "\xf1", "\xb0", "\xa1", "?"):
        for right in ("", " ", "a", "Z", "9", ",", ".", ")", "%", "_",
                      "\xe9", "\xb0", "th", "ST"):
            out.append(left + "1234567" + right)
            out.append(left + "1.5" + right)
            out.append(left + "0.7.3" + right)
            out.append(left + "1,234" + right)
            out.append(left + "-42" + right)

    # Words with digits in them stay words.
    out += ["mp3", "utf8", "x2go", "COM1", "20ish", "B12", "H2O", "MP3 H2O 20ish B12",
            "3M", "4x4", "7up", "a1b2c3"]

    # Sentences, several numbers at once, and text that must survive.
    out += [
        "30", "30 40 50", "25", "chapter 12", "-7 items", "the 3rd",
        "the 21st", "no digits here", "", " ", "   ", "\t5\n",
        "you owe 1,234.50", "1,000,000,000,000,000", "3.14",
        "it rose to 3222233 units in 1999", "call 5551234567 now",
        "1234567 and 7654321 and 12", "1984", "in 1984 there were 1984 of them",
        "the 1,234,567th of 0.7.3 on 0.5", "1234567 1234567 1234567",
        "tienes 25 mensajes", "son -3.5 grados", "p\xe1gina 1,234",
        "el 3rd intento", "hace 16 grados y son las 21", "a\xf1o 2016",
        "faltan 100 pesos", "faltan 101 pesos", "faltan 1,000,000 pesos",
        "\xa1son 25!", "\xbf25?", "25\xb0", "\xa325", "25\xa2", "5\xd1",
        "Hello there. You owe 1,234.50 dollars on the 3rd, and it is 25 degrees.",
        "Hola. Debes 1,234.50 pesos el 3 de mayo, y hace 25 grados.",
        "a b c 1 2 3 d e f 10 20 30",
    ]

    # A five-thousand-digit run: "large number" as words, all of them as
    # digits, and never a crash.
    out += ["9" * 5000, "9" * 5000 + "th", "1" * 5000 + ".5"]

    # A Unicode letter the encoding cannot carry: both sides see `?`.
    out += ["ж 5", "5ж", "あ5"]
    return out


MODES = (("words", False, "en"), ("digits", True, "en"),
         ("words", False, "es"), ("digits", True, "es"))


def run(lib, verbose=True, limit=20):
    """Every case in every mode. -> number of disagreements"""
    bad = checked = 0
    for text in corpus():
        for label, spell_out, lang in MODES:
            want = reference(text, spell_out, lang)
            got = host_normalise(lib, text, spell_out, lang)
            checked += 1
            if got != want:
                bad += 1
                if verbose and bad <= limit:
                    print("DIFFER  [%s %s] %r" % (lang, label, text[:60]))
                    print("   python: %r" % want[:200])
                    print("   host  : %r" % got[:200])
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
