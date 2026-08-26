# -*- coding: utf-8 -*-
"""The SAPI bridge is the NVDA driver, provably.

`sapi/osp_serve.py` serves the same modules NVDA loads, so its audio must be
**byte-identical** to what the driver feeds NVDA's player for the same text,
voice and settings.  This test states that as an assertion rather than a
release note: render through the in-process driver, render through the serve
subprocess, compare the bytes.

There is no port in the SAPI path, so there is nothing that can drift -- and
this test is where that claim is checked rather than made.
"""
import os
import struct
import subprocess
import sys

import pytest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SERVE = os.path.join(ROOT, "sapi", "osp_serve.py")
REQ, RSP = 0x4F535034, 0x4F535052

TEXT = "Parity is measured, not promised: 1,234 times."


def _exact(stream, n):
    buf = b""
    while len(buf) < n:
        chunk = stream.read(n - len(buf))
        assert chunk, "the serve bridge closed the pipe early"
        buf += chunk
    return buf


def _via_serve(voice, text, rate, pitch, volume):
    import globalVars
    proc = subprocess.Popen(
        [sys.executable, SERVE, globalVars.appArgs.configPath],
        stdin=subprocess.PIPE, stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL)
    try:
        v, t = voice.encode(), text.encode()
        proc.stdin.write(struct.pack("<IiiiII", REQ, rate, pitch, volume,
                                     len(v), len(t)) + v + t)
        proc.stdin.flush()
        magic, status = struct.unpack("<Ii", _exact(proc.stdout, 8))
        assert magic == RSP and status == 0, (hex(magic), status)
        pcm = b""
        while True:
            n = struct.unpack("<I", _exact(proc.stdout, 4))[0]
            if not n:
                return pcm
            pcm += _exact(proc.stdout, n * 2)
    finally:
        proc.stdin.close()
        proc.wait(timeout=10)


def _via_driver(driver, voice, text, rate, pitch, volume):
    import synthDriverHandler
    got = []
    realFeed = driver._player.feed

    def feed(data, *a, **k):
        got.append(bytes(data))
        return realFeed(data, *a, **k)

    driver._player.feed = feed
    try:
        driver._set_voice(voice)
        driver._set_rate(rate)
        driver._set_pitch(pitch)
        driver._set_volume(volume)
        synthDriverHandler.synthDoneSpeaking.arm()
        driver.speak([text])
        assert synthDriverHandler.synthDoneSpeaking.wait(60.0), \
            "the driver never finished speaking"
    finally:
        driver._player.feed = realFeed
    return b"".join(got)


def _voices(driver, prefix):
    return [v for v in driver._get_availableVoices() if v.startswith(prefix)]


@pytest.mark.parametrize("prefix", ["", "mt2:", "gala:"])
def test_the_bridge_is_the_driver(driver, rom_files, prefix):
    """One voice per engine family the staging provides, byte for byte."""
    candidates = (_voices(driver, prefix)
                  or ([] if prefix else
                      list(driver._get_availableVoices())))
    if not candidates:
        pytest.skip("no %r voices staged" % prefix)
    voice = candidates[0]
    ours = _via_driver(driver, voice, TEXT, 50, 50, 100)
    theirs = _via_serve(voice, TEXT, 50, 50, 100)
    assert ours, "the driver produced no audio"
    assert ours == theirs, (
        "%s: serve bridge differs from the driver -- %d vs %d bytes"
        % (voice, len(theirs), len(ours)))


def test_settings_travel_the_wire(driver, rom_files):
    """Rate, pitch and volume land: a different setting is different audio."""
    voice = list(driver._get_availableVoices())[0]
    base = _via_serve(voice, TEXT, 50, 50, 100)
    fast = _via_serve(voice, TEXT, 90, 50, 100)
    quiet = _via_serve(voice, TEXT, 50, 50, 40)
    assert base and fast and quiet
    assert fast != base, "rate did not reach the engine"
    assert quiet != base, "volume did not reach the widening"
