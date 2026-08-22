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

#: **One start-up dialog between all the Macintosh speech add-ons, not one
#: each.** They run in the same NVDA process, so the first to get here speaks
#: for the rest by claiming this attribute on `globalVars`.
#:
#: Tested by renaming the shared `macintalk` folder and restarting: three
#: add-ons meant *three* modal dialogs stacked at start-up, each naming its own
#: engine, with nothing in the Tools menu to reach the other two afterwards.
#: For a screen-reader user that is three dialogs to hear and dismiss before
#: NVDA is usable.
#:
#: `dict.setdefault` rather than get-then-set: three `threading.Timer(6.0)`
#: fire within milliseconds of each other, and setdefault is one atomic
#: operation under the GIL where a read followed by a write is two.
#:
#: Suppressing the others is only safe because of the Tools menu entry below --
#: without a way to ask again on purpose, a suppressed dialog is a lost one.
_SESSION_CLAIM = "_macintalkEngineDialogShown"


def _claim_the_startup_dialog(who):
    """-> True if this add-on is the one that should ask this session."""
    return globalVars.__dict__.setdefault(_SESSION_CLAIM, who) == who


#: Written only when the user explicitly says "stop asking".
#:
#: This used to be written *before* the dialog appeared, so it showed exactly
#: once in the add-on's life. Anyone who dismissed it without reading -- which
#: is most people, most of the time, for a dialog that arrives six seconds
#: after start-up -- never saw it again and was left with a synthesizer that
#: silently refused to appear. Asking again each start-up is the kinder
#: failure, so long as there is a way to say no and be believed.
_MARKER = "do-not-ask"

#: **This said the synthesizer would not appear in NVDA's list until an engine
#: was supplied, and that stopped being true when `check` started returning
#: True unconditionally** -- the whole point of that change being that a
#: synthesizer nobody can find is a synthesizer nobody can be told about. The
#: message outlived the behaviour it described by several releases.
_MESSAGE = (
    "MacinTalk has no engine to run yet.\n\n"
    "This add-on ships no part of MacinTalk. You supply it from your own copy. "
    "The synthesizer is listed in NVDA either way, and says what is missing if "
    "you select it before then.\n\n"
    "The extractor is a single Python file you download and run:\n"
    "https://github.com/tgeczy/outspoken-nvda/blob/main/tools/extract_rom.py\n"
    "It needs Python 3.8 or newer installed (tested on 3.13), and the machfs "
    "package for disk images -- py -3 -m pip install machfs\n\n"
    "If you have the Tiger or Leopard speech add-ons too, they share the same "
    "macintalk folder and each has its own entry in NVDA's Tools menu. Only "
    "one of them asks at start-up.\n\n"
    "Yes  -  open the folder the engine goes in\n"
    "No  -  do not ask again\n"
    "Cancel  -  remind me next time NVDA starts"
)


class GlobalPlugin(globalPluginHandler.GlobalPlugin):

    #: Shown in NVDA's Tools menu. Translators: an item in NVDA's Tools menu.
    MENU_LABEL = _("&MacinTalk engine (outSPOKEN)...")
    MENU_HELP = _("Check whether outSPOKEN can find its engine, and open the "
                  "folder it goes in.")

    def __init__(self):
        super().__init__()
        self._menuItem = None
        if globalVars.appArgs.secure:
            log.info("outSPOKEN: secure mode, not checking for the engine")
            return
        self._addMenuItem()
        log.info("outSPOKEN: ROM check armed")
        threading.Timer(6.0, self._check).start()

    def _addMenuItem(self):
        """A way to ask on purpose, which is what makes "do not ask" safe.

        Without this, saying no once meant deleting a file called
        `do-not-ask` by hand to ever see it again -- and there was no route at
        all to "is my engine actually installed?" short of selecting the
        synthesizer and listening for silence.
        """
        try:
            sysTrayIcon = gui.mainFrame.sysTrayIcon
            self._menuItem = sysTrayIcon.toolsMenu.Append(
                wx.ID_ANY, self.MENU_LABEL, self.MENU_HELP)
            sysTrayIcon.Bind(wx.EVT_MENU, self._onMenu, self._menuItem)
            log.info("outSPOKEN: added the Tools menu item")
        except Exception:
            # Never fatal: the add-on still speaks without a menu entry, and
            # global plugins load while the GUI is still assembling itself.
            log.error("outSPOKEN: could not add the Tools menu item",
                      exc_info=True)

    def terminate(self):
        """Take the menu item away again, or reloading duplicates it."""
        try:
            if self._menuItem is not None:
                gui.mainFrame.sysTrayIcon.toolsMenu.Remove(self._menuItem.Id)
                self._menuItem.Destroy()
                self._menuItem = None
        except Exception:
            log.error("outSPOKEN: could not remove the Tools menu item",
                      exc_info=True)
        super().terminate()

    def _onMenu(self, evt):
        """Always ask, whatever the marker says and whoever else asked today.

        Chosen on purpose: somebody who opens this from a menu is asking the
        question right now, and answering "you said not to ask" would be
        obtuse.
        """
        ok, lines = rom.explain()
        log.info("outSPOKEN: engine %s (from the Tools menu)\n  %s"
                 % ("ready" if ok else "NOT ready", "\n  ".join(lines)))
        folder = rom.config_dir()
        try:
            os.makedirs(folder, exist_ok=True)
        except OSError:
            pass
        if ok:
            gui.messageBox(
                "outSPOKEN has its engine.\n\n%s" % "\n".join(lines),
                "MacinTalk", wx.OK | wx.ICON_INFORMATION)
            return
        self._ask(folder)

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
            if not _claim_the_startup_dialog("outspoken"):
                log.info("outSPOKEN: another Macintosh speech add-on has "
                         "already asked this session; Tools menu has ours")
                return
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
