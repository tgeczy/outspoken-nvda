# outSPOKEN 2.0.3

**MacinTalk 1 pronounces the ends of words properly again**, and the SAPI voices
stop waking the processor while they have nothing to say. The add-on, the SAPI
installer, the APK and the Linux tarballs all carry the fix for MacinTalk 1.

## MacinTalk 1: words before punctuation (#5)

Credit to Aksel, who noticed that the 1984 voices said "hello" as something
like "hellaw" and "try" as "tree", and checked against recordings of outSPOKEN
on a real Macintosh, which said them properly.

MacinTalk 1 turns English into speech sounds with a table of letter-to-sound
rules, and many of those rules apply only at the end of a word: a final O is
"oh", a final Y after a consonant is "eye". In the rule table, a space beside a
rule means "the edge of a word". outSPOKEN for NVDA read it as a literal space
character, so a word followed by **any punctuation** -- a comma, a full stop, a
question mark, a bracket -- was treated as if it carried on, and lost its
word-ending rules. "Hello," came out "hellaw", "try." came out "tree", "go,"
came out "gah", and "me." came out "meh". Words on their own were right, which
is why this went unnoticed for so long: the rule table's own built-in checks
are all single words.

Now anything that is not part of a word counts as its edge, the way the rules
were written. Letters and apostrophes still count as part of a word, so
contractions such as "I'm", "don't" and "it's" are read exactly as before. A
word on its own sounds exactly as it did. What changes is every word that ends
a sentence or a clause, which is most of them; you will also hear sentence
endings fall where they should.

This affects only MacinTalk 1, the male and female voices. MacinTalk 2, 3 and
Pro have translators of their own.

Some words were always wrong with MacinTalk 1 -- "brown" and "town" rhyme with
"known", for instance -- and those are the 1984 engine's own rules, kept as
they were. This one was ours.

## SAPI: no more work while nothing is being said

The SAPI voices speak through a small background program, `osp_host.exe`, which
a screen reader or a program like Balabolka starts when it first uses an
outSPOKEN voice and keeps for as long as it holds that voice -- often the whole
session. Between utterances that program checked for new text every
2 milliseconds, which Windows 11 rounds to about **63 times a second**, for as
long as it was running. Each check costs almost nothing; waking that often is
what keeps a laptop's processor from settling into its low-power states.

Now it sleeps until there is something to say, and the request wakes it the
moment it arrives. The same idle host wakes about once a second. A short
request also gets its first sound a little sooner than before (a median of
20 ms against 29 ms on the test machine), because the old check could oversleep
by a whole timer tick before noticing new text. This came to light while fixing
the same kind of bug in Panthera, its sibling add-on, after Christopher reported
Panthera draining a phone's battery. Only outSPOKEN's SAPI program had it: the
NVDA driver and the Android app never polled.

Measured on Windows 11 with Fred: the audio is byte-identical to 2.0.2's,
including speech after the host has sat idle, and the 32-bit host's to the
64-bit host's. Windows 7 was not available to test.

## Everything else

Nothing of Apple's or Berkeley's is included. Keep using your extracted data.

## Updating

Press **Check for updates** in the speech data manager (NVDA's Tools menu) or in
the SAPI settings window, and this release installs itself; on Android, Check for
updates on the app's Setup page. The NVDA add-on, the SAPI installer, the APK and
the Linux tarballs are all attached below.
