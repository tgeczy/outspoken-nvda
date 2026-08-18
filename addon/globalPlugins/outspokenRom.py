# -*- coding: utf-8 -*-
"""Tell the user the engine folder is empty, and keep telling them.

The engine is not ours to ship, so a fresh install cannot speak until the user
supplies it. That needs saying somewhere, and a settings panel would be a lot
of scaffolding around one folder -- so it is a dialog with a button that opens
the folder.

**It asks again on every start-up until either the engine is there or the user
says no.** It used to ask exactly once ever, and recorded that it had asked
*before* the dialog was even shown: anyone who dismissed it without reading --
which is most people, for a dialog that arrives six seconds after start-up --
never saw it again and was left with a synthesizer that silently refused to
appear in the list. Repeating a question is a much smaller harm than that, and
"No" is honoured permanently.

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

#: Written only when the user explicitly says "stop asking".
#:
#: This used to be written *before* the dialog appeared, so it showed exactly
#: once in the add-on's life. Anyone who dismissed it without reading -- which
#: is most people, most of the time, for a dialog that arrives six seconds
#: after start-up -- never saw it again and was left with a synthesizer that
#: silently refused to appear. Asking again each start-up is the kinder
#: failure, so long as there is a way to say no and be believed.
_MARKER = "do-not-ask"

_MESSAGE = (
    "MacinTalk has no engine to run yet.\n\n"
    "This add-on ships no part of MacinTalk. You supply it from your own copy, "
    "and until then the synthesizer will not appear in NVDA's list at all.\n\n"
    "Yes  -  open the folder the engine goes in\n"
    "No  -  do not ask again\n"
    "Cancel  -  remind me next time NVDA starts"
)


class GlobalPlugin(globalPluginHandler.GlobalPlugin):

    def __init__(self):
        super().__init__()
        if globalVars.appArgs.secure:
            log.info("outSPOKEN: secure mode, not checking for the engine")
            return
        log.info("outSPOKEN: ROM check armed")
        threading.Timer(6.0, self._check).start()

    def _check(self):
        """Decide whether to ask, and leave a record either way.

        The condition is `rom.engines_present()`, which is the same question
        the synthesizer's own `check()` answers. It used to be a narrower test
        -- only `rom.REQUIRED`, which omits RULZ_1129.bin -- so a user with two
        of MacinTalk 1's three files satisfied the dialog and not the
        synthesizer, and got neither. Two halves disagreeing about the same
        question is how you end up with silence from both.

        Every step logs, because "no dialog" and "no synthesizer" look
        identical from outside: like nothing happening at all.
        """
        try:
            ok, lines = rom.explain()
            log.info("outSPOKEN: engine %s\n  %s"
                     % ("ready" if ok else "NOT ready", "\n  ".join(lines)))
            if ok:
                return
            folder = rom.config_dir()
            if os.path.exists(os.path.join(folder, _MARKER)):
                log.info("outSPOKEN: not asking, %s exists in %s"
                         % (_MARKER, folder))
                return
            os.makedirs(folder, exist_ok=True)
            log.info("outSPOKEN: showing the engine-missing dialog")
            wx.CallAfter(self._ask, folder)
        except Exception:
            log.error("outSPOKEN: ROM check failed", exc_info=True)

    def _ask(self, folder):
        """Ask, and only record a refusal when the user actually gives one.

        The style is `wx.YES_NO | wx.CANCEL`. It was `YES_NO_CANCEL`, which
        wxWidgets has in C++ and wxPython does not, so this raised
        AttributeError every single time and the dialog had never once appeared
        in any release of either add-on. It presented as nothing happening --
        which is also what a missing add-on, a suppressed reminder and a
        too-early timer all look like, so it was blamed on each of those in
        turn. A user's log gave it up in one line.
        """
        try:
            answer = gui.messageBox(_MESSAGE, "MacinTalk",
                                    wx.YES_NO | wx.CANCEL | wx.ICON_INFORMATION)
            if answer == wx.YES:
                os.startfile(folder)
            elif answer == wx.NO:
                with open(os.path.join(folder, _MARKER), "w") as f:
                    f.write("Delete this file to be asked about the engine "
                            "again.\n")
            # wx.CANCEL, and closing the dialog, both leave no marker, so the
            # question comes back next start-up. That is the default on
            # purpose: the alternative is a synthesizer the user cannot find
            # and has no way to be told about.
        except Exception:
            log.error("outSPOKEN: could not open the ROM folder", exc_info=True)
