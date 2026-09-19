# -*- coding: utf-8 -*-
"""The C serve host is the Python serve host, on every engine, over the wire.

`tests/test_sapi_serve.py` holds each serve host to the in-process driver,
but the suite's staged configuration carries only the 1984 engine.  This
points both hosts at NVDA's real configuration folder on this machine --
where every extracted engine lives -- and compares them with each other,
one voice per engine family, through the same framed protocol the SAPI
bridge speaks: request, response, chunks, terminator.  And it cancels an
utterance in flight the way the bridge does, by seq, and checks the stream
stays in step and the next utterance is untouched.

Skips without the C program, or without engine data in NVDA's folder.
"""
import os
import struct
import subprocess
import sys
import time

import pytest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SERVE_PY = os.path.join(ROOT, "sapi", "osp_serve.py")
_BITS = 64 if sys.maxsize > 2 ** 32 else 32
HOST_EXE = os.path.join(ROOT, "build",
                        ("osp_host.exe" if _BITS == 64 else "osp_host_x86.exe")
                        if os.name == "nt" else os.path.join("linux", "osp_host"))
REQ, RSP, CANCEL = 0x4F535034, 0x4F535052, 0x4F535043

TEXT = "Parity is measured, not promised: 1,234 times, on the 3rd try."
LONG = "Provided arguments colon: left bracket, debug logging. " * 6
VOICES = ["male", "mtk2:Ben", "mtk3:Fred", "gala:Bruce", "cami:Carlos"]


def _config():
    appdata = os.environ.get("APPDATA")
    if not appdata:
        pytest.skip("no APPDATA")
    cfg = os.path.join(appdata, "nvda")
    if not os.path.isdir(os.path.join(cfg, "macintalk", "outspoken")):
        pytest.skip("no engine data under NVDA's configuration folder")
    return cfg


def _exact(stream, n):
    buf = b""
    while len(buf) < n:
        chunk = stream.read(n - len(buf))
        assert chunk, "the serve host closed the pipe early"
        buf += chunk
    return buf


class Host(object):
    def __init__(self, command):
        self.proc = subprocess.Popen(command, stdin=subprocess.PIPE,
                                     stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)

    def request(self, seq, voice, text, rate=50, pitch=50, volume=100):
        v, t = voice.encode(), text.encode("utf-8")
        self.proc.stdin.write(struct.pack("<IIiiiII", REQ, seq, rate, pitch, volume,
                                          len(v), len(t)) + v + t)
        self.proc.stdin.flush()

    def cancel(self, seq):
        self.proc.stdin.write(struct.pack("<II", CANCEL, seq))
        self.proc.stdin.flush()

    def response(self):
        magic, status = struct.unpack("<Ii", _exact(self.proc.stdout, 8))
        assert magic == RSP, hex(magic)
        if status:
            return status, None
        pcm = b""
        while True:
            n = struct.unpack("<I", _exact(self.proc.stdout, 4))[0]
            if not n:
                return 0, pcm
            pcm += _exact(self.proc.stdout, n * 2)

    def close(self):
        self.proc.stdin.close()
        self.proc.wait(timeout=20)


def _hosts(cfg):
    if not os.path.isfile(HOST_EXE):
        pytest.skip("no C host built; run build.sh")
    return Host([sys.executable, SERVE_PY, cfg]), Host([HOST_EXE, "--serve", cfg])


@pytest.mark.parametrize("voice", VOICES)
def test_both_hosts_render_the_same_bytes(voice):
    cfg = _config()
    py, c = _hosts(cfg)
    try:
        for host in (py, c):
            host.request(1, voice, TEXT)
        sp, pp = py.response()
        sc, pc = c.response()
        if sp == 1 and sc == 1:
            pytest.skip("%s is not installed here" % voice)
        assert sp == 0 and sc == 0, "status python %d, c %d" % (sp, sc)
        assert pp, "no audio for %s" % voice
        assert pp == pc, "%s: %d bytes from Python, %d from C" % (voice, len(pp), len(pc))
        # A second utterance with different settings, on the same open engine.
        for host in (py, c):
            host.request(2, voice, TEXT, rate=80, pitch=30, volume=60)
        assert py.response() == c.response()
    finally:
        py.close()
        c.close()


def test_a_cancel_by_seq_keeps_the_stream_in_step():
    cfg = _config()
    _py, c = _hosts(cfg)
    _py.close()
    try:
        c.request(1, "gala:Bruce", LONG)
        status, whole = c.response()
        if status:
            pytest.skip("Bruce is not installed here")
        # The same again, cancelled the moment it is sent: the response must
        # still be well formed, and shorter.
        c.request(2, "gala:Bruce", LONG)
        c.cancel(2)
        status, cut = c.response()
        assert status == 0
        assert len(cut) < len(whole), "the cancel did not shorten the render"
        # A stale cancel -- for an utterance already finished -- must not cut
        # the next one: it rendered in full and identical to the first.
        c.cancel(1)
        c.request(3, "gala:Bruce", LONG)
        status, again = c.response()
        assert status == 0
        assert again == whole, "the next utterance after a cancel differs"
    finally:
        c.close()


def test_the_listing_is_the_drivers_listing():
    cfg = _config()
    if not os.path.isfile(HOST_EXE):
        pytest.skip("no C host built; run build.sh")
    want = subprocess.run([sys.executable, SERVE_PY, "--list", cfg],
                          capture_output=True, text=True, timeout=120).stdout
    got = subprocess.run([HOST_EXE, "--list", cfg],
                         capture_output=True, text=True, timeout=120).stdout
    assert want.splitlines() == got.splitlines()
