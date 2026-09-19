# -*- coding: utf-8 -*-
"""An engine can speak all day: the heap it allocates per utterance comes back.

Found 2026-09-19 by the render oracle's full grid, which wedged on its 133rd
render.  The 1984 driver allocates one block per utterance and disposes of
it afterwards, and MacinTalk Pro allocates and disposes about 57 KB per
utterance -- and the host's dispose traps did nothing, so the 512 KB heap
was full after 123 short utterances and Pro's 10 MB after about 140.  Then
_NewPtr answered memFullErr, the engine spun until its instruction budget
and every utterance after it was silent.  Nobody had spoken that many in one
sitting on one voice in a test; users would have.

These need the engine, so they skip without it.
"""
import pytest


def _sp(rom_files):
    import engine
    e = engine.Engine(rom_files)
    e.set_rate(180)
    e.set_voice(110)
    return e


def test_the_1984_engine_speaks_two_hundred_utterances_without_growing(rom_files):
    e = _sp(rom_files)
    try:
        texts = [e.translate(t) for t in ("Hello.", "The quick brown fox jumps over 12 lazy dogs.",
                                          "The 3rd of 1,234 items, at 25.5%, minus -7.")]
        first = e.speak(texts[0])
        used = e.h.lib.osp_heap_used()
        for i in range(1, 200):
            pcm = e.speak(texts[i % 3])
            assert e.h.stop == 1, "utterance %d did not finish (stop %d)" % (i, e.h.stop)
            assert pcm, "utterance %d rendered nothing" % i
            if i % 3 == 0:
                assert pcm == first, "utterance %d differs from the first" % i
        assert e.h.lib.osp_heap_used() == used, "the heap grew by %d bytes" % (
            e.h.lib.osp_heap_used() - used)
    finally:
        e.close()


def test_macintalk_pro_speaks_forty_utterances_without_growing():
    import paths
    import macintalkpro
    roots = paths.roots()
    _folder, voices = macintalkpro.find(roots)
    if not voices:
        pytest.skip("MacinTalk Pro not present")
    v = voices[0]
    e = macintalkpro.Engine(macintalkpro.engine_dir(roots, v.creator), voices, v)
    try:
        e.set_rate(180)
        text = e.translate("The quick brown fox jumps over 12 lazy dogs.")
        first = e.speak(text)
        used = e.h.lib.osp_heap_used()
        for i in range(1, 40):
            assert e.speak(text) == first, "utterance %d differs from the first" % i
        assert e.h.lib.osp_heap_used() == used, "the heap grew by %d bytes" % (
            e.h.lib.osp_heap_used() - used)
    finally:
        e.close()
