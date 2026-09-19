# -*- coding: utf-8 -*-
"""The host's settings arithmetic agrees with outspoken.py, value for value.

The rate curve, the pitch scale, the 1984 driver's hertz and the 8-to-16
widening with volume folded in -- `tools/settings_oracle.py` runs the driver's
own methods against `osp_settings.c` over every slider value and every offset
NVDA sends.  Pure arithmetic: needs the built host and nothing else.
"""
import os
import sys

import pytest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))

import settings_oracle                                        # noqa: E402


@pytest.fixture(scope="module")
def lib():
    import osp
    if not os.path.isfile(osp.DLL):
        pytest.skip("host library not built; run build.sh")
    return settings_oracle.load()


def test_the_host_agrees_with_the_driver_on_every_slider_value(lib):
    bad = settings_oracle.run(lib, verbose=True)
    assert bad == 0, "%d disagreement(s) between osp_settings.c and outspoken.py" % bad
