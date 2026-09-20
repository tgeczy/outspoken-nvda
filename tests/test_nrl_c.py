# -*- coding: utf-8 -*-
"""The host's letter-to-sound interpreter agrees with nrl.py, byte for byte.

`nrl.py` is the specification; `osp_nrl.c` is what the 1984 engine will be
driven through from C.  `tools/nrl_oracle.py` is the corpus and the diff;
this wraps it for the suite.  Needs the built host and the user's `RULZ`
(and `DICT`, when present) -- never in the repository -- so it skips
without them, like every other test that touches the engine's data.
"""
import os
import sys

import pytest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
sys.path.insert(0, os.path.join(ROOT, "addon", "synthDrivers", "_outspoken"))

import nrl_oracle                                             # noqa: E402


@pytest.fixture(scope="module")
def lib():
    import osp
    import paths
    if not os.path.isfile(osp.DLL):
        pytest.skip("host library not built; run build.sh")
    if not paths.find("RULZ_1129.bin"):
        pytest.skip("RULZ not present; run tools/extract_rom.py")
    return nrl_oracle.load()


def test_the_host_agrees_with_nrl_on_the_whole_corpus(lib):
    bad = nrl_oracle.run(lib, verbose=True)
    assert bad == 0, "%d disagreement(s) between osp_nrl.c and nrl.py" % bad


def test_an_unloaded_table_says_so(lib):
    """-2 is "nothing loaded", never an empty answer: an empty phoneme string
    is silence, and silence for a keystroke reads as a dropped character."""
    import ctypes
    lib.osp_nrl_unload()
    buf = ctypes.create_string_buffer(64)
    assert lib.osp_nrl_translate(b"hello", 5, buf, 64) == -2
    assert lib.osp_nrl_respell(b"hello", 5, buf, 64) == -2
    assert lib.osp_nrl_letter_name(b"a", 1, buf, 64) == -2


def test_a_table_that_does_not_parse_is_refused(lib):
    assert lib.osp_nrl_load(b"short", 5) == -1
    junk = b"\x00" * 112 + b"[A]=AH\\"
    assert lib.osp_nrl_load(junk, len(junk)) == -1        # offsets not from 112
