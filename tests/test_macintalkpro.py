# -*- coding: utf-8 -*-
"""MacinTalk Pro: that it opens, takes a voice, and speaks.

Needs the engine and a voice, so it skips when `rom/macintalkpro` is empty.

Everything here was hard-won and is easy to break from a distance -- the CPU
type, the two forks, the resource names, the map offsets, the voice's file --
so each assertion names what it is protecting.
"""
import os

import pytest

#: These asserted no audio for four days, on purpose: the engine opened, took
#: a voice and ran its synthesis modules without a sound coming out, and
#: "every stage finished" is not the same claim as "it speaks". Both are
#: asserted now -- see macintalkpro.SPEAKS, which a person listening set.


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


def test_it_is_offered_to_nvda_now_that_it_speaks(pro):
    """The gate, and it is open since 2026-08-20.

    A synthesizer that lists a voice and then says nothing is worse than one
    that does not list it, so `usable` stayed False for four days while the
    engine opened, took a voice, ran its synthesis modules and made no sound.
    What was missing was never in the engine: an asynchronous `_Read` whose
    completion routine the host never called, and `_FixRatio` never served.

    This asserts the flag as well as the function, so flipping `SPEAKS` back
    to silence a failure fails here instead -- the flag means a person heard
    it, and a test cannot hear anything."""
    import paths
    import macintalkpro
    assert macintalkpro.SPEAKS is True
    assert macintalkpro.usable(paths.roots()) is True


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


def test_it_speaks_again_on_the_same_instance(pro):
    """NVDA's first difference from a probe is a second utterance.

    The scheduler puts every node back to its preset of 8 when an utterance
    ends, and under the old broken state a second `SpeakBuffer` gave six
    dispatches instead of twenty-one and stayed silent -- so reuse is a
    genuinely separate path, not an obvious consequence of the first one
    working. Three in a row, because the failure to look for is drift rather
    than an immediate stop."""
    from probe_pro_modules import (SPEAK, SET_INFO, SO_CURRENT_VOICE,
                                   SO_RATE, fixed)
    from probe_pro_open import TEXT_BUF, PARAM_BUF, VOICE_SPEC, build

    h, tok, voice, (reason, result) = build()
    assert reason == 1 and result == 0, "Open failed"
    creator = voice.creator.encode("mac-roman", "replace")
    h.w32(VOICE_SPEC, int.from_bytes(creator[:4].ljust(4, b" "), "big"))
    h.w32(VOICE_SPEC + 4, voice.id)
    h.component_call(tok, SET_INFO, [SO_CURRENT_VOICE, VOICE_SPEC])
    h.w32(PARAM_BUF, fixed(180))
    h.component_call(tok, SET_INFO, [SO_RATE, PARAM_BUF])

    sizes = []
    for text in (b"The first utterance.", b"And here is a second one.",
                 b"Three times on one instance."):
        h.pcm_reset()
        h.load(TEXT_BUF, text + b"\0")
        _r, res = h.component_call(tok, SPEAK, [TEXT_BUF, len(text), 0])
        assert res == 0, "SpeakBuffer %d returned %d" % (len(sizes) + 1, res)
        while h.buffers_taken < 400:
            if not h.run_callbacks(max_rounds=8):
                break
        pcm = h.pcm
        assert len(pcm) > 5000, \
            "utterance %d gave %d bytes" % (len(sizes) + 1, len(pcm))
        assert min(pcm) != max(pcm), "utterance %d is flat" % (len(sizes) + 1)
        sizes.append(len(pcm))
    assert h.fault_count == 0, "%d faults over three utterances" % h.fault_count
    # Longer text, more audio.  Drift would show as a stuck or shrinking
    # length while the text grows.
    assert sizes[0] < sizes[1] < sizes[2], "lengths went %r" % (sizes,)


# -- pitch ------------------------------------------------------------------
#
# 'pbas' is the same musical scale MacinTalk 2 uses: twelve units to the
# octave. Measured on Agnes, `tools/probe_pitch.py`: she answers 56, -12 gives
# 0.507 of the base frequency against a predicted 0.500, +6 gives 1.423
# against 1.414, +12 gives 1.982 against 2.000.
#
# Shared with MacinTalk 2's module rather than with a single pitch module,
# because two engines cannot be alive at once. See tests/pitchcheck.py.
import pitchcheck                                              # noqa: E402


def test_it_reports_a_musical_pitch(pro):
    pitchcheck.sane_base(pro[0])


def test_it_takes_a_pitch_offset(pro):
    pitchcheck.takes_the_offset(pro[0])


def test_the_pitch_slider_is_audible(pro):
    pitchcheck.slider_is_audible(pro[0])


def test_it_survives_the_top_of_the_pitch_slider(pro):
    """Regression: high 'pbas' used to halt the engine outright.

    Above roughly 'pbas' 69 -- which the top of the slider reaches for Agnes
    and Victoria -- MacinTalk Pro issues SANE `_FP68K` opword $0015 to clear
    its exception flags on the way out of a routine:

        clr.w   -$c(a6)
        pea     -$c(a6)
        move.w  #$0015,-(a7)
        _FP68K

    The host did not serve it, `sane_fail` took vector 10, and the utterance
    died partway through with "unhandled exception". Nothing had ever asked
    this engine for a high pitch before, so nothing had ever reached it.

    A longer phrase than the other pitch tests use, on purpose: the crash
    needed one. Short text survived all the way to 'pbas' 74 and hid it.
    """
    eng = pro[0]
    pitchcheck.live(eng)
    phrase = ("The rain in Spain falls mainly on the plain, "
              "and the pitch is set high enough to reach the edge.")
    for tenths in (120, 180, 240):
        eng.set_pitch(tenths)
        pcm = eng.speak(eng.translate(phrase))
        assert len(pcm) > 10000, (
            "%+d tenths produced %d samples -- the engine stopped early"
            % (tenths, len(pcm)))
    eng.set_pitch(0)


# -- streaming --------------------------------------------------------------
#
# **The slowest engine here**: about 17x realtime against MacinTalk 2's 194x,
# measured on the same text. A 26-second utterance took 1.53 s to render, and
# nothing could be played until all of it existed -- a second and a half of
# silence before a long sentence.


def test_streaming_produces_exactly_the_same_audio(pro):
    """The regression gate for the streaming path.

    Concatenating what the sink was handed must equal what the ordinary call
    returns, byte for byte, trimming included. "a, b, c, d, e" is in the list
    because it has real silence inside it four times over, which is exactly
    what a lookbehind can mistake for the end of the utterance.
    """
    eng = pro[0]
    for text in ["button", "s", "a, b, c, d, e", "   ",
                 "Voice testing, one two three.",
                 "Just posted a long thread about speech synthesis. " * 4]:
        prepared = eng.translate(text)
        whole = bytes(eng.speak(prepared))
        chunks = []
        eng.speak(prepared, sink=lambda b: chunks.append(bytes(b)))
        assert b"".join(chunks) == whole, (
            "%r: streamed %d bytes, whole %d"
            % (text[:30], len(b"".join(chunks)), len(whole)))


def test_a_long_utterance_is_no_longer_truncated(pro):
    """MacinTalk Pro's buffers are 1271 bytes, and the ceiling was 400.

    That is 23 seconds, and it was being hit: tripling a 366-character line
    gave 23.59 s of audio and doubling *that* gave 23.36, having simply
    stopped growing. Long paragraphs were cut off mid-word.
    """
    eng = pro[0]
    line = ("Provided arguments colon: left bracket, debug logging, dash f, "
            "C colon backslash Users backslash Tomi backslash App Data. ")
    short = len(bytes(eng.speak(eng.translate(line * 3))))
    longer = len(bytes(eng.speak(eng.translate(line * 6))))
    assert short / 22254.0 > 20, "%.1f s is too short to be testing a ceiling"
    assert longer > short * 1.7, (
        "doubling the text gave %.2f s against %.2f -- still truncating"
        % (longer / 22254.0, short / 22254.0))


def test_abandoning_a_render_really_abandons_it(pro):
    """Scrolling a timeline is nothing but this.

    A sink returning False means the listener has moved on. Before the engine
    was told to stop, the drain afterwards went on rendering anyway: giving up
    after the first piece still did 48 per cent of the work on a
    Mastodon-sized post, so scrolling past five spent seconds on audio nobody
    heard and every keystroke queued behind it.

    Pro cannot get as low as MacinTalk 3 does, because its SpeakBuffer
    analyses the whole text before returning a single buffer -- about 110 ms
    of a 340 ms render, and that part is not skippable.
    """
    import time
    eng = pro[0]
    prepared = eng.translate(
        "the esoteric programmer boosted David Tovey. Just posted a long "
        "thread about the history of speech synthesis on the Macintosh, and "
        "how much of it still runs today. Reply, boost, favourite. ")
    t0 = time.perf_counter()
    reference = bytes(eng.speak(prepared))
    whole = time.perf_counter() - t0

    t0 = time.perf_counter()
    out = eng.speak(prepared, sink=lambda c: False)
    stopped = time.perf_counter() - t0
    assert out == b""
    assert stopped < whole * 0.6, (
        "abandoning took %.0f ms of a %.0f ms render -- is StopSpeech still "
        "being issued before the drain?" % (stopped * 1000, whole * 1000))
    # The half of it that would actually be noticed: a stopped engine must not
    # leave anything behind for the next utterance to inherit.
    assert bytes(eng.speak(prepared)) == reference


# -- inflection ------------------------------------------------------------
#
# **The floor is the whole story here.** A 'pmod' near zero makes this engine
# loop forever inside SpeakBuffer on anything longer than one sentence, and
# the threshold belongs to the voice: 0.05 for Bruce, 0.025 for Agnes, 0 for
# Victoria. See `macintalkpro.set_inflection`.
import inflectioncheck                                         # noqa: E402


def test_it_reports_the_voices_own_modulation(pro):
    inflectioncheck.sane_base(pro[0])


def test_it_takes_the_inflection_setting(pro):
    import macintalkpro
    inflectioncheck.takes_the_setting(
        pro[0], floor=macintalkpro.INFLECTION_FLOOR)


def test_the_inflection_slider_is_audible(pro):
    inflectioncheck.slider_is_audible(pro[0])


def test_the_middle_of_the_inflection_slider_changes_nothing(pro):
    # `pristine=False`: the `spoken` fixture above re-initialises the emulator
    # underneath this one. See `inflectioncheck.the_midpoint_changes_nothing`.
    inflectioncheck.the_midpoint_changes_nothing(pro[0], pristine=False)


def test_the_floor_clears_every_threshold_that_was_measured():
    """Pure arithmetic, and deliberately not a render.

    Each of those thresholds cost a hung emulator and a hundred million
    instructions to find. This is the cheap guard that stops somebody lowering
    the constant toward "more monotone" without going back and repeating them.
    """
    import macintalkpro
    worst = 0.05                     # Bruce, on a four-sentence text
    assert macintalkpro.INFLECTION_FLOOR >= worst * 10, (
        "the floor is %r, which is not clear of the %r that hung Bruce"
        % (macintalkpro.INFLECTION_FLOOR, worst))


# Every Pro voice at both ends of the slider is the check that would actually
# have caught the hang -- Agnes survives depths that freeze Bruce -- but it
# cannot live here: **this engine can only ever speak the voice it was built
# with**, so covering all three means building three engines, and only one may
# be alive at a time. It runs through the driver instead, which is where
# rebuilding a Pro engine per voice is already handled:
# `test_driver.test_every_voice_speaks_at_both_ends_of_the_inflection_slider`.
