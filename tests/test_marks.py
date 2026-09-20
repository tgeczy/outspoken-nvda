# -*- coding: utf-8 -*-
"""Where an index is reported: on which side of its audio, and when.

An index is how NVDA hangs a callback on a point in the speech: the say-all
cursor, its own spelling-error sound, an add-on's earcon.  Until 2.0 this
driver reported every index it had collected before it rendered the run,
which put an earcon over the start of the words it was meant to follow.
Now an index goes on the audio queue on the side of the run's audio that
its place in the sequence says, and the feeder reports it when playback
reaches it -- the way NVDA's own eSpeak driver reports its markers.  No
outSPOKEN engine breathes, so unlike Panthera there is no checkbox and no
other mode.

The first half drives the render loop on a bare driver with a stand-in
engine that renders silence sized to the text, ten milliseconds a
character, so a test can tell which audio an index landed beside.  The
second half drives the feeder against the fake player, which keeps NVDA's
`onDone` timing.
"""
import queue
import threading
import time

import pytest

import nvwave
import synthDriverHandler
import outspoken
from outspoken import SynthDriver, _silence16


#: One second of audio per hundred characters.
MS_PER_CHAR = 10


class _Engine(object):
    """What `_flush` asks of an engine, answered with silence."""

    def __init__(self):
        self.pending = b""
        self.cancelled = False
        self.calls = []

    def numbers(self, mode):
        self.calls.append(("numbers", mode))

    def settings(self, rate, pitch, inflection):
        self.calls.append(("settings", rate, pitch, inflection))

    def apply(self, radj, padj):
        self.calls.append(("apply", radj, padj))

    def volume(self, percent, vadj):
        self.calls.append(("volume", percent, vadj))

    def translate(self, text):
        return text.encode("mac_roman", "replace")

    def speak_start(self, prepared):
        self.cancelled = False
        frames = int(22254 * MS_PER_CHAR * len(prepared) / 1000)
        self.pending = bytes([0x80]) * frames
        return bool(frames)

    def pull(self):
        piece, self.pending = self.pending, b""
        return b"" if self.cancelled else piece

    def cancel(self):
        self.cancelled = True

    def widen(self, pcm8):
        return bytes(len(pcm8) * 2)


def _bare(player=None):
    """A driver with only the parts the render loop and the feeder touch."""
    d = SynthDriver.__new__(SynthDriver)
    d._queue = queue.Queue()
    d._audioQueue = queue.Queue()
    d._stopped = False
    d._cancels = 0
    d._audioOut = False
    d._numberWords = True
    d._rate = d._pitch = d._inflection = 50
    d._volume = 100
    d._nSpoken = d._nEmpty = 0
    d._lastReport = 0.0
    d._player = player if player is not None else nvwave.WavePlayer()
    d._markLock = threading.Lock()
    d._marks = []
    d._markGen = 0
    d._fedBytes = d._playedBytes = 0
    d._playerTakesOnDone = d._probeOnDone()
    d._engineRef = _Engine()
    d._sync = lambda: d._engineRef
    return d


def _drain(q):
    out = []
    while True:
        try:
            out.append(q.get_nowait())
        except queue.Empty:
            return out


def _shape(items):
    """The audio queue as (kind, value): audio reduced to its length in
    bytes, and the generation tag dropped, so an assertion can say "the
    first part's audio" and mean it."""
    out = []
    for item in items:
        if item[0] == "audio":
            out.append(("audio", len(item[1])))
        else:
            out.append((item[0], item[1]))
    return out


def _run(d, items, timeout=3.0):
    """Render one sequence on the worker's loop and return the audio queue
    as `_shape` sees it, ending with the completion item."""
    d._queue.put((list(items), time.perf_counter()))
    worker = threading.Thread(target=d._render)
    worker.start()
    got = []
    deadline = time.perf_counter() + timeout
    while time.perf_counter() < deadline:
        try:
            got.append(d._audioQueue.get(timeout=0.05))
        except queue.Empty:
            continue
        if got[-1][0] == "done":
            break
    d._queue.put(None)
    worker.join(timeout=2.0)
    assert not worker.is_alive(), "the render loop did not stop"
    return _shape(got)


def _ms(n_chars):
    return len(_silence16(MS_PER_CHAR * n_chars))


#: The shape the Earcons and Speech Rules add-on sends, and the shape NVDA's
#: own spelling-error sound takes when a break follows it: a callback after
#: some words, then a pause for the sound, then the rest.
EARCON = [("text", "first part "), ("index", 7), ("break", 250),
          ("text", "second part")]


# -- the render loop: which side of the audio an index lands on ------------

def test_an_index_before_a_break_follows_the_words_before_it():
    out = _run(_bare(), EARCON)
    assert out == [("audio", _ms(11)), ("index", 7),
                   ("audio", len(_silence16(250))), ("audio", _ms(11)),
                   ("done", None)], out


def test_an_index_with_text_on_both_sides_stays_at_the_head():
    """Its position in the audio is not known without the engine's help --
    the `[[sync]]` callback the host does not yet install -- so it is
    reported where it always was.  The day the host can say where it got
    to, this is the test to change."""
    out = _run(_bare(), [("text", "a "), ("index", 7), ("text", "b")])
    assert out == [("index", 7), ("audio", _ms(3)), ("done", None)], out


def test_a_head_index_precedes_the_run_and_an_end_index_follows_it():
    """NVDA's say-all puts an index at the start of a line and its manager
    one at the end of every utterance; the first keeps say-all a line
    ahead, the second is what asks for the next utterance, and each is
    reported where it belongs."""
    out = _run(_bare(), [("index", 1), ("text", "The rain in Spain."),
                         ("index", 2)])
    assert out == [("index", 1), ("audio", _ms(18)), ("index", 2),
                   ("done", None)], out


def test_indexes_with_no_words_are_reported_and_the_utterance_completes():
    out = _run(_bare(), [("index", 3), ("text", "  "), ("index", 4)])
    assert out == [("index", 3), ("index", 4), ("done", None)], out


def test_spelling_makes_each_character_its_own_utterance():
    out = _run(_bare(), [("spell", True), ("text", "h"), ("text", "i"),
                         ("spell", False), ("text", "there")])
    assert out == [("audio", _ms(1)), ("audio", _ms(1)), ("audio", _ms(5)),
                   ("done", None)], out


def test_a_cancel_mid_render_abandons_the_rest_and_loses_no_index():
    """The cancel lands *during* the render of the first part -- the real
    case -- so that audio is tagged stale, the rest of the sequence is not
    rendered, and the index that trailed the first part still goes out."""
    d = _bare()
    eng = d._engineRef
    pull = eng.pull

    def cancelledMidRender():
        d._cancels += 1                 # cancel() arrived while rendering
        return pull()
    eng.pull = cancelledMidRender
    d._queue.put((list(EARCON), time.perf_counter()))
    worker = threading.Thread(target=d._render)
    worker.start()
    time.sleep(0.3)
    d._queue.put(None)
    worker.join(timeout=2.0)
    raw = _drain(d._audioQueue)
    assert [v for k, v, *_ in raw if k == "index"] == [7]
    live = [(k, len(v)) for k, v, *t in raw if k == "audio" and t[0] == d._cancels]
    assert not live, "audio from the cancelled run would play: %r" % (live,)
    assert not any(k == "audio" and len(v) == len(_silence16(250))
                   for k, v, *_ in raw), "the break after the cancel was queued"


# -- the feeder: when a mark is reported --------------------------------------

class _Reports(object):
    """Timestamps of every index reported, in order."""

    def __init__(self, monkeypatch):
        self.seen = []
        original = outspoken.synthIndexReached.notify

        def notify(**k):
            self.seen.append((time.perf_counter(), k.get("index")))
            original(**k)
        monkeypatch.setattr(outspoken.synthIndexReached, "notify", notify)


def _feed(d, items):
    for item in items:
        d._audioQueue.put(item)
    d._audioQueue.put(None)
    t0 = time.perf_counter()
    d._feed()
    return t0


#: Four tenths of a second of audio: long enough that being fed and being
#: heard are visibly different moments.
AUDIO = _silence16(400)


def test_a_mark_is_reported_when_its_audio_has_played(monkeypatch):
    r = _Reports(monkeypatch)
    d = _bare()
    t0 = _feed(d, [("audio", AUDIO, 0), ("index", 5), ("done", None)])
    assert [i for _t, i in r.seen] == [5]
    at = r.seen[0][0] - t0
    # The fake device starts a stream 120 ms after its first feed and the
    # audio lasts 400 ms, so the mark is due at 520 ms at the earliest.
    assert at >= 0.50, "reported %.0f ms in, before the audio ended" % (at * 1000)


def test_a_mark_with_nothing_playing_is_reported_at_once(monkeypatch):
    r = _Reports(monkeypatch)
    d = _bare()
    t0 = _feed(d, [("index", 9), ("done", None)])
    assert [i for _t, i in r.seen] == [9]
    assert r.seen[0][0] - t0 < 0.05


class _PlayerWithoutCallbacks(nvwave.WavePlayer):
    """NVDA's player as it was before it could say when a chunk had played."""

    def feed(self, data):
        nvwave.WavePlayer.feed(self, data)


def test_a_mark_never_waits_on_a_player_that_cannot_call_back(monkeypatch):
    """The manifest admits NVDA older than the callback.  There a mark is
    reported the moment it is dequeued -- what every index did before --
    and never lost."""
    r = _Reports(monkeypatch)
    d = _bare(player=_PlayerWithoutCallbacks())
    assert not d._playerTakesOnDone
    t0 = _feed(d, [("audio", AUDIO, 0), ("index", 5), ("done", None)])
    assert [i for _t, i in r.seen] == [5]
    assert r.seen[0][0] - t0 < 0.45


def test_every_mark_is_reported_before_the_utterance_is_declared_done(monkeypatch):
    r = _Reports(monkeypatch)
    done = []
    original = outspoken.synthDoneSpeaking.notify

    def notify(**k):
        done.append((time.perf_counter(), [i for _t, i in r.seen]))
        original(**k)
    monkeypatch.setattr(outspoken.synthDoneSpeaking, "notify", notify)
    d = _bare()
    _feed(d, [("audio", AUDIO, 0), ("index", 1), ("audio", AUDIO, 0),
              ("index", 2), ("done", None)])
    assert done and done[0][1] == [1, 2], \
        "done was declared with %r reported" % (done[0][1],)


def test_marks_are_reported_in_order_and_each_after_its_own_audio(monkeypatch):
    r = _Reports(monkeypatch)
    d = _bare()
    t0 = _feed(d, [("audio", AUDIO, 0), ("index", 1), ("audio", AUDIO, 0),
                   ("index", 2), ("done", None)])
    assert [i for _t, i in r.seen] == [1, 2]
    first, second = (t - t0 for t, _i in r.seen)
    assert first >= 0.50 and second >= 0.90, (first, second)
    assert second - first >= 0.30, "the second mark did not wait for its audio"


def test_stale_audio_is_dropped_by_the_feeder(monkeypatch):
    """A piece rendered under a cancel count that has since moved on was
    put after cancel() drained the queue; it must not sound."""
    d = _bare()
    d._cancels = 3
    _feed(d, [("audio", AUDIO, 2), ("audio", AUDIO, 3), ("done", None)])
    assert d._player.fed == 1 and d._player.bytes == len(AUDIO)


def test_a_cancel_drops_held_marks_and_retires_their_callbacks(monkeypatch):
    """After cancel() the device holds nothing of ours, so a callback from
    the audio it threw away must not count against what comes next."""
    r = _Reports(monkeypatch)
    d = _bare()
    d._feedPiece(AUDIO)
    d._markReached(4)
    assert d._marks and not r.seen
    old = d._markGen
    d._resetMarks()                     # what cancel() does
    assert not d._marks and d._fedBytes == 0 and d._playedBytes == 0
    d._played(old, len(AUDIO))          # the retired callback arriving late
    assert d._playedBytes == 0, "a stale callback moved the played count"
    assert not r.seen, "a cancelled mark was reported"


def test_the_drain_reports_whatever_a_silent_device_left_behind(monkeypatch):
    """A player that never calls back still loses no index."""
    r = _Reports(monkeypatch)
    d = _bare()
    d._fedBytes = len(AUDIO)
    d._markReached(6)
    assert d._marks == [(len(AUDIO), 6)]
    d._marksDrained()
    assert [i for _t, i in r.seen] == [6]
    assert d._fedBytes == 0 and d._playedBytes == 0 and not d._marks
