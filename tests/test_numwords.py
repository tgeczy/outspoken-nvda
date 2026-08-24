# -*- coding: utf-8 -*-
"""The number reader.

Numbers are the one thing every one of these engines gets wrong in the same
way: `RULZ` bucket 26 holds the ten digit names and nothing else, so `30` is
"three zero".  Fixing that is an addition in front of the rules, so it is
testable without any engine data at all -- no ROM, no emulator, no audio.
"""
import os
import sys

import pytest

sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(
    os.path.abspath(__file__))), "addon", "synthDrivers", "_outspoken"))
import numwords                                               # noqa: E402


@pytest.mark.parametrize("n,want", [
    (0, "zero"), (7, "seven"), (13, "thirteen"), (20, "twenty"),
    (30, "thirty"), (42, "forty two"), (99, "ninety nine"),
    (100, "one hundred"), (101, "one hundred one"),
    (115, "one hundred fifteen"), (999, "nine hundred ninety nine"),
    (1000, "one thousand"),
    (1234, "one thousand two hundred thirty four"),
    (1000000, "one million"),
    (2000000000, "two billion"),
    (10 ** 12, "one trillion"),
    (10 ** 15, "one quadrillion"),
    (10 ** 18, "large number"),
    (-5, "minus five"),
    (-10 ** 18, "minus large number"),
])
def test_cardinal(n, want):
    assert numwords.cardinal(n) == want


@pytest.mark.parametrize("n,want", [
    (1, "first"), (2, "second"), (3, "third"), (4, "fourth"),
    (5, "fifth"), (8, "eighth"), (9, "ninth"), (12, "twelfth"),
    (20, "twentieth"), (21, "twenty first"), (30, "thirtieth"),
])
def test_ordinal(n, want):
    assert numwords.ordinal(n) == want


@pytest.mark.parametrize("text,want", [
    # The cases Tomi reported: multi-digit numbers were unspeakable.
    ("30", "thirty"),
    ("30 40 50", "thirty forty fifty"),
    ("25", "twenty five"),
    ("chapter 12", "chapter twelve"),
    # Grouping and decimals.
    ("1,234", "one thousand two hundred thirty four"),
    ("1,000,000,000,000,000", "one quadrillion"),
    ("3.14", "three point one four"),
    ("1,234.50", "one thousand two hundred thirty four point five zero"),
    # A bare leading dot is not a number: "that is it. 5 items"
    # would otherwise lose its sentence break.
    (".5", ".5"),
    # Signs and ordinals.
    ("-7 items", "minus seven items"),
    ("the 3rd", "the third"),
    ("the 21st", "the twenty first"),
    # Nothing to do.
    ("no digits here", "no digits here"),
    ("", ""),
])
def test_normalise(text, want):
    assert numwords.normalise(text) == want


def test_spell_out_is_the_engines_own_behaviour():
    assert numwords.normalise("30", spell_out=True) == "three zero"
    assert numwords.normalise("2024", spell_out=True) == "two zero two four"
    # ...but an ordinal still becomes a word, because "three r d" helps nobody.
    assert numwords.normalise("3rd", spell_out=True) == "third"


def test_a_version_string_is_left_alone():
    """`2.1` is a number; `v2.1.3` is an identifier and must not be mangled.

    The lookarounds in the pattern are what keep the dotted form out, and a
    regression here would be heard rather than seen.
    """
    assert numwords.normalise("v2.1.3") == "v2.1.3"
    assert numwords.normalise("192.168.0.1") == "192.168.0.1"


def test_word_boundaries():
    """A digit glued to letters is part of a token, not a number."""
    for t in ("mp3", "utf8", "x2go", "COM1"):
        assert numwords.normalise(t) == t


def test_read_year_is_never_automatic():
    """Because `1984 items` is not `nineteen eighty four items`."""
    assert numwords.normalise("1984") == \
        "one thousand nine hundred eighty four"
    assert numwords.read_year(1984) == "nineteen eighty four"
    assert numwords.read_year(2000) == "two thousand"
    assert numwords.read_year(1905) == "nineteen oh five"


def test_scale_names_end_at_quadrillion():
    """One more digit past 100 trillion is a quadrillion, as Eloquence says
    it; from 10^18 up it is "large number", because admitting the size says
    more than a stack of scale words nobody can verify by ear.
    """
    assert numwords.cardinal(10 ** 15) == "one quadrillion"
    assert numwords.cardinal(10 ** 18 - 1).startswith(
        "nine hundred ninety nine quadrillion")
    assert numwords.cardinal(10 ** 18) == "large number"
    # The ordinal collapses whole, never to "large numberth".
    assert numwords.ordinal(10 ** 18) == "large number"


def test_a_five_thousand_digit_run_does_not_raise():
    """Python refuses `int()` past 4300 digits, so the size decision is made
    on the string.  Before it was, a long enough digit run in any text NVDA
    read -- a log, a dump -- was a crashed utterance, not a number.
    """
    run = "9" * 5000
    assert numwords.normalise(run) == "large number"
    assert numwords.normalise(run + "th") == "large number"
    # Spelling out is exempt on purpose: digit by digit never needed `int()`
    # and a user who asked for digits should get all of them.
    spelled = numwords.normalise(run, spell_out=True)
    assert spelled.split().count("nine") == 5000
