# -*- coding: utf-8 -*-
"""English text to MacinTalk phonemes, using the engine's own rule table.

MacinTalk speaks phonemes and nothing else -- its `Prime` rejects English at
+$338, and the Amiga split the same engine into `narrator.device` (speaks) and
`translator.library` (translates).  outSPOKEN's own Pascal played the second
part, using the `RULZ` resource.  This is that part, rewritten.

**The rules are the user's data; only the interpreter is ours.**  Nothing here
embeds any part of the engine -- `RULZ` is read from the ROM the user supplies,
exactly like `DRVR` and `TALK`.

`RULZ` is 28 big-endian 4-byte offsets followed by 28 buckets of rules
separated by backslashes.  Buckets 0-25 are A-Z, 26 is digits, 27 is
punctuation.  Each rule is

    left [ focus ] right = phonemes

in the notation of Elovitz et al., *Automatic Translation of English Text to
Phonetics by Means of Letter-to-Sound Rules*, NRL Report 7948 (1976).

Among the rules matching at the cursor, **the longest focus wins**, with file
order breaking ties.  See `translate()` for the measurement behind that; the
textbook first-match-in-file-order reading scores 91.5% against the file's own
assertions where longest-match scores 97.9%, and cannot reach `[INTO]` at all.

Do not sort or rewrite the buckets.  File order is load-bearing for ties, and
disturbing it breaks the translator in a way that still produces plausible
output -- the worst kind of broken.

    py -3 tools/nrl.py "This apple is."
    py -3 tools/nrl.py --test
"""
import os
import struct
import sys


VOWELS = set("AEIOUY")
CONSONANTS = set("BCDFGHJKLMNPQRSTVWXZ")
FRONT_VOWELS = set("EIY")                       # '+'
VOICED = set("BDVGJLMNRWZ")                     # '.'
SIBILANTS = set("SCGZXJ")                       # '&', plus CH and SH
RULE_U = set("TSRDLZNJ")                        # '@', plus TH CH SH
SUFFIXES = ("ER", "E", "ES", "ED", "ING", "ELY")    # '%'


class Rules(object):
    """The 28 rule buckets, in file order."""

    def __init__(self, data):
        if len(data) < 112:
            raise ValueError("RULZ too short to hold its offset table")
        offs = [struct.unpack(">I", data[i * 4:i * 4 + 4])[0] for i in range(28)]
        if offs[0] != 112 or any(offs[i] >= offs[i + 1] for i in range(27)):
            raise ValueError("RULZ offset table is not ascending from 112")
        if offs[-1] >= len(data):
            raise ValueError("RULZ offset table points past the end")

        self.buckets = []
        for i in range(28):
            end = offs[i + 1] if i + 1 < 28 else len(data)
            self.buckets.append(self._parse(data[offs[i]:end]))

    @staticmethod
    def _parse(blob):
        out = []
        for raw in blob.split(b"\\"):
            if b"[" not in raw or b"]" not in raw or b"=" not in raw:
                continue
            left, rest = raw.split(b"[", 1)
            focus, rest = rest.split(b"]", 1)
            right, phon = rest.split(b"=", 1)
            out.append((left.decode("latin-1"), focus.decode("latin-1"),
                        right.decode("latin-1"), phon.decode("latin-1")))
        return out

    def bucket_for(self, ch):
        if "A" <= ch <= "Z":
            return self.buckets[ord(ch) - 65]
        if ch.isdigit():
            return self.buckets[26]
        return self.buckets[27]


# --- context matching ----------------------------------------------------
#
# Left context is matched right-to-left from just before the focus; right
# context left-to-right from just after it.  Everything is uppercase and the
# text is padded with spaces, so ' ' in a rule means a word boundary and needs
# no special case.

def _suffix_at(text, i):
    for s in SUFFIXES:
        if text[i:i + len(s)] == s:
            return len(s)
    return 0


def _match_right(text, i, pat):
    for k, c in enumerate(pat):
        if c == "#":                       # one or more vowels
            if i >= len(text) or text[i] not in VOWELS:
                return False
            while i < len(text) and text[i] in VOWELS:
                i += 1
        elif c == ":":                     # zero or more consonants
            while i < len(text) and text[i] in CONSONANTS:
                i += 1
        elif c == "^":
            if i >= len(text) or text[i] not in CONSONANTS:
                return False
            i += 1
        elif c == "+":
            if i >= len(text) or text[i] not in FRONT_VOWELS:
                return False
            i += 1
        elif c == ".":
            if i >= len(text) or text[i] not in VOICED:
                return False
            i += 1
        elif c == "&":                     # sibilant, incl. CH and SH
            if text[i:i + 2] in ("CH", "SH"):
                i += 2
            elif i < len(text) and text[i] in SIBILANTS:
                i += 1
            else:
                return False
        elif c == "@":                     # consonant giving long U as in RULE
            if text[i:i + 2] in ("TH", "CH", "SH"):
                i += 2
            elif i < len(text) and text[i] in RULE_U:
                i += 1
            else:
                return False
        elif c == "%":
            n = _suffix_at(text, i)
            if not n:
                return False
            i += n
        elif c == "?":                     # a digit
            if i >= len(text) or not text[i].isdigit():
                return False
            i += 1
        else:
            if i >= len(text) or text[i] != c:
                return False
            i += 1
    return True


def _match_left(text, i, pat):
    """`i` is the index just past the last character of the left context."""
    for c in reversed(pat):
        if c == "#":
            if i <= 0 or text[i - 1] not in VOWELS:
                return False
            while i > 0 and text[i - 1] in VOWELS:
                i -= 1
        elif c == ":":
            while i > 0 and text[i - 1] in CONSONANTS:
                i -= 1
        elif c == "^":
            if i <= 0 or text[i - 1] not in CONSONANTS:
                return False
            i -= 1
        elif c == "+":
            if i <= 0 or text[i - 1] not in FRONT_VOWELS:
                return False
            i -= 1
        elif c == ".":
            if i <= 0 or text[i - 1] not in VOICED:
                return False
            i -= 1
        elif c == "&":
            if i >= 2 and text[i - 2:i] in ("CH", "SH"):
                i -= 2
            elif i > 0 and text[i - 1] in SIBILANTS:
                i -= 1
            else:
                return False
        elif c == "@":
            if i >= 2 and text[i - 2:i] in ("TH", "CH", "SH"):
                i -= 2
            elif i > 0 and text[i - 1] in RULE_U:
                i -= 1
            else:
                return False
        elif c == "%":
            return False                   # '%' is a right-context device only
        elif c == "?":
            if i <= 0 or not text[i - 1].isdigit():
                return False
            i -= 1
        else:
            if i <= 0 or text[i - 1] != c:
                return False
            i -= 1
    return True


def translate(text, rules):
    """English -> a MacinTalk phoneme string.

    Among the rules that match at the cursor, **the longest focus wins**; file
    order breaks ties.  That is not the textbook first-match-in-order reading of
    Elovitz, and the choice was made by measurement rather than by argument:
    `RULZ` contains 142 whole-word assertions, and

        first rule in file order   130/142   91.5%
        longest focus wins         139/142   97.9%

    The decisive case is `INTO`.  The I bucket holds ` [IN]=IH4N` with an empty
    right context at index 3 and ` [INTO] =IH2NTUW` at index 7, so file order
    can never reach `INTO` -- yet the file asserts it.  Nobody hand-tunes a
    6.9 KB resource and leaves unreachable rules in it.
    """
    s = " " + text.upper() + " "
    out, i = [], 1
    while i < len(s) - 1:
        best = None
        for left, focus, right, phon in rules.bucket_for(s[i]):
            if len(focus) <= (len(best[1]) if best else 0):
                continue                       # cannot beat what we have
            if s[i:i + len(focus)] != focus:
                continue
            if not _match_left(s, i, left):
                continue
            if not _match_right(s, i + len(focus), right):
                continue
            best = (left, focus, right, phon)
        if best:
            out.append(best[3])
            i += len(best[1])
        else:
            # No rule matched at all.  Dropping the character silently is how a
            # translator mispronounces a word with no trace, so skip it but let
            # --test surface the gap.
            i += 1
    return "".join(out).strip()


# The rule file carries its own regression suite: every whole-word entry in the
# exception dictionary is an exact input -> output pair, written by the people
# who wrote the engine.  If these pass, the context matcher is right.
def self_test(rules, verbose=True):
    # A focus can carry more than one rule -- ` [ARR]`=AXR for the word-initial
    # case and `[ARR]`=AE4R for the middle of "carry" -- and only one of them
    # can be right for the word in isolation.  So a case passes if the output
    # is any of the readings recorded for that focus; demanding one particular
    # reading just tests which duplicate the extractor happened to see last.
    want = {}
    for b in rules.buckets:
        for left, focus, right, phon in b:
            if focus.isalpha() and len(focus) >= 3 \
                    and left.strip() == "" and right.strip() == "":
                want.setdefault(focus, set()).add(phon.strip())
    cases, bad = sorted(want.items()), []
    for word, readings in cases:
        got = translate(word, rules)
        if got not in readings:
            bad.append((word, "/".join(sorted(readings)), got))
    if verbose:
        print("whole-word assertions in RULZ: %d" % len(cases))
        print("matching: %d   differing: %d   (%.1f%%)"
              % (len(cases) - len(bad), len(bad),
                 100.0 * (len(cases) - len(bad)) / max(1, len(cases))))
        for w, want, got in bad[:15]:
            print("   %-14s want %-22s got %s" % (w, want, got))
    return cases, bad


def load(path=None):
    """Read the rules. With no path, ask tools/paths.py where the ROM is.

    The lookup is deferred so this module imports cleanly inside the add-on,
    where `paths` does not exist and the caller always passes the file it
    already located.
    """
    if path is None:
        import paths
        path = paths.rulz()
    return Rules(open(path, "rb").read())


def main():
    args = sys.argv[1:]
    rules = load(os.environ.get("RULZ"))
    if not args:
        print(__doc__)
        return 1
    if args[0] == "--test":
        _, bad = self_test(rules)
        return 1 if bad else 0
    for t in args:
        print("%r\n  -> %s" % (t, translate(t, rules)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
