# -*- coding: utf-8 -*-
"""Moving the engine folder under `macintalk`, which touches the user's data.

Every other test here can be wrong and cost a bad render. This one can be
wrong and cost somebody the engine they extracted from a disc they may no
longer have to hand, so it is written from that end: what must never happen,
first.
"""
import os

import pytest


@pytest.fixture
def fake_config(tmp_path, monkeypatch):
    """A throwaway NVDA configuration directory, with `rom` pointed at it."""
    import rom
    monkeypatch.setattr(rom, "config_dir",
                        lambda: os.path.join(str(tmp_path), "macintalk",
                                             "outspoken"))
    return tmp_path


def _plant(folder, name="DRVR_1030.bin", data=b"engine"):
    os.makedirs(folder, exist_ok=True)
    with open(os.path.join(folder, name), "wb") as f:
        f.write(data)
    return folder


def test_the_old_folder_is_moved_not_copied(fake_config):
    """A rename, so it cannot half-succeed and cannot cost minutes.

    Leopard's tree is 717 MB; outspoken's is 7. The rule is the same for both
    and the reason is the same: a copy would need to be resumable, verified
    and undoable before it could be run without asking.
    """
    import rom
    old = _plant(os.path.join(str(fake_config), "outspoken-roms", "macintalk1"))
    assert rom.migrate()
    assert not os.path.exists(os.path.join(str(fake_config), "outspoken-roms"))
    moved = os.path.join(str(fake_config), "macintalk", "outspoken",
                         "macintalk1", "DRVR_1030.bin")
    assert os.path.isfile(moved), "the engine did not arrive"
    assert open(moved, "rb").read() == b"engine"
    assert not os.path.exists(old), "the original was left behind as well"


def test_it_never_touches_an_existing_new_folder(fake_config):
    """The case that would destroy data: both folders present.

    Someone installs, downgrades, re-extracts, upgrades again. If migration
    overwrote or merged, whichever copy is newer loses. It must decline.
    """
    import rom
    _plant(os.path.join(str(fake_config), "outspoken-roms"), data=b"old")
    _plant(os.path.join(str(fake_config), "macintalk", "outspoken"),
           data=b"new")
    assert rom.migrate() is None, "it moved on top of an existing folder"
    kept = os.path.join(str(fake_config), "macintalk", "outspoken",
                        "DRVR_1030.bin")
    assert open(kept, "rb").read() == b"new", "the newer folder was clobbered"
    stale = os.path.join(str(fake_config), "outspoken-roms", "DRVR_1030.bin")
    assert open(stale, "rb").read() == b"old", "the older folder was destroyed"


def test_a_failed_move_leaves_everything_where_it_was(fake_config,
                                                      monkeypatch):
    """Windows refuses to rename a directory with a file open in it.

    That is not an error case to be recovered from -- it is the ordinary case
    of the engine being in use -- so the answer is to change nothing and go on
    reading the old location.
    """
    import rom
    _plant(os.path.join(str(fake_config), "outspoken-roms"))

    def refuse(*a, **k):
        raise OSError(32, "The process cannot access the file")

    monkeypatch.setattr(os, "rename", refuse)
    assert rom.migrate() is None
    assert os.path.isfile(os.path.join(str(fake_config), "outspoken-roms",
                                       "DRVR_1030.bin"))


def test_the_old_folder_is_still_searched_afterwards(fake_config,
                                                     monkeypatch):
    """Not a courtesy for one release -- the permanent answer to a locked move.

    If the rename cannot happen the engine has to keep working from where it
    is, for as long as that lasts, which is possibly forever.
    """
    import rom
    _plant(os.path.join(str(fake_config), "outspoken-roms"))
    monkeypatch.setattr(os, "rename",
                        lambda *a, **k: (_ for _ in ()).throw(OSError()))
    roots = rom.search_roots()
    assert os.path.join(str(fake_config), "outspoken-roms") in roots
    assert roots.index(rom.config_dir()) < roots.index(
        os.path.join(str(fake_config), "outspoken-roms")), \
        "the old folder must be searched after the new one, not before"


def test_migrating_twice_is_harmless(fake_config):
    import rom
    _plant(os.path.join(str(fake_config), "outspoken-roms"))
    assert rom.migrate()
    assert rom.migrate() is None
    assert os.path.isfile(os.path.join(str(fake_config), "macintalk",
                                       "outspoken", "DRVR_1030.bin"))


def test_nothing_happens_on_a_fresh_install(fake_config):
    import rom
    assert rom.migrate() is None
    assert not os.path.exists(os.path.join(str(fake_config), "macintalk"))


def test_the_breadcrumb_is_read_back_as_a_root(fake_config):
    """The pointer file makes a rollback survivable, so it must be read.

    Unlike the sibling add-ons this mechanism is new here, which means the
    breadcrumb only helps from this release onward -- an earlier outspoken
    looks in `outspoken-roms` and nowhere else. Said plainly in the release
    notes rather than discovered.
    """
    import rom
    _plant(os.path.join(str(fake_config), "outspoken-roms"))
    moved = rom.migrate()
    pointer = os.path.join(str(fake_config), "outspoken-roms.txt")
    assert os.path.isfile(pointer), "no breadcrumb was left"
    assert open(pointer, encoding="utf-8").read().strip() == moved
    assert moved in rom.search_roots()


def test_a_pointer_the_user_wrote_is_never_overwritten(fake_config):
    """Somebody keeping the engine on another drive said so in that file."""
    import rom
    pointer = os.path.join(str(fake_config), "outspoken-roms.txt")
    with open(pointer, "w", encoding="utf-8") as f:
        f.write("D:\\my-own-roms")
    _plant(os.path.join(str(fake_config), "outspoken-roms"))
    rom.migrate()
    assert open(pointer, encoding="utf-8").read() == "D:\\my-own-roms"
    assert "D:\\my-own-roms" in rom.search_roots()
