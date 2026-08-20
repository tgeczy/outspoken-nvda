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


@pytest.fixture(scope="module")
def spoken(pro):
    """One whole utterance through the real engine.

    Speaking is asynchronous: `SpeakBuffer` queues the first buffer and
    returns, and everything after it only exists if the host keeps being the
    Sound Manager. Shares the `pro` fixture's skip, so this whole group
    disappears when the engine is not extracted."""
    import probe_pro_modules
    h, _voice = probe_pro_modules.speak(text=b"Hello, this is MacinTalk Pro.")
    order, _seen = probe_pro_modules.modules(h)
    # Read the pipeline out BEFORE pumping: once the utterance finishes the
    # scheduler puts every node back to its preset of 8 for the next one, so a
    # state read at the end of the run says nothing about what happened during
    # it. Reading a byte too late is how two landmarks got into the notes.
    pipeline = [(h.r8(n + 0x3E + 0x0B), h.r32(n + 0x3E + 0x10),
                 h.r32(n + 0x3E + 0x14)) for _e, n in order]
    while h.buffers_taken < 400:
        if not h.run_callbacks(max_rounds=8):
            break
    return h, order, pipeline



def test_the_asynchronous_lexicon_read_gets_its_completion(spoken):
    """**Pro reads its lexicon with `_Read` and the async bit set ($A402)**,
    parks the module that asked, and waits for the routine in ioCompletion to
    wake it. Copying the bytes and returning is only half the service.

    Without the completion the engine sleeps forever and nothing downstream of
    the phoneme stage ever runs -- and every field of the param block still
    reads back correct, so nothing in the engine can tell. This is the same
    rule as "a stubbed trap is a lie the caller cannot detect", one step on:
    **so is a trap answered synchronously when it was asked asynchronously.**
    """
    h, _order, _pipeline = spoken
    runs, dropped = h.completions
    assert runs > 0, "no completion routine ran; the lexicon lookups are lost"
    assert dropped == 0, "the completion queue overflowed -- a dropped " \
                         "callback is a module asleep forever"


def test_the_phoneme_stage_consumes_all_of_its_input(spoken):
    """Module #1 is `EnglPhon`, and it either finishes or the pipeline stops
    dead behind it.

    The scheduler's node picker skips any node whose state is 2, 3 or 4 before
    it even looks at the pipes, and ends the pass at the first node in state 6.
    EnglPhon suspending itself at a lexicon lookup (state 3) therefore parks
    the whole engine, which is what 93 of 99 units consumed used to mean."""
    _h, _order, pipeline = spoken
    state, avail, used = pipeline[1]              # #1 is EnglPhon
    assert avail > 0, "the text module produced nothing to work on"
    assert used == avail, \
        "EnglPhon left %d of %d units unconsumed" % (avail - used, avail)
    assert state == 6, "EnglPhon reports state %d, not 6 (finished)" % state


def test_every_stage_of_the_pipeline_finishes(spoken):
    """The live chain is Cmd -> Phon -> Allo -> Wave -> Snd, and each stage
    only starts when the one before it hands something on.

    `*XPh` and `*XAl` are the phoneme-input alternates and are correctly never
    primed, so they stay at the scheduler's preset of 8."""
    _h, _order, pipeline = spoken
    names = ["*Cmd", "EnglPhon", "*XPh", "*Wave", "*XAl", "EnglAllo", "*Snd"]
    for i in (0, 1, 3, 5):
        state = pipeline[i][0]
        assert state == 6, "%s reports state %d, not 6" % (names[i], state)


def test_it_produces_speech_shaped_audio(spoken):
    """MacinTalk Pro speaking, which is the whole point.

    Not "some bytes": a buffer the engine cleared and never filled is flat,
    and flat is what every earlier attempt produced. This checks the samples
    move, which neither silence nor a stuck DC level can fake.

    Two missing services were between here and that. `_FixRatio` unserved was
    worth twenty million out-of-range reads and no audio at all, and the
    unanswered asynchronous lexicon read was worth nothing past the phoneme
    stage. Both looked like the engine failing."""
    h, _order, _pipeline = spoken
    pcm = h.pcm
    assert h.fault_count == 0, "%d memory faults while synthesising" % h.fault_count
    assert len(pcm) > 10000, "only %d bytes of audio" % len(pcm)
    lo, hi = min(pcm), max(pcm)
    assert lo != hi, "flat at %d -- cleared buffers, never filled" % lo
    assert hi - lo > 60, "range %d..%d is too small to be speech" % (lo, hi)
    live = sum(1 for c in pcm if c != 0x80)
    assert live > len(pcm) // 4,         "only %d of %d samples are not silence" % (live, len(pcm))
