# -*- coding: utf-8 -*-
"""The volume slider, which is arithmetic and can be tested as arithmetic.

Volume is the one setting no engine here is asked about. All four hand back
8-bit unsigned and the driver widens it to 16, so the slider is folded into
that widening -- which means these tests need no ROM, no emulator and no
audio device, and they check the thing that actually ships.
"""
import struct

import pytest


def _driver():
    """A driver object without `__init__`, which needs an engine folder."""
    import outspoken
    d = outspoken.SynthDriver.__new__(outspoken.SynthDriver)
    d._rate, d._pitch, d._inflection = 50, 50, 50
    d._volume = 100
    d._gain = outspoken.SynthDriver._gainTables(100)
    return d


def _samples(pcm16):
    return list(struct.unpack("<%dh" % (len(pcm16) // 2), pcm16))


ALL_BYTES = bytes(range(256))


def test_full_volume_is_byte_for_byte_what_0_8_0_produced():
    """The regression this whole feature has to not be.

    Before the slider existed the conversion was one `translate` into the high
    byte of each sample and nothing in the low one. Everybody who never
    touches the slider must keep getting exactly that -- so the test spells
    out the old implementation rather than calling the new one twice.
    """
    d = _driver()
    old = bytearray(len(ALL_BYTES) * 2)
    old[1::2] = ALL_BYTES.translate(bytes(b ^ 0x80 for b in range(256)))
    assert d._to16(ALL_BYTES) == bytes(old)


def test_a_sample_is_the_byte_less_128_times_256():
    d = _driver()
    got = _samples(d._to16(ALL_BYTES))
    assert got == [(b - 128) * 256 for b in range(256)]
    assert min(got) == -32768 and max(got) == 32512


@pytest.mark.parametrize("volume", [0, 25, 50, 75, 90, 99])
def test_the_slider_scales_and_never_exceeds_the_engine(volume):
    """Linear, and attenuating only.

    There is no boost here. 100 is the engine's own level, so no sample can
    come out louder than it went in and nothing can clip that did not clip
    before -- which is why none of the sibling Leopard driver's margin and
    ceiling apparatus is needed.
    """
    d = _driver()
    d._gain = d._gainTables(volume)
    got = _samples(d._to16(ALL_BYTES))
    for b, v in enumerate(got):
        want = int(round((b - 128) * 256 * volume / 100.0))
        assert v == max(-32768, min(32767, want)), "byte %d" % b
    assert max(abs(v) for v in got) <= 32768


def test_zero_is_actually_silent():
    d = _driver()
    d._gain = d._gainTables(0)
    assert set(_samples(d._to16(ALL_BYTES))) == {0}


def test_every_position_of_the_slider_changes_the_audio():
    """The bug the pitch slider had for months: a control that does nothing.

    A byte comparison rather than a loudness measurement, because that is the
    failure being guarded against -- not "slightly wrong volume" but renders
    that are the same bytes.
    """
    d = _driver()
    seen = {}
    for volume in range(0, 101, 5):
        d._gain = d._gainTables(volume)
        out = d._to16(ALL_BYTES)
        assert out not in seen.values(), (
            "volume %d is byte-identical to %r"
            % (volume, [k for k, v in seen.items() if v == out]))
        seen[volume] = out


def test_the_tables_are_clamped_rather_than_wrapping():
    """Out of range in either direction, since NVDA is not the only caller.

    A `VolumeCommand` offset is added to the user's setting before it gets
    here, so 100 + 30 is a perfectly ordinary thing to arrive.
    """
    d = _driver()
    assert d._gainTables(130) is d._gainTables(100)
    assert d._gainTables(-20) is d._gainTables(0)


def test_a_volume_command_reaches_the_conversion(monkeypatch):
    """`VolumeCommand` is not sent at all unless it is in supportedCommands.

    The same trap "capital pitch change percentage" fell into: NVDA drops any
    command the driver has not declared, so nothing in a log would ever show
    it, and the setting looks broken rather than unsupported.
    """
    import outspoken
    import speech.commands
    wanted = outspoken.SynthDriver.supportedCommands
    assert speech.commands.VolumeCommand in wanted
    assert speech.commands.RateCommand in wanted

    d = _driver()
    seen = []
    d._gainTables = lambda v: seen.append(v) or (None, d._FLIP)
    d._applySettings = lambda eng, adj=0, radj=0: seen.append(("rate", radj))
    d._entry = lambda: ("mtk2:Ben", "Ben", "mtk2", None)

    class Eng(object):
        def translate(self, text):
            return text

        def speak(self, text):
            return b"\x80" * 100

    d._stopped = False
    d._engineRate = 200
    d._cancels = 0
    d._audioOut = False
    d._nSpoken = d._nEmpty = 0
    d._audioQueue = type("Q", (), {"put": lambda self, x: None})()
    d._flush(Eng(), ["hello"], 0, -40, 15, [], 0.0)
    assert ("rate", 15) in seen, (
        "a RateCommand offset never reached the engine")
    assert 60 in seen, "a VolumeCommand offset never reached the conversion"
