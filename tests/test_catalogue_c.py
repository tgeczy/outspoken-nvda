# -*- coding: utf-8 -*-
"""The host's voice catalogue agrees with the Python, entry for entry.

`voices.py`, the engine modules' `find()` and the driver's `_catalogue`
are the specification; `osp_voices.c` is what the command line, the SAPI
host and Android list voices from.  `tools/catalogue_oracle.py` is the
diff; this wraps it.  Needs the built host and engine data; skips without.
"""
import os
import sys

import pytest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
sys.path.insert(0, os.path.join(ROOT, "addon", "synthDrivers", "_outspoken"))

import catalogue_oracle                                       # noqa: E402


@pytest.fixture(scope="module")
def lib():
    import osp
    import paths
    if not os.path.isfile(osp.DLL):
        pytest.skip("host library not built; run build.sh")
    if not paths.roots():
        pytest.skip("no engine data; run tools/extract_rom.py")
    return catalogue_oracle.load()


def test_the_host_lists_what_the_python_lists(lib):
    bad = catalogue_oracle.run(lib, verbose=True)
    assert bad == 0, "%d disagreement(s) between osp_voices.c and the Python" % bad


def test_an_empty_root_lists_nothing(lib, tmp_path):
    assert lib.osp_catalogue_scan(str(tmp_path).encode("utf-8")) == 0
    assert lib.osp_catalogue_count() == 0
    assert lib.osp_catalogue_skipped_count() == 0
