# -*- coding: utf-8 -*-
"""Classic MacinTalk 3: that it opens, takes nineteen voices, and speaks.

Needs the engine and its voices, so it skips when `rom/macintalk3` is empty.

Its own module, and it has to stay that way: `osp_init()` resets the
emulator's globals and `Host.close()` drains the DLL's reference count, so a
second engine built alongside this one turns teardown into an access
violation. See tests/pitchcheck.py.

**The bug this engine cost a week to find is worth stating here**, because
every assertion below is downstream of it. It rendered two buffers and then
ran away into four hundred million bus faults, and the recorded diagnosis --
that the host needed a Sound Manager command queue -- was wrong; the host had
been copying commands into CB_SCRATCH all along. It was `_Microseconds`, which
returns its count in the A0/D0 pair and writes no memory. The host stored
eight bytes through A0, which held whatever the caller left there; here that
was the engine's own SndCommand, and the callback trampoline then walked the
overwritten `param2` and jumped through it. The clock was corrupting the thing
it was timing.
"""
import pytest

import pitchcheck

PHRASE = "Voice testing, one two three."
RATE = 200

#: Nine of the nineteen carry wave data and **the engine refuses them without
#: it** -- -192, resNotFound, rather than going quiet. Measured, all nineteen:
#: these nine name a `ttvw` in their own `ttvd` and the other ten read 1 there.
NEEDS_WAVE = {"Albert", "Bahh", "Bells", "Boing", "Bubbles", "Cellos",
              "Deranged", "Hysterical", "Pipe Organ"}


@pytest.fixture(scope="module")
def mt3():
    import paths
    import macintalk3
    folder, voices = macintalk3.find(paths.roots())
    if not folder or not voices:
        pytest.skip("MacinTalk 3 is not extracted; run tools/extract_rom.py")
    eng = macintalk3.Engine(folder, voices, voices[0])
    eng.set_rate(RATE)
    yield eng, voices
    eng.close()


def test_it_offers_every_voice_the_image_had(mt3):
    """Nineteen, and the novelty ones are the point.

    A formant voice is a parameter set, so MacinTalk 3 brings Bells, Boing,
    Cellos, Pipe Organ and Zarvox for almost no space -- voices no other
    engine here has.
    """
    _eng, voices = mt3
    names = {v.name for v in voices}
    assert len(voices) == 19, sorted(names)
    assert NEEDS_WAVE <= names
    assert {"Fred", "Kathy", "Princess", "Ralph", "Junior"} <= names


def test_the_wave_voices_declare_their_own_wave(mt3):
    """The gate is data-driven, not a list of nine names kept in a file.

    Each `ttvd` names its `ttvw` at offset 648, and the ten that need none
    read 1 there. `voices.describe` turns that into `ttvw_id`, and
    `voice_incomplete` refuses a folder whose wave is missing -- which matters
    because the engine's own refusal comes far too late to keep the voice out
    of NVDA's list.
    """
    _eng, voices = mt3
    for v in voices:
        wants = v.ttvw_id is not None
        assert wants == (v.name in NEEDS_WAVE), (
            "%s says ttvw_id=%r" % (v.name, v.ttvw_id))
        if wants:
            assert "ttvw" in v.files, "%s has no wave beside it" % v.name


def test_every_voice_speaks_and_none_sound_the_same(mt3):
    """The whole of what "supported" means, and it is one assertion twice.

    Distinct bytes rather than a pitch measurement: two voices rendering
    identically is what a mis-registered resource looks like, and it would
    pass any test that only asked whether audio came out.

    All nineteen on one instance, because that is the model -- 45 of the
    host's 64 resource slots hold the engine and every voice at once, so
    changing voice is one SetSpeechInfo('cvox') and never a rebuild.
    """
    eng, voices = mt3
    pitchcheck.live(eng)
    seen = {}
    for v in voices:
        assert eng.select(v), "the engine refused %s" % v.name
        eng.set_rate(RATE)
        pcm = bytes(eng.speak(eng.translate(PHRASE)))
        assert len(pcm) > 4000, "%s rendered %d bytes" % (v.name, len(pcm))
        seen.setdefault(pcm, []).append(v.name)
    dupes = [names for names in seen.values() if len(names) > 1]
    assert not dupes, "voices that render identically: %r" % dupes


def test_the_powerpc_build_is_not_registered(mt3):
    """`ttvi 11` is the same engine compiled for PowerPC, 102 KB of it.

    It sits in the same file as the 68k build because Apple shipped one fat
    extension. Registering it wastes a resource slot in a table with 64, and
    this host cannot run a byte of it.
    """
    import macintalk3
    assert ("ttvi", 11) in macintalk3.SKIP


# -- pitch ------------------------------------------------------------------
def test_it_reports_a_musical_pitch(mt3):
    pitchcheck.sane_base(mt3[0])


#: The lowest pitch this engine will hold, whatever it is asked for, and how
#: far its stored value wanders from the one it was given. Both measured with
#: `tools/probe_pitch.py`: asked 10, 20, 25, 28, 30 and 31 it answers 31.34766
#: every time; asked 40 it holds 40.020 and asked 60 it holds 59.988, because
#: it takes the value through a frequency and back.
PITCH_FLOOR = 31.34766
PITCH_TOL = 0.05


def test_it_takes_a_pitch_offset(mt3):
    pitchcheck.takes_the_offset(mt3[0], tol=PITCH_TOL, floor=PITCH_FLOOR)


def test_the_pitch_floor_is_the_engine_s_and_not_a_fault(mt3):
    """Asked below 31.348 it holds 31.348, and that is a real answer.

    It matters to the driver only at the very bottom of the slider and only
    for the lower voices -- Fred's own pitch is 42.785, so an octave down is
    30.79 and lands half a semitone above where he was aimed. Inaudible, and
    not worth a clamp of our own; worth a test so nobody later reads it as
    the selector being ignored.
    """
    eng, voices = mt3
    pitchcheck.live(eng)
    assert eng.select(next(v for v in voices if v.name == "Fred"))
    import macintalk3
    for asked in (10.0, 20.0, 28.0, 31.0):
        eng._set_info(macintalk3.SO_PITCH_BASE, eng._fixed_arg(asked))
        assert abs(eng.current_pitch() - PITCH_FLOOR) < 0.01, (
            "asked %.1f, holds %.5f" % (asked, eng.current_pitch()))
    eng._set_info(macintalk3.SO_PITCH_BASE, eng._fixed_arg(50.0))
    assert abs(eng.current_pitch() - 50.0) <= PITCH_TOL
    eng._base_pitch = None
    eng.set_pitch(0)


def test_the_pitch_slider_is_audible(mt3):
    pitchcheck.slider_is_audible(mt3[0])


def test_each_voice_brings_its_own_pitch(mt3):
    """Which is why the slider is an offset rather than an absolute.

    Measured: Hysterical sits near 31 and Good News near 59, most of an octave
    apart, so one absolute scale would put the middle of the slider in a
    different place for each of them.
    """
    eng, voices = mt3
    pitchcheck.live(eng)
    bases = {}
    for v in voices:
        assert eng.select(v)
        bases[v.name] = eng.base_pitch()
    assert len(set(bases.values())) > 5, bases
    assert min(bases.values()) < 40 < max(bases.values())


def test_the_pitch_offset_does_not_compound_across_voices(mt3):
    """The property that makes dropping the cached base safe.

    `set_pitch` works from the voice's own pitch and remembers it; `select`
    drops that memory. That is only sound if taking a voice resets the
    channel's pitch to the new voice's own -- otherwise the next question
    returns our own offset and the pitch climbs an octave per voice change.
    It is a property of the engine, so it is measured here rather than
    inherited from MacinTalk 2.
    """
    eng, voices = mt3
    pitchcheck.live(eng)
    a = next(v for v in voices if v.name == "Fred")
    b = next(v for v in voices if v.name == "Kathy")

    assert eng.select(a)
    natural = eng.current_pitch()
    for _round in range(3):
        eng.set_pitch(120)
        assert eng.select(b)
        eng.set_pitch(120)
        assert eng.select(a)
        eng.set_pitch(120)
        assert abs(eng.current_pitch() - (natural + 12.0)) < 0.01, (
            "an octave above %.3f drifted to %.3f"
            % (natural, eng.current_pitch()))
    eng.set_pitch(0)


# -- streaming --------------------------------------------------------------
#
# This engine renders at about 24x realtime where MacinTalk 2 manages 157x, so
# a long sentence took the best part of a second before a sample of it could
# be played. `speak(sink=...)` hands each piece over as it is rendered.

STREAM_TEXTS = ["button", "s", "a, b, c, d, e",
                "Voice testing, one two three.", "   ",
                "Provided arguments colon: left bracket, debug logging. " * 3]


def test_streaming_produces_exactly_the_same_audio(mt3):
    """The regression gate for the whole streaming path.

    Concatenating what the sink was handed must equal what the ordinary call
    returns, byte for byte, including the trimming at both ends. "a, b, c, d,
    e" is in the list on purpose: it has real silence *inside* it, four times
    over, which is what the lookbehind has to not mistake for the end.
    """
    eng, _voices = mt3
    pitchcheck.live(eng)
    for text in STREAM_TEXTS:
        prepared = eng.translate(text)
        whole = bytes(eng.speak(prepared))
        chunks = []
        eng.speak(prepared, sink=lambda b: chunks.append(bytes(b)))
        assert b"".join(chunks) == whole, (
            "%r: streamed %d bytes, whole %d"
            % (text[:30], len(b"".join(chunks)), len(whole)))


def test_streaming_hands_over_the_first_piece_early(mt3):
    """The point of it. A long utterance must not be silent while it renders.

    Asserted as a fraction of the whole render rather than a millisecond
    count, so it does not become a speed test of whatever machine runs it.
    """
    import time
    eng, _voices = mt3
    pitchcheck.live(eng)
    prepared = eng.translate(
        "Provided arguments colon: left bracket, debug logging. " * 3)
    t0 = time.perf_counter()
    eng.speak(prepared)
    whole = time.perf_counter() - t0

    stamps = []
    t0 = time.perf_counter()
    eng.speak(prepared, sink=lambda b: stamps.append(time.perf_counter() - t0))
    assert len(stamps) > 4, "only %d pieces for a long utterance" % len(stamps)
    assert stamps[0] < whole / 3, (
        "first piece at %.0f ms of a %.0f ms render"
        % (stamps[0] * 1000, whole * 1000))


def test_a_sink_that_says_no_stops_the_render(mt3):
    """How a cancel reaches an utterance already being rendered.

    Returning False means the caller has lost interest. Without it a cancelled
    long sentence goes on rendering for most of a second and then plays.
    """
    import time
    eng, _voices = mt3
    pitchcheck.live(eng)
    prepared = eng.translate(
        "Provided arguments colon: left bracket, debug logging. " * 3)
    t0 = time.perf_counter()
    eng.speak(prepared)
    whole = time.perf_counter() - t0

    seen = []

    def refuse(chunk):
        seen.append(chunk)
        return False                        # give up after the first piece

    t0 = time.perf_counter()
    out = eng.speak(prepared, sink=refuse)
    stopped = time.perf_counter() - t0
    assert len(seen) == 1, "kept going for %d pieces" % len(seen)
    assert out == b"", "an aborted render still returned audio"
    assert stopped < whole / 2, (
        "abandoning took %.0f ms of a %.0f ms render"
        % (stopped * 1000, whole * 1000))
    # And the engine is still usable afterwards, which is the part that would
    # actually be noticed: an abandoned utterance must not poison the next.
    assert len(bytes(eng.speak(eng.translate("button")))) > 4000
