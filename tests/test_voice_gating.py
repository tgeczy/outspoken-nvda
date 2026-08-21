# -*- coding: utf-8 -*-
"""A voice is only offered when the engine that speaks it is installed.

These build their own `rom/` tree out of a synthesised `ttvd`, so they need
nothing extracted and run on any machine.

The case is not hypothetical. `tools/extract_rom.py` pulls MacinTalk Pro's
Victoria, Bruce and Agnes out of any disk image that carries them -- about
900 KB each -- independently of whether that image also had MacinTalk Pro
itself. So holding three Pro voices and nothing able to speak them is a
perfectly ordinary state to be in, and NVDA must not offer them: a synthesizer
that lists a voice and then says nothing is worse than one that does not list
it.
"""
import os
import struct

import pytest


def _ttvd(creator=b"gala", vid=210, name=b"Victoria", gender=2):
    """A valid 362-byte Apple VoiceDescription. See tools/voices.py."""
    head = struct.pack(">I4sII", 362, creator, vid, 1)
    nm = bytes([len(name)]) + name + b"\0" * (63 - len(name))
    comment = b"\0" * 256
    tail = struct.pack(">hhhhh", gender, 35, 0, 0, 0) + b"\0" * 16
    out = head + nm + comment + tail
    assert len(out) == 362
    return out


def _tree(root, voices, engine_files=(), complete=True):
    """Lay out a rom folder: some voices, and maybe an engine to speak them.

    `complete=False` leaves each voice as the bare `ttvd` that a pre-Pro
    extraction produced. That is a real state to be in rather than an invented
    one -- see `voices.VOICE_PARTS`.
    """
    import voices as _voicelib
    for folder, (creator, vid, name) in voices.items():
        d = os.path.join(root, "voices", folder)
        os.makedirs(d, exist_ok=True)
        with open(os.path.join(d, "ttvd_%d.bin" % vid), "wb") as fh:
            fh.write(_ttvd(creator, vid, name))
        if not complete:
            continue
        need_files, need_types = _voicelib.VOICE_PARTS.get(
            creator.decode("ascii"), ((), ()))
        for f in need_files:
            with open(os.path.join(d, f), "wb") as fh:
                fh.write(bytes(16))
        for t in need_types:
            if t == "ttvd":
                continue
            with open(os.path.join(d, "%s_%d.bin" % (t, vid)), "wb") as fh:
                fh.write(bytes(16))
    for rel in engine_files:
        p = os.path.join(root, rel)
        os.makedirs(os.path.dirname(p), exist_ok=True)
        with open(p, "wb") as fh:
            fh.write(b"\0" * 16)
    return str(root)


def _engine(creator):
    """Every file that engine needs, from the add-on's own list.

    Written out rather than hardcoded here: MacinTalk Pro's grew from one file
    to three when it turned out that an engine folder without `datafork.bin`
    OPENS, takes a voice and then says nothing.
    """
    import voices as _v
    folder = {"gala": "macintalkpro", "mtk2": "macintalk2"}[creator]
    return ["%s/%s" % (folder, f) for f in _v.ENGINE_FILES[creator]]


@pytest.fixture
def voicelib():
    import voices
    return voices


def test_a_pro_voice_alone_is_not_offered(tmp_path, voicelib):
    """Victoria extracted, MacinTalk Pro not. She must not reach NVDA."""
    root = _tree(tmp_path, {"Victoria": (b"gala", 210, b"Victoria")})
    found, _bad = voicelib.installed(roots=[root])
    assert [v.name for v in found] == ["Victoria"], \
        "the voice itself should still be visible to the tools"
    speakable, bad = voicelib.installed(roots=[root], speakable=True)
    assert speakable == [], "a Pro voice was offered with no Pro engine"
    assert bad and "MacinTalk Pro" in bad[0][1]


def test_the_same_voice_is_offered_once_the_engine_is_there(tmp_path,
                                                            voicelib):
    root = _tree(tmp_path, {"Victoria": (b"gala", 210, b"Victoria")},
                 engine_files=_engine("gala"))
    speakable, _bad = voicelib.installed(roots=[root], speakable=True)
    assert [v.name for v in speakable] == ["Victoria"]


def test_one_missing_engine_does_not_hide_another_engines_voices(tmp_path,
                                                                 voicelib):
    """The failure worth guarding: gating too broadly.

    A user with MacinTalk 2 and a stray Pro voice must keep all ten of their
    MacinTalk 2 voices.
    """
    root = _tree(tmp_path,
                 {"Victoria": (b"gala", 210, b"Victoria"),
                  "Ben": (b"mtk2", 3, b"Ben"),
                  "Boris": (b"mtk2", 5, b"Boris")},
                 engine_files=_engine("mtk2"))
    speakable, _bad = voicelib.installed(roots=[root], speakable=True)
    assert [v.name for v in speakable] == ["Ben", "Boris"]


def test_half_an_engine_is_not_an_engine(tmp_path, voicelib):
    """MacinTalk 2 is a pair. One half is a broken extraction, not an engine."""
    root = _tree(tmp_path, {"Ben": (b"mtk2", 3, b"Ben")},
                 engine_files=["macintalk2/Cecy_3.bin"])
    assert not voicelib.engine_installed("mtk2", roots=[root])
    speakable, _bad = voicelib.installed(roots=[root], speakable=True)
    assert speakable == []


def test_macintalk_3_voices_are_never_offered(tmp_path, voicelib):
    """Delegated, not missing: a native add-on builds that engine. However
    complete the extraction, those voices are not ours to offer."""
    root = _tree(tmp_path, {"Fred": (b"mtk3", 1, b"Fred")})
    assert not voicelib.engine_installed("mtk3", roots=[root])
    speakable, _bad = voicelib.installed(roots=[root], speakable=True)
    assert speakable == []


def test_the_shipped_copy_matches_the_tool(voicelib):
    """`voices.py` exists twice, in tools/ and in the add-on. A fix applied to
    one and not the other is invisible until a user reports it."""
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    a = os.path.join(root, "tools", "voices.py")
    b = os.path.join(root, "addon", "synthDrivers", "_outspoken", "voices.py")
    with open(a, "rb") as fa, open(b, "rb") as fb:
        assert fa.read() == fb.read(), \
            "tools/voices.py and the add-on's copy have diverged"


def test_an_incomplete_voice_is_not_offered_even_with_the_engine(tmp_path,
                                                                 voicelib):
    """The upgrade case, and it is the worst kind of failure.

    A user who extracted voices before this project could drive MacinTalk Pro
    has a `voices/Agnes` holding one file -- the `ttvd` -- because that was all
    anything needed then. Tomi's own machine was in exactly this state on
    2026-08-20. Installing the engine afterwards makes the stub *look* ready:
    it is listed, it can be chosen, and then the engine raises "has neither
    fork" and the voice says nothing.

    Gating on the engine alone cannot catch this, because the engine really is
    there. See `voices.VOICE_PARTS`.
    """
    root = _tree(tmp_path, {"Agnes": (b"gala", 320, b"Agnes")},
                 engine_files=_engine("gala"), complete=False)
    found, _bad = voicelib.installed(roots=[root])
    assert [v.name for v in found] == ["Agnes"], \
        "the tools should still see her, so the user can be told why"
    speakable, bad = voicelib.installed(roots=[root], speakable=True)
    assert speakable == [], "an incomplete Pro voice was offered to NVDA"
    assert bad and "incomplete" in bad[0][1], \
        "it must say what is missing rather than just refusing: %r" % (bad,)
    assert "rsrcfork.bin" in bad[0][1]


def test_a_complete_voice_is_still_offered(tmp_path, voicelib):
    """The guard on the guard. Gating too broadly is its own failure, and this
    one would quietly remove every voice the user has."""
    root = _tree(tmp_path, {"Agnes": (b"gala", 320, b"Agnes")},
                 engine_files=_engine("gala"))
    speakable, _bad = voicelib.installed(roots=[root], speakable=True)
    assert [v.name for v in speakable] == ["Agnes"]


def test_a_macintalk_2_voice_without_its_wave_data_is_not_offered(tmp_path,
                                                                  voicelib):
    """The same hole on the other engine: MacinTalk 2 registers three
    resources per voice, and with only the descriptor it loads and says
    nothing. Real MacinTalk 2 voice folders carry no `resources.tsv`, so that
    must not be required of them -- checked against one rather than assumed."""
    root = _tree(tmp_path, {"Ben": (b"mtk2", 3, b"Ben")},
                 engine_files=_engine("mtk2"),
                 complete=False)
    speakable, bad = voicelib.installed(roots=[root], speakable=True)
    assert speakable == []
    assert bad and "ttvw" in bad[0][1], bad


# The extractor makes the same judgement, before NVDA is ever restarted.


@pytest.fixture
def extractor():
    import extract_rom
    return extract_rom


def test_the_extractor_says_what_nvda_will_offer(tmp_path, extractor, capsys):
    """Writing files is not the same as being able to speak, and until this
    existed the difference only showed up after restarting NVDA."""
    root = _tree(tmp_path,
                 {"Agnes": (b"gala", 320, b"Agnes"),
                  "Ben": (b"mtk2", 3, b"Ben")},
                 engine_files=_engine("gala") + _engine("mtk2"))
    extractor.report_ready(root)
    out = capsys.readouterr().out
    assert "MacinTalk Pro" in out and "Agnes" in out
    assert "MacinTalk 2" in out and "Ben" in out


def test_the_extractor_names_an_incomplete_voice_and_what_is_missing(
        tmp_path, extractor, capsys):
    """The case that actually happened. A voice extracted before this project
    could drive its engine is a folder holding a `ttvd` and nothing else, and
    adding the engine afterwards makes it look ready.

    Saying "3 resources written" while NVDA silently drops the voice is the
    unhelpful half of the truth, so the reason has to be printed with the
    missing file named."""
    root = _tree(tmp_path, {"Agnes": (b"gala", 320, b"Agnes")},
                 engine_files=_engine("gala"), complete=False)
    extractor.report_ready(root)
    out = capsys.readouterr().out
    assert "NOT offered" in out
    assert "incomplete" in out and "rsrcfork.bin" in out
    assert "Re-run this tool" in out, "it must say how to fix it"


def test_the_extractor_can_find_where_nvda_actually_reads_from(tmp_path,
                                                               extractor,
                                                               monkeypatch):
    """`--nvda` exists because extracting into ./rom and expecting NVDA to
    notice is the mistake that left stub voice folders on the author's own
    machine while a complete extraction sat in the repository.

    It refuses to guess: a folder nothing will read is worse than an error.
    """
    fake = tmp_path / "roaming"
    (fake / "nvda").mkdir(parents=True)
    monkeypatch.setenv("APPDATA", str(fake))
    got = extractor.nvda_roms()
    # Against `rom.CONFIG_DIRNAME` rather than a literal, because the whole
    # point of this test is that the extractor writes where the driver reads.
    # A literal on both sides lets them drift apart in step and still pass.
    import rom
    assert got, "it refused a configuration directory that exists"
    assert got == os.path.join(str(fake / "nvda"), rom.CONFIG_DIRNAME), (
        "the extractor writes to %r; the driver reads %r"
        % (got, rom.CONFIG_DIRNAME))

    monkeypatch.setenv("APPDATA", str(tmp_path / "nothing-here"))
    monkeypatch.setattr(os.path, "expanduser", lambda p: str(tmp_path / "nope"))
    assert extractor.nvda_roms() is None, "it guessed instead of refusing"


def test_half_a_pro_engine_is_not_an_engine(tmp_path, voicelib):
    """The engine side of the same hole, and the dangerous half is silent.

    Measured, with a complete Agnes beside each:

        everything          speaks
        no rsrcfork.bin     Open returns -64
        no datafork.bin     Open succeeds, takes the voice, speaks NOTHING
        neither fork        "has neither fork"

    The third is why `ENGINE_FILES["gala"]` is three files. A partial
    extraction that opens is indistinguishable from a working one right up to
    the silence, which is the failure this project treats as worst.
    """
    for missing in ("datafork.bin", "rsrcfork.bin", "gtse_1.bin"):
        files = [f for f in _engine("gala") if not f.endswith(missing)]
        root = _tree(tmp_path / missing, {"Agnes": (b"gala", 320, b"Agnes")},
                     engine_files=files)
        assert not voicelib.engine_installed("gala", roots=[root]), \
            "a Pro engine folder without %s was accepted" % missing
        speakable, bad = voicelib.installed(roots=[root], speakable=True)
        assert speakable == [], "Agnes was offered without %s" % missing
        assert bad and "MacinTalk Pro" in bad[0][1]


def test_the_extractor_and_the_driver_agree_about_macintalk_1(extractor):
    """`report_ready` promises the same judgement the add-on makes, and for
    MacinTalk 1 it cannot get that from `voices.installed` -- its two voices
    are a pitch setting, not folders -- so the condition is written out twice
    and has to be held together.

    The driver needs `rom.REQUIRED` plus the letter-to-sound rules: without
    RULZ the engine takes phonemes only, which no screen reader sends."""
    import rom
    assert set(extractor.MT1_REQUIRED) == set(rom.REQUIRED) | {"RULZ_1129.bin"}


def test_the_extractor_does_not_assemble_an_engine_from_two_folders(
        tmp_path, extractor, capsys):
    """Names repeat across a rom tree by design, so "have I got these files"
    has to be asked per directory. Accumulating them declared MacinTalk 1
    present when its three files were scattered over three folders."""
    for i, name in enumerate(extractor.MT1_REQUIRED):
        d = tmp_path / ("part%d" % i)
        d.mkdir()
        (d / name).write_bytes(bytes(16))
    extractor.report_ready(str(tmp_path))
    assert "MacinTalk 1" not in capsys.readouterr().out, \
        "an engine was assembled out of three separate folders"

    whole = tmp_path / "macintalk1"
    whole.mkdir()
    for name in extractor.MT1_REQUIRED:
        (whole / name).write_bytes(bytes(16))
    extractor.report_ready(str(tmp_path))
    out = capsys.readouterr().out
    assert "MacinTalk 1" in out and "Male" in out, \
        "a complete MacinTalk 1 was not reported: %s" % out
