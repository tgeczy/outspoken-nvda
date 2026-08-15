# -*- coding: utf-8 -*-
"""Tell the user, once, that the ROM folder is empty.

The engine is not ours to ship, so a fresh install cannot speak until the user
supplies it. That needs saying somewhere, and a settings panel would be a lot
of scaffolding around one folder -- so it is one dialog, on first run only,
with a button that opens the folder.

The check runs off the main thread after a short delay: NVDA is still starting
up when global plugins load, and a modal dialog thrown at that moment is a good
way to make a screen reader look broken.
"""
import os
import threading

import globalPluginHandler
import globalVars
import gui
import wx
from logHandler import log

import sys
_ADDON = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
_ENGINE_DIR = os.path.join(_ADDON, "synthDrivers", "_outspoken")
if _ENGINE_DIR not in sys.path:
    sys.path.insert(0, _ENGINE_DIR)

import rom                                                    # noqa: E402

#: A marker beside the ROM folder, so the dialog appears once rather than at
#: every start-up. Deleting it asks again, which is the obvious repair.
_MARKER = "asked-once"

_MESSAGE = (
    "ROM for outSPOKEN is not present.\n\n"
    "Press OK to open the empty ROM folder in Windows Explorer, where you can "
    "paste a working outSPOKEN ROM."
)


class GlobalPlugin(globalPluginHandler.GlobalPlugin):

    def __init__(self):
        super().__init__()
        if globalVars.appArgs.secure:
            return
        threading.Timer(6.0, self._check).start()

    def _check(self):
        try:
            found, _missing = rom.find()
            if all(n in found for n in rom.REQUIRED):
                return
            folder = rom.config_dir()
            marker = os.path.join(folder, _MARKER)
            if os.path.exists(marker):
                return
            os.makedirs(folder, exist_ok=True)
            with open(marker, "w") as f:
                f.write("Delete this file to be asked about the ROM again.\n")
            wx.CallAfter(self._ask, folder)
        except Exception:
            log.error("outSPOKEN: ROM check failed", exc_info=True)

    def _ask(self, folder):
        try:
            if gui.messageBox(_MESSAGE, "outSPOKEN",
                              wx.OK | wx.CANCEL | wx.ICON_INFORMATION) == wx.OK:
                os.startfile(folder)
        except Exception:
            log.error("outSPOKEN: could not open the ROM folder", exc_info=True)
