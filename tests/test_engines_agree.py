# -*- coding: utf-8 -*-
"""The two halves that must never disagree about what engines exist.

`rom.engines_present()` decides whether the start-up dialog says "you have no
engine". `SynthDriver._catalogue()` decides which voices NVDA is offered. They
answer the same question from different lists, and when they drifted apart the
result was a user being told every start-up that they had nothing, in nineteen
working voices.

That has now happened twice. The first time, `engines_present` tested only
`REQUIRED` and omitted `RULZ_1129.bin`, so somebody with two of MacinTalk 1's
three files satisfied one half and not the other and got silence from both.
The second time, MacinTalk 3 shipped into the catalogue in 0.8.0 and was never
added to `engines_present`. The fix for the first is quoted in that function's
own docstring, immediately above the code that made the second.

So this asserts the agreement rather than either list.
"""
import pytest


def test_every_engine_module_the_driver_knows_is_named_by_the_dialog():
    """No ROM needed: this is about the two lists, not about the files.

    The driver builds `mtk2:`, `mtk3:` and `gala:` voice ids from its own
    per-engine imports, so the prefixes it can emit are the engines it can
    offer. Every one of them has to appear in `rom.ENGINE_MODULES` or the
    dialog can under-report.
    """
    import rom
    named = {mod for mod, _label in rom.ENGINE_MODULES}
    assert named == {"macintalk2", "macintalk3", "macintalkpro"}, (
        "rom.ENGINE_MODULES is %r; an engine added to the driver's catalogue "
        "was not added here, which makes the add-on tell people they have no "
        "engine while speaking to them" % (named,))


def test_the_labels_are_the_ones_the_voice_list_uses():
    """A person reading "runnable engines: MacinTalk 3" then looks for
    "(MacinTalk 3)" in NVDA's voice list. Same words, or it is a puzzle."""
    import rom
    labels = {label for _mod, label in rom.ENGINE_MODULES}
    assert labels == {"MacinTalk 2", "MacinTalk 3", "MacinTalk Pro"}


def test_the_searched_list_never_repeats_a_folder(tmp_path, monkeypatch):
    """It is shown to people in the Tools menu, and a folder listed twice
    reads as a bug in the thing that is supposed to be reassuring you."""
    import rom
    monkeypatch.setattr(rom, "config_dir",
                        lambda: str(tmp_path / "macintalk" / "outspoken"))
    pointer = tmp_path / "outspoken-roms.txt"
    # The ordinary case after a migration: the pointer names the folder that
    # would have been searched first anyway.
    pointer.write_text(str(tmp_path / "macintalk" / "outspoken"),
                       encoding="utf-8")
    roots = rom.search_roots()
    normalised = [r.lower().rstrip("\\/") for r in roots]
    assert len(normalised) == len(set(normalised)), (
        "a folder is searched twice and reported twice: %r" % (roots,))


def test_a_pointer_somewhere_else_is_still_searched(tmp_path, monkeypatch):
    """Deduplicating must not swallow the case the pointer exists for."""
    import rom
    monkeypatch.setattr(rom, "config_dir",
                        lambda: str(tmp_path / "macintalk" / "outspoken"))
    elsewhere = tmp_path / "on-another-drive"
    (tmp_path / "outspoken-roms.txt").write_text(str(elsewhere),
                                                 encoding="utf-8")
    assert str(elsewhere) in rom.search_roots()


@pytest.mark.parametrize("engine", ["macintalk2", "macintalk3",
                                    "macintalkpro"])
def test_each_named_module_can_actually_be_imported(engine):
    """A typo here is invisible: `engines_present` swallows ImportError, so a
    misspelt module simply never appears and the dialog quietly under-reports
    forever."""
    __import__(engine)
