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


#: The two serve hosts held to the driver: the Python script that IS the
#: driver, and the C program 2.0 replaces it with.  Both must be byte-identical
#: to the in-process driver; that is the gate the SAPI launcher swaps on.
_BITS = 64 if sys.maxsize > 2 ** 32 else 32
HOST_EXE = os.path.join(ROOT, "build",
                        ("osp_host.exe" if _BITS == 64 else "osp_host_x86.exe")
                        if os.name == "nt" else os.path.join("linux", "osp_host"))


def _serve_commands():
    import globalVars
    cfg = globalVars.appArgs.configPath
    out = [("python", [sys.executable, SERVE, cfg])]
    if os.path.isfile(HOST_EXE):
        out.append(("c", [HOST_EXE, "--serve", cfg]))
    return out


def _via_serve(voice, text, rate, pitch, volume, command=None):
    if command is None:
        command = _serve_commands()[0][1]
    proc = subprocess.Popen(
        command,
        stdin=subprocess.PIPE, stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL)
    try:
        v, t = voice.encode(), text.encode()
        proc.stdin.write(struct.pack("<IIiiiII", REQ, 1, rate, pitch, volume,
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


@pytest.mark.parametrize("prefix", ["", "mtk2:", "mtk3:", "gala:", "cami:"])
@pytest.mark.parametrize("host", ["python", "c"])
def test_the_bridge_is_the_driver(driver, rom_files, prefix, host):
    """One voice per engine family the staging provides, byte for byte,
    through each serve host there is."""
    commands = dict(_serve_commands())
    if host not in commands:
        pytest.skip("no C host built; run build.sh")
    candidates = (_voices(driver, prefix)
                  or ([] if prefix else
                      list(driver._get_availableVoices())))
    if not candidates:
        pytest.skip("no %r voices staged" % prefix)
    voice = candidates[0]
    ours = _via_driver(driver, voice, TEXT, 50, 50, 100)
    theirs = _via_serve(voice, TEXT, 50, 50, 100, commands[host])
    assert ours, "the driver produced no audio"
    assert ours == theirs, (
        "%s via %s: serve bridge differs from the driver -- %d vs %d bytes"
        % (voice, host, len(theirs), len(ours)))


def test_settings_travel_the_wire(driver, rom_files):
    """Rate, pitch and volume land: a different setting is different audio."""
    voice = list(driver._get_availableVoices())[0]
    base = _via_serve(voice, TEXT, 50, 50, 100)
    fast = _via_serve(voice, TEXT, 90, 50, 100)
    quiet = _via_serve(voice, TEXT, 50, 50, 40)
    assert base and fast and quiet
    assert fast != base, "rate did not reach the engine"
    assert quiet != base, "volume did not reach the widening"
