# -*- coding: utf-8 -*-
"""The host looks where the driver looks: rom.search_roots(), entry for entry.

`tools/roots_oracle.py` asks both for temporary configuration folders --
plain, with a pointer file, with a pointer file naming a folder already in
the list -- and diffs.  Data-free; needs only the built host.
"""
import os
import sys

import pytest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))

import roots_oracle                                           # noqa: E402


@pytest.fixture(scope="module")
def lib():
    import osp
    if not os.path.isfile(osp.DLL):
        pytest.skip("host library not built; run build.sh")
    return roots_oracle.load()


def test_the_host_searches_the_drivers_roots_in_the_drivers_order(lib):
    assert roots_oracle.run(lib, verbose=True) == 0
