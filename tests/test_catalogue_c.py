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


# -- the walk is pruned and bounded -------------------------------------------
#
# The roots include NVDA's whole configuration folder, and under it sit
# every add-on's Python tree and Panthera's generations, gigabytes that can
# never hold this project's engines.  Walking them cost 3-5 s per
# synthesizer load on the Rog Ally (2.0.0).  So the walk skips those names
# and goes no deeper than the layouts need; these prove both, with a real
# engine folder copied where the walk must not look and where it must.

def _data():
    root = os.path.join(os.environ.get("APPDATA", ""), "nvda", "macintalk", "outspoken")
    if not os.path.isdir(os.path.join(root, "macintalk2")) or \
            not os.path.isdir(os.path.join(root, "voices", "Ben")):
        pytest.skip("MacinTalk 2 and Ben are not extracted here")
    return root


def _scan(roots):
    import osp
    entries, _skipped = osp.HostEngine().catalogue(roots)
    return sorted(e["id"] for e in entries)


def _plant(tmp_path, engine_under):
    import shutil
    data = _data()
    shutil.copytree(os.path.join(data, "macintalk2"), os.path.join(tmp_path, *engine_under, "macintalk2"))
    shutil.copytree(os.path.join(data, "voices", "Ben"), os.path.join(tmp_path, "voices", "Ben"))


def test_an_engine_under_a_wrapper_folder_is_found(lib, tmp_path):
    _plant(tmp_path, ("My Backup", "outspoken"))
    assert _scan([str(tmp_path)]) == ["mtk2:Ben"]


@pytest.mark.parametrize("under", [("addons", "somebody"), ("Leopard", "Speech"),
                                   ("outspoken-backups", "2026-09-01"), ("scratchpad",)])
def test_an_engine_where_the_walk_must_not_look_is_not_found(lib, tmp_path, under):
    _plant(tmp_path, under)
    assert _scan([str(tmp_path)]) == []


def test_the_walk_stops_where_the_layouts_end(lib, tmp_path):
    _plant(tmp_path, tuple("d%d" % i for i in range(8)))
    assert _scan([str(tmp_path)]) == []
