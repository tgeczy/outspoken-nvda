# outSPOKEN 0.7.0 — MacinTalk Pro

**Agnes, Bruce and Victoria.** The first concatenative Macintosh voices, and
the ones most people mean when they remember what a Mac sounded like. Fifteen
voices now, across three engines, all running as real 68000 code inside NVDA.

As far as we can establish, these have not run outside a Macintosh before.

## You need to extract again

**This is the one thing to do.** MacinTalk Pro needs far more of a voice than
earlier versions ever took — the concatenative unit database, about 11 KB of
per-voice code, and the resource fork itself, which Pro reads by seeking into
rather than by asking for handles. If you extracted your voices before 0.7.0,
your `voices/Agnes` holds the voice descriptor and nothing else.

```sh
py -3 tools/extract_rom.py "C:/path/to/MacOS7.hfv" --nvda   # needs machfs
```

`--nvda` is new and writes straight into the folder the add-on actually reads,
which is `outspoken-roms` in your NVDA configuration directory. Without it the
files land in `./rom`, which NVDA never looks at — "I extracted it and nothing
changed" is the commonest way this goes wrong, and it is now one flag.

Re-running completes an existing folder in place. Nothing is lost.

**An incomplete voice is not offered rather than offered-and-silent.** A
synthesizer that lists a voice and then says nothing is worse than one that
does not list it, so the add-on now checks that a voice folder is complete and
not merely that the engine is installed. Those are different failures and only
one of them was caught before.

The engine folder is checked the same way, and that half is the quieter one: a
MacinTalk Pro folder missing its `datafork.bin` *opens*, accepts a voice, and
then speaks nothing, because the lexicon lives in there. A partial extraction
that opens is indistinguishable from a working one right up to the silence.

The extractor says what will happen before you restart anything:

```
  NVDA will offer, from C:\Users\you\AppData\Roaming\nvda\outspoken-roms:
    MacinTalk 2    10 voices  Ben, Boris, Brenda, Mariel, ...
    MacinTalk Pro   3 voices  Agnes, Bruce, Victoria

  Present but NOT offered, and why:
    Fred and 19 more    MacinTalk 3 is not installed
    Agnes               incomplete extraction, missing rsrcfork.bin
```

## What was actually wrong

Pro has opened, taken a voice and run its synthesis modules for days without a
sound coming out. Neither cause was in the engine; both were services this host
answered wrongly.

**Pro reads its lexicon asynchronously.** It issues `_Read` with the async bit
set, hands the File Manager a completion routine, and parks the module that
asked until that routine wakes it. We copied the bytes, set `ioResult` and
returned. Every field read back correct and the callback never came, so the
phoneme stage slept forever and nothing downstream of it ever ran.

**`_FixRatio` was never served at all**, and the waveform stage calls it twice.
An unserved Toolbox trap is not a no-op — it leaves the arguments on the stack
and the result slot unwritten — so that stage took a null for a table base and
walked it in 82-byte steps: twenty million out-of-range reads in one utterance.

Both are the same mistake wearing different clothes, and it is the rule this
project already had written down: **a stubbed trap is a lie the caller cannot
detect, and so is a trap answered synchronously when it was asked
asynchronously.** Nothing in the engine can tell the difference.

## Also

* Pro renders at roughly 40x realtime — 10 to 20 milliseconds for a typed
  character — so it behaves like the other engines rather than like an
  emulator.
* Pro holds one voice at a time. Changing voice rebuilds, which is invisible
  except that it is why this is not instant on the first utterance after a
  change.
* Pro needs a 68020, finds its modules by resource *name* rather than id, and
  reads its own 573 KB lexicon out of its data fork while it speaks.
* The pitch slider is inert for Pro, as it is for MacinTalk 2, until it can be
  made to behave.

## Unchanged

This add-on contains no part of any engine and no release built from it ever
will. You supply them from your own copy; `tools/package.py` refuses to build a
release if anything resembling engine data is in the tree.
