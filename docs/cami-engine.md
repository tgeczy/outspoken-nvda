# The `cami` engine — MacinTalk Pro in Mexican Spanish

There are two MacinTalk Pro synthesisers, not one. The English one everybody
knows — Agnes, Bruce, Victoria — is the Component Manager component whose
manufacturer is **`gala`**. Apple also shipped a **Mexican Spanish** MacinTalk
Pro, manufacturer **`cami`**, with two voices: **Carlos** and **Catalina**. It
is the same 1993–96 concatenative architecture, the same `ttsc` component with
the same selector map, and this add-on drives it with the same
`macintalkpro.Engine`. What follows is only where it differs, because the
places it is the same are what let one Engine drive both.

As far as we can establish, Carlos and Catalina had not run outside a Macintosh
before. Nothing in this add-on, or in any release, contains a byte of either;
this is the recipe for reading them out of your own copy.

## Where it comes from

`cami` was not on a System disk to be mounted. Apple sold it as three
**self-mounting floppy images** — `Mexican_TTS_1.5_1of3.smi.bin` and its `2of3`
and `3of3` — and the extractor reads those `.smi.bin` files directly. All three
are required and must sit in one folder: the engine is on disk 1, Carlos on disk
3, and **Catalina's voice is split across disks 1 and 2**, so a missing floppy
is not a smaller extraction but a broken one. The four wrappers between the
download and the resource forks — MacBinary, an NDIF disk image, an Apple
Installer tome, and the InstaCompOne codec — are documented in
[`self-mounting-images.md`](self-mounting-images.md) and implemented in
[`_outspoken/smi.py`](../addon/synthDrivers/_outspoken/smi.py) and
[`_outspoken/insta3.py`](../addon/synthDrivers/_outspoken/insta3.py). The whole
pipeline is pure Python; no emulator is needed to extract, only to speak.

The extractor writes the engine to its **own** rom folder, `macintalkespanol`,
and the voices to `voices/Carlos` and `voices/Catalina` beside the English ones.
Its own folder matters: both engines carry a `rsrcfork.bin`, and
`voices.engine_installed` checks a single folder for a whole engine's file list,
so sharing a folder would let half of one engine stand in for the other.

## Three differences, and only three

Everything the driver does — the 68040, the auto-advancing Ticks clock,
resource registration **by name**, `cvox`/`rate`/`pbas`/`pmod` — is identical to
English Pro. `macintalkpro.VARIANTS` names the whole of the difference:

| | `gala` (English) | `cami` (Spanish) |
|---|---|---|
| rom folder | `macintalkpro` | `macintalkespanol` |
| entry code | `gtse 1` (`*TTS`) | `gtse 99` (`*TTS`) |
| component manufacturer | `gala` | `cami` |
| data fork | 573 KB lexicon | **none** |

The missing data fork is the one that would bite a careless extractor. English
Pro keeps its lexicon in a data fork; `cami` has no data fork at all, because
its front end is **rule-based** rather than dictionary-based — the letter-to-
sound rules live in the resource fork as `gtst` resources (`OrthToPhonRules`,
`Literals`, `Abbreviations`) with `gtsp` phonemes and a `ttss` phoneme-symbol
table. So `cami`'s `ENGINE_FILES` in `voices.py` asks for `gtse_99.bin` and
`rsrcfork.bin` and **not** `datafork.bin`; requiring the data fork English Pro
has would gate a perfectly good engine off.

## The voices

Carlos and Catalina are ordinary `ttvd` VoiceDescriptions — creator `cami`,
language Spanish — so `voices.describe` reads them with no special case and NVDA
lists them as **"Carlos (MacinTalk Pro)"** and **"Catalina (MacinTalk Pro)"**,
tagged `es`, sitting with the English three. Inside, each voice's data resources
carry an engineering codename — Carlos's `gtss` is `SpanEdselData`, Catalina's
`SpanMagdaData` — but the name a user sees is the `ttvd`'s own, which is Carlos
and Catalina. Each voice is a `ttvd`, a 128-byte `gtsv` record, and a `gtss`
holding ~800 KB of concatenative units plus ~11 KB of per-voice code, read out
of the voice file's resource fork the same way English Pro reads Bruce.

## What it took to make them speak

`cami` opened under the host at the first attempt — its `*TTS` dispatch table is
byte-identical to `gala`'s — but voice-select and synthesis exposed four host
bugs the English engine had always tolerated. All four are fixed, and each fix
is provably safe for `gala`, because Bruce renders **byte-for-byte identical**
before and after (the regression gate). In order:

1. **`_X2Fix`/`_X2Frac` popped four bytes they should not.** Every call site is
   in-place (`pea ptr; trap; move.l (a7)+,dst`) with no reserved result slot, so
   the toolbox glue's `param_bytes` had to be 0, not 4. The four extra bytes per
   call drifted the stack and corrupted a saved register in the `pbas` handler;
   `cami`'s voice-select read the garbage and returned 20125. `gala` had the
   identical drift but overwrote the register with its success status before
   returning, so it never saw it.
2. **The allocator zeroed every block; a real `NewPtr` does not.** `cami`'s text
   front end scans allocated memory expecting the non-zero garbage a Mac heap
   leaves, and a zero-filled block sent it into an infinite loop. The host now
   honours the *Clear* bit — zero only for `NewPtrClear`/`NewHandleClear`, a
   pattern otherwise — and primes the heap the same way.
3. **`in_ram` overflowed.** `(a + n) <= size` wraps for a wild pointer near
   `0xFFFFFFFF`, passing the check and dereferencing gigabytes past the buffer.
   This one was not `cami`'s at all: it is a real crash English Pro's Victoria
   hits when a fast-typed utterance is cancelled mid-synthesis, taking NVDA down
   with an access violation instead of the fault the host means to record.

The fourth was not in the host but in the extraction: the InstaCompOne tome
payload is a **sequential record stream**, not a bitstream to scan for markers,
and Catalina — the split voice — decoded to garbage until that was understood.
See [`self-mounting-images.md`](self-mounting-images.md) and `smi.py`.
