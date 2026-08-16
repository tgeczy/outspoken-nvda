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


def _tree(root, voices, engine_files=()):
    """Lay out a rom folder: some voices, and maybe an engine to speak them."""
    for folder, (creator, vid, name) in voices.items():
        d = os.path.join(root, "voices", folder)
        os.makedirs(d, exist_ok=True)
        with open(os.path.join(d, "ttvd_%d.bin" % vid), "wb") as fh:
            fh.write(_ttvd(creator, vid, name))
    for rel in engine_files:
        p = os.path.join(root, rel)
        os.makedirs(os.path.dirname(p), exist_ok=True)
        with open(p, "wb") as fh:
            fh.write(b"\0" * 16)
    return str(root)


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
                 engine_files=["macintalkpro/gtse_1.bin"])
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
                 engine_files=["macintalk2/Cecy_1.bin",
                               "macintalk2/Cecy_3.bin"])
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
