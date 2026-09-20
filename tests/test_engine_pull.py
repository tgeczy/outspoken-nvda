# -*- coding: utf-8 -*-
"""The host's streamed render is its blocking render, byte for byte.

The Python asserts this for MacinTalk 3 (`tests/test_macintalk3.py`): what
the sink is handed, concatenated, equals what the ordinary call returns,
trimming and all.  The host's pull model -- `osp_engine_speak_start`, then
`osp_engine_pull` until it answers 0 -- is held to the same rule on every
engine, and to two more the Python asserts: a long utterance's first piece
arrives early, and an abandoned utterance stops the engine quickly and
leaves it able to speak the next one exactly as it would have anyway.

Needs the built host and the engine data; skips without.
"""
import ctypes
import os
import sys
import time

import pytest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
sys.path.insert(0, os.path.join(ROOT, "addon", "synthDrivers", "_outspoken"))

import render_oracle                                          # noqa: E402

TEXTS = ["Hello.", "a, b, c, d, e", "The quick brown fox jumps over 12 lazy dogs.",
         "Provided arguments colon: left bracket, debug logging. " * 3, "", "   "]


@pytest.fixture(scope="module")
def host():
    import osp
    import paths
    if not os.path.isfile(osp.DLL):
        pytest.skip("host library not built; run build.sh")
    if not paths.roots():
        pytest.skip("no engine data; run tools/extract_rom.py")
    side = render_oracle.HostSide()
    if not side.available:
        pytest.skip("host has no engine API")
    L = side.lib
    L.osp_engine_speak_start.argtypes = [ctypes.c_char_p, ctypes.c_int]
    L.osp_engine_speak_start.restype = ctypes.c_int
    L.osp_engine_pull.argtypes = [ctypes.c_char_p, ctypes.c_int]
    L.osp_engine_pull.restype = ctypes.c_int
    return side


def _pull_all(side, prepared, stamps=None, refuse_after=None):
    L = side.lib
    r = L.osp_engine_speak_start(prepared, len(prepared))
    assert r >= 0, "speak_start failed: %d" % r
    pieces = []
    buf = ctypes.create_string_buffer(1 << 20)
    t0 = time.perf_counter()
    while True:
        n = L.osp_engine_pull(buf, len(buf))
        if n <= 0:
            break
        pieces.append(buf.raw[:n])
        if stamps is not None:
            stamps.append(time.perf_counter() - t0)
        if refuse_after is not None and len(pieces) >= refuse_after:
            L.osp_engine_cancel()
    return pieces


def _refs():
    return [r for r in render_oracle.engine_refs()]


@pytest.mark.parametrize("kind", ["sp", "mtk2", "mtk3", "pro"])
def test_pulled_pieces_concatenate_to_the_blocking_render(host, kind):
    refs = [r for r in _refs() if r.kind == kind]
    if not refs:
        pytest.skip("%s not present" % kind)
    ref = refs[0]
    assert host.open(ref, ref.voices[0])
    try:
        host.select(ref.voices[0])
        host.set_rate(180); host.set_pitch(0, ref.voices[0]); host.set_inflection(50)
        for text in TEXTS:
            prepared = host.translate(text)
            # Twice, and compare the pulled render with the SECOND blocking
            # one: MacinTalk 2 carries something of the previous utterance
            # into the next -- measured on the Python reference, the first
            # "quick brown fox" after "a, b, c, d, e" is 8 bytes longer than
            # every repetition after it -- so like is compared with like, an
            # utterance in the same position as the one before it.
            host.speak(prepared)
            whole = host.speak(prepared)
            streamed = b"".join(_pull_all(host, prepared))
            assert streamed == whole, "%s %r: streamed %d bytes, whole %d" % (
                kind, text[:24], len(streamed), len(whole))
    finally:
        host.close()


@pytest.mark.parametrize("kind", ["mtk3", "pro"])
def test_the_first_piece_arrives_early_on_the_slow_engines(host, kind):
    refs = [r for r in _refs() if r.kind == kind]
    if not refs:
        pytest.skip("%s not present" % kind)
    ref = refs[0]
    assert host.open(ref, ref.voices[0])
    try:
        host.select(ref.voices[0])
        host.set_rate(180); host.set_pitch(0, ref.voices[0]); host.set_inflection(50)
        prepared = host.translate(TEXTS[3])
        t0 = time.perf_counter()
        host.speak(prepared)
        whole = time.perf_counter() - t0
        stamps = []
        pieces = _pull_all(host, prepared, stamps=stamps)
        assert len(pieces) > 4, "only %d pieces for a long utterance" % len(pieces)
        assert stamps[0] < whole / 3, "first piece at %.0f ms of a %.0f ms render" % (
            stamps[0] * 1000, whole * 1000)
    finally:
        host.close()


@pytest.mark.parametrize("kind", ["mtk3", "pro"])
def test_an_abandoned_utterance_stops_early_and_does_not_poison_the_next(host, kind):
    refs = [r for r in _refs() if r.kind == kind]
    if not refs:
        pytest.skip("%s not present" % kind)
    ref = refs[0]
    assert host.open(ref, ref.voices[0])
    try:
        host.select(ref.voices[0])
        host.set_rate(180); host.set_pitch(0, ref.voices[0]); host.set_inflection(50)
        long_text = host.translate(TEXTS[3])
        t0 = time.perf_counter()
        host.speak(long_text)
        whole = time.perf_counter() - t0
        short = host.translate("button")
        fresh = host.speak(short)
        t0 = time.perf_counter()
        pieces = _pull_all(host, long_text, refuse_after=1)
        stopped = time.perf_counter() - t0
        assert len(pieces) == 1, "kept going for %d pieces" % len(pieces)
        assert stopped < whole / 3, "abandoning took %.0f ms of a %.0f ms render" % (
            stopped * 1000, whole * 1000)
        assert host.speak(short) == fresh, "the abandoned utterance changed the next one"
    finally:
        host.close()
