# -*- coding: utf-8 -*-
"""No module of ours may share a name with one of NVDA's.

This shipped, and it was the worst failure this project recognises: voices
listed, selectable, and then silent.

A helper was added as `_outspoken/audio.py`. NVDA has its own `source/audio`
package and imports it during start-up, so by the time anything of ours runs,
`sys.modules["audio"]` is already NVDA's -- and `import audio` consults
`sys.modules` before it ever looks at `sys.path`. Putting our directory first
on the path, which the driver does, makes no difference whatever. Every
MacinTalk 3 and MacinTalk Pro utterance then died with

    AttributeError: module 'audio' has no attribute 'Stream'

and the tests could not have caught it, because outside NVDA nothing competes
for the name. So this test does not run the code -- it compares the names.

`rom`, `voices`, `engine`, `osp`, `nrl` and `numwords` are all fine, checked
against a real NVDA source tree rather than against memory. Skips when that
tree is not present, so it never becomes a reason a clone will not test.
"""
import os

import pytest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OURS = os.path.join(ROOT, "addon", "synthDrivers", "_outspoken")

#: Where NVDA's own modules live. A checkout rather than an installation,
#: because the installed copy is inside `library.zip` and this wants names.
NVDA_SOURCES = (
    r"C:\git\nvda-release-2026.1.1\source",
    r"C:\git\nvda-release-tg\source",
)


def _nvda_source():
    for p in NVDA_SOURCES:
        if os.path.isdir(p):
            return p
    return None


def _top_level(folder):
    """-> {importable top-level names} in a source folder."""
    names = set()
    for entry in os.listdir(folder):
        full = os.path.join(folder, entry)
        if entry.endswith(".py") and entry != "__init__.py":
            names.add(entry[:-3])
        elif os.path.isdir(full) and os.path.isfile(
                os.path.join(full, "__init__.py")):
            names.add(entry)
    return names


def _ours():
    return {n[:-3] for n in os.listdir(OURS)
            if n.endswith(".py") and n != "__init__.py"}


def test_our_module_names_are_ours_alone():
    """The whole of it. A name NVDA already uses is a silent voice."""
    src = _nvda_source()
    if src is None:
        pytest.skip("no NVDA source tree to compare against; looked in %s"
                    % ", ".join(NVDA_SOURCES))
    clash = sorted(_ours() & _top_level(src))
    assert not clash, (
        "these would be shadowed by NVDA's own modules, and `sys.modules` is "
        "consulted before `sys.path` so putting our folder first cannot save "
        "them: %s" % ", ".join(clash))


def test_the_helper_that_caused_this_is_still_renamed():
    """Named so the reason survives a rename back.

    `ospaudio` rather than `audio`, and the prefix is the point: it matches
    `osp.py` and `osp_host.dll`, so it reads as ours at a glance.
    """
    ours = _ours()
    assert "ospaudio" in ours
    assert "audio" not in ours


def test_every_module_we_ship_is_importable_on_its_own():
    """A cheap guard on the flat-import layout these modules use.

    They import each other by bare name -- `import osp`, `import voices` --
    which works because the driver puts `_outspoken` on `sys.path` first. That
    is exactly the arrangement the `audio` collision exploited, so it is worth
    a test that each one still resolves.
    """
    import importlib
    import sys
    if OURS not in sys.path:
        sys.path.insert(0, OURS)
    for name in sorted(_ours()):
        # The engine modules pull in ctypes and the host DLL; importing is
        # still safe, since nothing runs until an Engine is constructed.
        importlib.import_module(name)
