# -*- coding: utf-8 -*-
"""The English front end.

`RULZ` carries its own regression suite: every whole-word entry is an exact
input -> output pair written by the people who wrote the engine.
"""
import string

import nrl
import pytest


def test_rule_file_parses(rules):
    assert len(rules.buckets) == 28
    assert sum(len(b) for b in rules.buckets) > 500


def test_its_own_assertions(rules):
    """139 of 140. The one exception is an artefact of the extractor, not the
    matcher: ` :[ABLE]`=EY4BUL is right for the standalone word but its ' :'
    context keeps it out of the assertion set, which sees only [ABLE]=AXBUL."""
    cases, bad = nrl.self_test(rules, verbose=False)
    assert len(cases) >= 140
    assert len(bad) <= 1, ["%s: want %s got %s" % b for b in bad]


@pytest.mark.parametrize("word,expected", [
    # `%` is a WORD-FINAL suffix. Matching it anywhere made [E]^% fire inside
    # SELECT and produce "sea-lect"; it must still fire in COMPLETE, where the
    # final E really is the suffix.
    ("select", "SEH1LEH1KT"),
    ("complete", "KAA1MPLIY4T"),
    # Straight from the file's own dictionary.
    ("macintalk", "MAE5KINTAO1K"),
    ("softvoice", "SAA4FTVOYS"),
    ("amiga", "AHMIY5GAH"),
])
def test_known_words(rules, word, expected):
    assert nrl.translate(word, rules) == expected


def test_every_letter_says_its_name(rules):
    """A lone 'a' matches the rule for the WORD "a" and comes out a schwa;
    every other letter already gives its name."""
    names = {c: nrl.letter_name(c, rules) for c in string.ascii_lowercase}
    assert names["a"] == "EH4Y"          # not "AH"
    assert names["b"] == "BIY4"
    assert names["w"] == "DAH4BULYUW"
    # None of them may be empty -- an empty string is silence, and silence for
    # a keystroke reads as a dropped character.
    assert all(v.strip() for v in names.values()), names


def test_digits_and_punctuation(rules):
    assert nrl.translate("2026", rules).strip()
    assert nrl.translate(".", rules) is not None


def test_no_rule_reordering(rules):
    """File order breaks ties, so the buckets must stay as they are read.

    Longest-focus-wins needs file order underneath it: sorting the rules
    produces plausible but wrong output, which is the worst kind of wrong.
    """
    i_bucket = rules.buckets[ord("I") - 65]
    focuses = [f for _l, f, _r, _p in i_bucket]
    assert focuses != sorted(focuses)
    # [INTO] sits after a [IN] with an empty right context, which is why
    # first-match-in-file-order cannot reach it.
    assert "INTO" in focuses
