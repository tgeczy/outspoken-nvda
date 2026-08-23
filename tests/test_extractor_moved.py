# -*- coding: utf-8 -*-
"""The extractor is add-on code now, and has to keep behaving like a tool.

**It used to be a script and nothing else**, which meant extracting an engine
needed Python, a terminal and `pip install machfs`. That is the single largest
barrier between somebody and a 1984 speech synthesiser, and it is the reason
`ospmanager.py` exists. The logic was *moved*, not rewritten -- `git log
--follow` on `ospextract.py` reaches `tools/extract_rom.py` -- and this holds
the move honest.

What is checked here is the shape, not the extraction: whether an image really
yields the right resources is answered by running it against a real image, and
was, byte for byte against the output of the version before the move -- 254
files, same hash, from both a MacBinary `.bin` and a one-gigabyte HFV.

The parts that can rot silently are the ones below: an import that only works
because a developer has something pip-installed, a vendored library drifting
from upstream, and the CLI quietly losing a flag people's notes tell them to
use.
"""
import io
import os
import re
import subprocess
import sys

import pytest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PRIVATE = os.path.join(ROOT, "addon", "synthDrivers", "_outspoken")
MACHFS = os.path.join(PRIVATE, "_machfs")


@pytest.fixture(scope="module")
def ospextract():
    sys.path.insert(0, PRIVATE)
    import ospextract as mod
    return mod


# -- the vendored library -------------------------------------------------

def test_machfs_is_vendored_with_its_licence():
    """MIT is fine inside a GPL add-on; MIT without the notice is not."""
    licence = os.path.join(MACHFS, "LICENSE")
    assert os.path.isfile(licence), "machfs is vendored without its licence"
    text = io.open(licence, encoding="utf-8").read()
    assert "MIT" in text and "Elliot Nunn" in text


def test_nothing_imports_machfs_from_site_packages():
    """**The add-on must not depend on anything pip-installed.**

    It cannot run `pip`, so an import that happens to work on a developer's
    machine is an import that fails on everybody else's -- and fails at the
    moment the user asks for the one thing this add-on is for.
    """
    for name in sorted(os.listdir(PRIVATE)):
        if not name.endswith(".py"):
            continue
        src = io.open(os.path.join(PRIVATE, name), encoding="utf-8").read()
        for line in src.splitlines():
            stripped = line.strip()
            if stripped.startswith("#"):
                continue
            assert not re.match(r"^(import machfs|from machfs\b)", stripped), (
                "%s imports the installed machfs; it must use the vendored "
                "_machfs" % name)


def test_the_vendored_copy_differs_from_upstream_only_in_its_imports():
    """One line in each of two files, so an update is a readable diff.

    machfs imports `macresources` at module scope for code that only ever
    writes a volume. Rather than vendor a second library or delete the lines,
    the import is pointed at a private stub -- and this is what keeps that from
    growing into a fork.
    """
    changed = []
    for name in ("__init__.py", "bitmanip.py", "btree.py", "directory.py",
                 "main.py"):
        src = io.open(os.path.join(MACHFS, name), encoding="utf-8").read()
        for line in src.splitlines():
            if "macresources" in line:
                changed.append((name, line.strip()))
    assert changed == [
        ("directory.py",
         "from ._macresources import make_rez_code, parse_rez_code, "
         "make_file, parse_file"),
        ("main.py", "from ._macresources import Resource, make_file, "
                    "parse_file"),
    ], changed


def test_the_stub_raises_rather_than_inventing_an_answer(ospextract):
    """It is the reason the one read-path caller was found at all.

    `Volume.read` calls `parse_file` while resolving Finder aliases -- which
    was not obvious, and a stub returning something plausible would have hidden
    it behind a wrong answer much further downstream.
    """
    from _machfs import _macresources
    with pytest.raises(NotImplementedError):
        _macresources.make_file([])
    #: The one that read genuinely reaches, and an empty list is a supported
    #: answer there: the caller catches StopIteration and leaves aliases alone.
    assert _macresources.parse_file(b"") == []


# -- the module ------------------------------------------------------------

def test_the_engines_it_lists_are_the_engines_it_can_install(ospextract):
    """The manager's rows come from here, not from what is installed.

    A manager that lists only what you already have cannot tell you what you
    are missing, which is the question somebody opens it to ask.
    """
    listed = {name for name, _about in ospextract.ENGINES}
    assert listed == {"MacinTalk 1", "MacinTalk 2", "MacinTalk 3",
                      "MacinTalk Pro"}
    for _name, about in ospextract.ENGINES:
        assert about and about[0].isdigit(), about


def test_extract_is_importable_without_a_disk_image(ospextract):
    for name in ("extract", "plan", "run", "open_image", "installed_engines",
                 "report_ready", "nvda_roms"):
        assert callable(getattr(ospextract, name)), name


def test_installed_engines_answers_for_an_empty_folder(ospextract, tmp_path):
    have, bad = ospextract.installed_engines(str(tmp_path))
    assert have == {}
    assert bad == [] or isinstance(bad, list)


def test_open_image_declines_what_is_not_an_image(ospextract, tmp_path):
    """Returning None is how the caller knows to treat it as a single file."""
    junk = tmp_path / "notanimage.bin"
    junk.write_bytes(b"\x00" * 4096)
    assert ospextract.open_image(str(junk)) is None
    assert ospextract.open_image(str(tmp_path / "nothing-here")) is None


# -- the command line ------------------------------------------------------

def test_the_cli_still_offers_the_flags_people_were_told_to_use():
    """`--nvda` and `--list` are in the README, the dialogs and the READMEs
    this add-on writes into the engine folder. Losing one in a refactor would
    break instructions already in front of people."""
    out = subprocess.run(
        [sys.executable, os.path.join(ROOT, "tools", "extract_rom.py"),
         "--help"], capture_output=True, text=True, timeout=60)
    assert out.returncode == 0, out.stderr
    for flag in ("--out", "--nvda", "--list"):
        assert flag in out.stdout, flag


def test_the_cli_is_a_thin_wrapper_now():
    """If the logic creeps back into the tool, the dialog stops getting it."""
    src = io.open(os.path.join(ROOT, "tools", "extract_rom.py"),
                  encoding="utf-8").read()
    assert "import ospextract" in src
    body = [l for l in src.splitlines()
            if l.strip() and not l.strip().startswith("#")]
    assert len(body) < 120, ("the CLI has grown back to %d lines; the work "
                             "belongs in ospextract.py where NVDA can reach "
                             "it" % len(body))
