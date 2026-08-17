# -*- coding: utf-8 -*-
"""MacinTalk Pro: that it opens at all, and takes a voice.

Needs the engine and a voice, so it skips when `rom/macintalkpro` is empty.

Everything here was hard-won and is easy to break from a distance -- the CPU
type, the two forks, the resource names, the map offsets, the voice's file --
so each assertion names what it is protecting.
"""
import os

import pytest

#: What speaking would need on top of this, and does not have yet. These tests
#: deliberately do not assert audio: the engine opens, takes a voice and runs
#: its synthesis modules, and that is still not the same as making a sound.
#: See macintalkpro.SPEAKS.


@pytest.fixture(scope="module")
def pro():
    import paths
    import macintalkpro
    folder, voices = macintalkpro.find(paths.roots())
    if not folder or not voices:
        pytest.skip("MacinTalk Pro is not extracted; run tools/extract_rom.py")
    eng = macintalkpro.Engine(folder, voices, voices[0])
    yield eng, voices, folder
    eng.close()


def test_the_extractor_kept_the_names_and_map_offsets(pro):
    """**Pro looks its modules up by name**, and asks the Resource Manager
    where each sits in the map before reading it out of the file. An
    extraction that kept only `type_id.bin` is an engine that cannot start,
    and the failure it produces is resNotFound several calls away from the
    cause."""
    import macintalkpro
    _eng, _voices, folder = pro
    index = macintalkpro.read_index(folder)
    assert index, "no resources.tsv -- re-run tools/extract_rom.py"
    entry, name = index[("gtse", 1)]
    assert name == "*TTS", "the engine's own code is named *TTS"
    assert entry > 0, "gtse 1 has no map offset"
    # Every named module the language configuration lists.
    names = {nm for _e, nm in index.values()}
    for mod in ("*TTS", "*Wave", "*Snd", "*Lex", "*Cmd", "EnglPhon",
                "EnglAllo"):
        assert mod in names, "%s missing from the index" % mod


def test_both_forks_were_kept(pro):
    """The lexicon is in the engine's DATA fork and a voice's units are in the
    VOICE file's RESOURCE fork, and Pro reads both as files."""
    _eng, voices, folder = pro
    df = os.path.join(folder, "datafork.bin")
    assert os.path.isfile(df), "the engine's data fork is missing"
    assert os.path.getsize(df) > 500000, "the lexicon is 572,928 bytes"
    rf = os.path.join(voices[0].folder, "rsrcfork.bin")
    assert os.path.isfile(rf), "the voice's resource fork is missing"
    assert os.path.getsize(rf) > 100000, "a voice fork is hundreds of KB"


def test_it_opens_and_holds_the_voice_it_was_given(pro):
    """Open returning noErr took a 68020, an eight-byte exception frame, the
    resource names, a File Manager for its own file, and TopMapHndl. Taking
    the voice took the FSSpec naming the voice's file."""
    eng, voices, _folder = pro
    assert eng.voice is not None
    assert eng.voice.creator == "gala"
    assert eng.voice.name == voices[0].name


def test_it_will_not_pretend_to_hold_another_voice(pro):
    """One voice per Engine: the host has 64 resource slots, Pro takes 50 and
    a voice another ten. Saying yes to a voice it does not have would leave
    the driver believing a switch happened -- the driver rebuilds instead."""
    eng, voices, _folder = pro
    if len(voices) < 2:
        pytest.skip("only one MacinTalk Pro voice extracted")
    other = next(v for v in voices if v.name != eng.voice.name)
    assert eng.select(other) is False
    assert eng.voice.name != other.name, "it kept the voice it really has"


def test_it_is_not_offered_to_nvda_until_it_speaks(pro):
    """The gate. A synthesizer that lists a voice and then says nothing is
    worse than one that does not list it, so `usable` stays False until a
    probe has written a WAV somebody heard."""
    import paths
    import macintalkpro
    if macintalkpro.SPEAKS:
        pytest.skip("SPEAKS is on; audio is expected to work now")
    assert macintalkpro.usable(paths.roots()) is False
