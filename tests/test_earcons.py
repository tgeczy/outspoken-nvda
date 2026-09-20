"""The Earcons and Speech Rules add-on, and where its index lands.

The add-on (formerly Phonetic Punctuation, ``C:\\git\\nvda-phonetic-punctuation``)
turns a punctuation mark, a role or a state into a callback command -- an
``IndexCommand`` by the time it reaches a driver -- **followed by a
``BreakCommand`` sized to the sound**, so the speech pauses while the earcon
plays on the add-on's own player.  ``text | index | break | text`` is its
whole shape, and the one thing it needs from us is that the index is reported
when the text before it has been spoken, not before.

This driver reports every collected index before it renders the run
(``_flush``), which is right for a say-all line marker and wrong here: the
earcon sounds over the start of the word and the silence lands after it.
Panthera has the same fault with breathing on, and cannot fix it without
losing the breath.  No outSPOKEN engine breathes, so outSPOKEN can report at
playback position unconditionally -- see ``docs/release-2.0.0.md``, "Asked
for along the way".

The test below asserts the required order.  It was an expected failure
until 2.0's driver put indexes on the audio queue and reported them where
their audio is heard; it passes since, and must keep passing.
"""

import time

import pytest


def _record(driver, synthDriverHandler, order):
    """Log feeds and index reports, in the order the threads make them."""
    player = driver._player
    orig_feed = player.feed
    notifier = synthDriverHandler.synthIndexReached
    orig_notify = notifier.notify

    def feed(data, *a, **k):
        order.append(("feed", len(data)))
        return orig_feed(data, *a, **k)

    def notify(**kw):
        order.append(("index", kw.get("index")))
        return orig_notify(**kw)

    player.feed = feed
    notifier.notify = notify
    return lambda: (setattr(player, "feed", orig_feed),
                    setattr(notifier, "notify", orig_notify))


def test_an_earcon_index_follows_the_text_before_it(driver, rom_files):
    """``hello | index | break | world``: the index after ``hello``'s audio."""
    import speech.commands
    import synthDriverHandler
    order = []
    restore = _record(driver, synthDriverHandler, order)
    try:
        driver.speak(["hello", speech.commands.IndexCommand(7),
                      speech.commands.BreakCommand(300), "world"])
        t0 = time.perf_counter()
        # Three feeds are due -- hello, the break's silence, world -- and
        # the index, which waits for hello to finish sounding.
        while ((sum(1 for k, _v in order if k == "feed") < 3
                or ("index", 7) not in order)
               and time.perf_counter() - t0 < 5.0):
            time.sleep(0.01)
        time.sleep(0.1)
    finally:
        restore()
    kinds = [k for k, _v in order]
    # Preconditions fail outright rather than satisfying the expected
    # failure: only the ordering assertion below is the known fault.
    if ("index", 7) not in order:
        pytest.fail("the index was never reported: %r" % order)
    if kinds.count("feed") < 3:
        pytest.fail("fewer than three feeds (text, silence, text): %r" % order)
    first_feed = kinds.index("feed")
    index_at = order.index(("index", 7))
    assert index_at > first_feed, \
        "the earcon fired before its text had reached the player: %r" % order
