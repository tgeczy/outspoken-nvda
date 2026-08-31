# -*- coding: utf-8 -*-
"""The log stays quiet when everything works.

From 2026-08-17 to 1.2.2 every start wrote the whole engine report -- every
folder searched, every file looked for -- at INFO, the level a stable NVDA
shows by default.  Two weeks and eight releases of it, until Panthera drew
the complaint that it looked like debugging left on and both add-ons were
fixed in the same hour.  A working engine gets one line at INFO, no more;
the report belongs to DEBUG.  This is the guard that makes "never again"
mechanical rather than remembered.
"""
import os
import sys
import types

import pytest

ADDON = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(
    __file__))), "addon")


def _stub_nvda():
    """Enough of NVDA for the plugin to import.  Nothing here is exercised."""
    if "globalPluginHandler" in sys.modules:
        return
    gph = types.ModuleType("globalPluginHandler")
    gph.GlobalPlugin = type("GlobalPlugin", (object,), {
        "__init__": lambda self: None, "terminate": lambda self: None})
    sys.modules["globalPluginHandler"] = gph
    gv = types.ModuleType("globalVars")
    gv.appArgs = types.SimpleNamespace(secure=False, configPath=None)
    sys.modules["globalVars"] = gv
    g = types.ModuleType("gui")
    g.messageBox = lambda *a, **k: None
    g.mainFrame = None
    sys.modules["gui"] = g
    wx = types.ModuleType("wx")
    for n in ("OK", "CANCEL", "YES", "NO", "YES_NO", "ID_ANY", "EVT_MENU",
              "ICON_INFORMATION", "ICON_WARNING"):
        setattr(wx, n, len(n))
    wx.CallAfter = wx.CallLater = lambda *a, **k: None
    sys.modules["wx"] = wx
    lh = types.ModuleType("logHandler")
    lh.log = types.SimpleNamespace()
    sys.modules["logHandler"] = lh


class _RecordingLog(object):
    def __init__(self):
        self.calls = []

    def __getattr__(self, level):
        def record(message, *a, **k):
            self.calls.append((level, message))
        return record


@pytest.fixture(scope="module")
def plugin():
    _stub_nvda()
    sys.path.insert(0, os.path.join(ADDON, "synthDrivers", "_outspoken"))
    sys.path.insert(0, os.path.join(ADDON, "globalPlugins"))
    import outspokenRom
    return outspokenRom


def test_a_ready_engine_logs_one_short_line_at_info(plugin, monkeypatch):
    rec = _RecordingLog()
    monkeypatch.setattr(plugin, "log", rec)
    monkeypatch.setattr(plugin.rom, "explain", lambda: (True, [
        "searched:", "  C:\somewhere exists", "  DRVR_1030.bin found",
        "runnable engines: MacinTalk 1, MacinTalk Pro"]))
    plugin.GlobalPlugin._check(None)
    infos = [m for level, m in rec.calls if level == "info"]
    assert len(infos) == 1, infos
    assert "\n" not in infos[0], "a multi-line report at INFO is the bug"
    assert "ready" in infos[0]
    # The report is not lost, only demoted.
    assert any(level == "debug" and "\n" in m for level, m in rec.calls)


def test_the_full_report_is_only_ever_at_info_when_not_ready(plugin):
    """Source-level, because the not-ready path goes on to draw dialogs.

    Every `log.info(` in the plugin that carries a joined multi-line report
    must be the NOT-ready one or the on-demand Tools-menu one.
    """
    with open(plugin.__file__, encoding="utf-8") as f:
        src = f.read()
    at = 0
    offenders = []
    while True:
        at = src.find("log.info(", at)
        if at < 0:
            break
        call = src[at:src.find(")", at) + 1]
        if "\n  %s" in call and "NOT ready" not in call \
                and "Tools menu" not in call:
            offenders.append(call)
        at += 1
    assert not offenders, offenders
