# -*- coding: utf-8 -*-
"""The host's number rules agree with numwords.py, byte for byte.

`numwords.py` is the specification and `osp_numbers.c` the implementation
every front end now shares; `tools/numbers_oracle.py` is the corpus and the
diff.  This wraps it so the Windows suite runs it against the DLL here, the
way the Linux workflow runs it against the .so it just built.  Needs the
built host and no engine data; skips when the host is absent.
"""
import os
import sys

import pytest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
sys.path.insert(0, os.path.join(ROOT, "addon", "synthDrivers", "_outspoken"))

import numbers_oracle                                         # noqa: E402


@pytest.fixture(scope="module")
def lib():
    import osp
    if not os.path.isfile(osp.DLL):
        pytest.skip("host library not built; run build.sh")
    return numbers_oracle.load()


def test_the_host_agrees_with_numwords_on_the_whole_corpus(lib):
    bad = numbers_oracle.run(lib, verbose=True)
    assert bad == 0, "%d disagreement(s) between osp_numbers.c and numwords.py" % bad


@pytest.mark.parametrize("text,spell_out,lang", [
    ("you owe 1,234.50", False, "en"),
    ("the 1,234,567th", False, "en"),           # the reproduced quirk
    ("tienes 25 mensajes", False, "es"),
    ("2024", True, "es"),
    ("9" * 5000, False, "en"),
    ("9" * 5000, True, "en"),
])
def test_named_cases_match(lib, text, spell_out, lang):
    """A handful by name, so a failure here reads without the corpus."""
    assert numbers_oracle.host_normalise(lib, text, spell_out, lang) == \
        numbers_oracle.reference(text, spell_out, lang)


def test_a_small_buffer_asks_for_a_bigger_one(lib):
    """The contract: the return value is the size needed, cut or not."""
    import ctypes
    raw = b"1234567"
    small = ctypes.create_string_buffer(4)
    need = lib.osp_numbers(raw, len(raw), 0, 0, small, 4)
    want = numbers_oracle.reference("1234567").encode("mac_roman")
    assert need == len(want)
    assert small.raw[:4] == want[:4]
