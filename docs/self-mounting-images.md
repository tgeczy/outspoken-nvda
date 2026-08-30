# Reading `.smi` self-mounting images (the Mexican Spanish MacinTalk Pro trail)

Someone downloads a classic Mac speech package off an archive site as
MacBinary `.bin` files and has no emulator. The extractor already reads a
MacBinary resource fork and a raw `.hfv`; a **self-mounting disk image**
(`.smi`) is the next wrapper it should read, so those downloads reach the same
"point it at your own file" path as everything else. Four layers sit between
the `.bin` and the files, and **all four are solved and shipping** — pure
Python, no emulator to extract. The code is
[`_outspoken/smi.py`](../addon/synthDrivers/_outspoken/smi.py) (layers 1–3 and
the InstaCompOne record framing) and
[`_outspoken/insta3.py`](../addon/synthDrivers/_outspoken/insta3.py) (the
codec); the engine they carve is documented in
[`cami-engine.md`](cami-engine.md).

Worked example, and the reason this exists: **`Mexican_TTS_1.5_{1,2,3}of3.smi.bin`**
— the Mexican Spanish MacinTalk Pro package, three MacBinary-wrapped
self-mounting images Apple shipped together. From them the pipeline carves the
`cami` engine and the Carlos and Catalina voices, each reproduced byte-for-byte
and verified against a live host.

## Layer 1 — MacBinary

128-byte header. Filename length at +1, name at +2, file type at +65, creator
at +69, **data-fork length at +83 (u32), resource-fork length at +87 (u32)**.
Data fork starts at 128; resource fork starts at `128 + roundup(dataLen,128)`.
The `.smi` unwraps to type `APPL`, creator `oneb` (a One-Button-Mounter app):
the disk image lives in its **data fork**, the mounting stub in its resource
fork (`CODE 'OneButtonMounter'`, `DRVR '.HDI'`, the `bcem` map, barber-pole
`PAT`terns).

## Layer 2 — NDIF (the `bcem` block map + ADC) — CRACKED

The data fork is an NDIF image: an HFS volume stored as chunks, some raw, some
compressed, zero runs omitted. The map is the `bcem` resource in the stub's
resource fork.

`bcem` layout, big-endian:

* **128-byte header.** version `0x000B` at +0; the image name as a Pascal
  string from +4; **total sectors (u32) at +0x44** (1600 here → an 819200-byte
  volume); a checksum at +0x50.
* then **12-byte chunk entries**, `numChunks` of them (also at +0x7C):
  `(w0, srcOffset, compLen)`.
  * **`w0 = (destSector << 8) | type`.** Types: `0x00` zero-fill, `0x02` raw,
    `0x83` ADC-compressed, `0xFF` end-of-map. Dest sectors are 512-aligned so
    `destByte = (w0 >> 8) * 512`.
  * `srcOffset` / `compLen` address the chunk's bytes in the data fork.
    They are contiguous and cumulative: the compressed lengths sum to exactly
    the data-fork length, which is how the field roles were proven.

Reconstruct by walking the entries in order, placing each at its dest sector:
raw → copy `compLen` bytes; ADC → decompress; zero-fill → emit
`(nextDestSector - thisDestSector) * 512` zeros. The result mounts with
`machfs`. (Chunk 0 is always raw — it carries the boot blocks and the primary
MDB, which is why `'BD'` sits at data-fork offset 1024 before any decode.)

**ADC** (Apple Data Compression), byte-oriented, three opcodes:

    b & 0x80          literal run:  count = (b & 0x7F) + 1, copy count bytes
    b & 0x40          long match:   count = (b & 0x3F) + 4,
                                    dist  = (next two bytes) + 1
    else              short match:  count = ((b >> 2) & 0x0F) + 3,
                                    dist  = ((b & 0x03) << 8 | next) + 1

A run whose ADC decode overruns the input (an invalid back-reference on the
first bytes) is a raw chunk, not ADC — detect by trying and falling back.

The three mounted volumes hold an **Apple Installer** and its tomes:

    disk 1  Installer, "Mexican TTS Install Script", SimpleText,
            readme, "Mexican Spanish TTS Tome 1"
    disk 2  "Mexican Spanish TTS Tome 2"
    disk 3  "Mexican Spanish TTS Tome 3"

## The Apple Installer manifest — DECODED

The Install Script is an ordinary resource fork; its `in··` resources are the
standard Apple Installer atoms. Two are enough to know every file, its size and
its destination:

* **`infa`** (file atom): flags(+0), size-in-tome(+4), `intf` id in the low 16
  bits of +8, a file id (+0xC), **data-fork length (+0x10)**, **resource-fork
  length (+0x14)**, then the file name as a Pascal string at +0x20. (Atoms
  shorter than ~32 bytes are placeholders — guard the fixed reads.)
* **`intf`**: file **type** (+4) and **creator** (+8), Finder dates, then the
  **target path** as a Pascal string, e.g. `special-extn:Voices:Carlos`.

The Mexican package's payload, straight from the manifest:

| file | type/creator | rsrc | target |
|---|---|---|---|
| MacinTalk Español Mexicano | `thng`/`cami` | 153379 | Extensions |
| Carlos   | `ttvf`/`cami` | 784524 | Extensions/Voices |
| Catalina | `ttvf`/`cami` | ~810000 | Extensions/Voices |

**The Spanish engine is a separate synthesiser from English Pro** — component
manufacturer **`cami`**, not `gala` — and it has **no data fork**: the lexicon
that Pro keeps in its 573 KB data fork lives in `cami`'s resource fork instead.
The two voices are **Carlos** and **Catalina**.

## Layer 3 — the tome archive (Apple Installer) — STRUCTURE CRACKED

The tome is an **Apple Installer archive** (Installer 4.0.3; script © Apple
1994–96). Structure, confirmed byte-for-byte against `kainjow/TomeViewerX`'s
`tome.c`:

* **36-byte header**: magic `0x6B630001`, then 24 bytes, then the **file
  count** (u32) at +28.
* then one **128-byte section per file**: `f1`(u32), `id`(u16),
  `name_length`(u8), `name`(31), `type`(u32), `creator`(u32), two dates,
  `version`(u16), finder flags, then for **each fork** a triple —
  `size`, `offset`, `compressed_size` (u32 each) — with a checksum after each.
  Data-fork triple at section +60, resource-fork triple at +76.

The offsets are absolute into the tome data fork; a fork with `size>0` and
`compressed_size>0` is an InstaCompOne stream at `offset`. A file too big for
one 800 KB disk is **split across tomes** (Catalina: 37999 bytes in Tome 1 +
810000 in Tome 2 = 847999), so a reader must concatenate a file's pieces across
the tome set the installer script lists.

The Mexican package installs five files (Speech Manager `INIT`, the engine
`thng`/`cami`, the Speech control panel `cdev`/`earc`, and voices Carlos and
Catalina `ttvf`/`cami`); only the three `cami` files matter to us.

## Layer 4 — InstaCompOne — SOLVED (a record stream, not a bitstream)

**The framing that took the longest to get right:** a fork is not one
bitstream to scan for `00 01 00 00` markers — it is a **sequential run of
records**. Each record has a 4-byte header (`00 01 00 00` compressed,
`01 01 00 00` stored), each contributes output up to the next 64 KiB boundary,
and the output position and the LZ history are **global** across records. The
codec inside a compressed record is `dcmp 3` exactly (raw magnitude = running
output position, no window cap); the record framing around it is what
`smi._decode_into` implements.

A marker *scan* fails because an accidental `00 01 00 00` occurs inside raw
audio — it does, at Catalina's relative offset 262061 — and a scan stops there
and leaves the rest of the fork as garbage. That was the whole of the
"before-start reads / sliding window / segment restart" saga below: symptoms of
scanning a record stream as if it were one bitstream. Consuming every record
consumes every source byte, reproduces the engine and Carlos forks
byte-for-byte, and makes Catalina's split pieces reassemble cleanly. Credit for
the record-format crack is Sol's.

> The paragraphs below describe the codec itself (exact and correct) and record
> the dead ends the record-stream framing resolved. They are kept as a
> reference for anyone reading a related Installer archive; the working code is
> `smi.py` + `insta3.py`.

The fork data is **InstaCompOne**: LZ77 + Huffman, a 4-byte prefix then a
big-endian MSB-first bitstream. `maximumspatium/ResDecompress`
(`InstaCompOne.py`) has the framework but was **never tested** (its suite only
covers GreggyBits). Ground truth is the Installer's own 68k decompressor,
resource **`dcmp 3`** (`dcmp 0` is DonnBits), reversed with capstone-M68K.

**The codec is a byte-exact replica of `dcmp 3`** (raw magnitude = running output
position, no cap). Every component was read from the disassembly of `dcmp_3.bin`
and confirmed by an emulator byte-match: the main loop `+0x50`, `decode_length`
`+0x294`, `decode_literal` `+0x1b8`, the mode/state machine, and the distance
dispatcher `+0x3f8` — a **per-tier jump table**, each tier uniform (bit0=0 →
small `reader(n)+1`; bits `10` → mid `reader(n+2)+2^n+1`; bits `11` → cascade
base `5·2^n+1`, width `ceil_log2(mag − 5·2^n)`). The thresholds confirm **70000
(0x11170)**, not 86016.

**Historical dead end (do not revive):** we briefly believed `0x00010000` = 65536
was an LZ window size and that `mag = min(position, 65536)` was the fix. It made
the engine total land on 153379 by coincidence but still did not parse, and it
diverges from the real 68k. The 65536 is the skipped `word,word2` prefix; the
before-start behaviour is set up by the **caller**, not the codec (see the
superseded note above).

Each token: a **length** Huffman (unary prefix `u` = count of leading 1-bits,
capped at 10, then a `(bits, base)` table); if the length is 0 and not just
after a reference it is a **literal** run (its own Huffman gives the count, then
that many `getbits(8)` bytes); otherwise a **reference** — `copyCount = len+2`
(`+1` more right after a literal) and a **distance**.

**Distance** — a tier chosen by a magnitude (current output position) ladder,
read verbatim from `dcmp 3`'s dispatcher:

    thresholds: 10 20 40 80 160 672 1000 2688 5376 10752 21504 43008 70000 172032
    tier n:      0  1  2  3   4   5    6    7    8     9    10    11    12     13   (14 above)
    small (bit 0)    : getbits(n)              + 1
    mid   (bits 1,0) : getbits(n+2)            + (2^n + 1)
    high  (bits 1,1) : getbits(ceil_log2(mag - 5*2^n)) + (5*2^n + 1)

The three fixes ResDecompress needs (each cost a full debug pass):

1. **The unary length cap emits no terminating 0.** Ten leading 1-bits ends
   the prefix with *no* trailing 0; ResDecompress's `lenHuffTab` encodes case 10
   as `0b11111111110` and so eats one extra bit whenever a length ≥ that shows
   up (it did at engine output 53503).
2. **The upper thresholds are literal, not `21*2^n`** — n=12 is **70000**
   (0x11170), not 86016; there is an n=14 subroutine for everything above
   172032 (Catalina's magnitudes reach there).
3. **A reference may point before the output start.** `dist > dpos` gives a
   negative source; the 68k reads from its zero-initialised buffer there, so
   those bytes are **0**, never a wrapped byte. This is how zero runs are coded.

`getbits` (single-byte refill) and a second reader (`+0x1cd6`, refills to ≥23
bits, used for length case 9/10 and the small/mid distance of tiers ≥7) are
value-identical to a loop-refilling MSB-first reader — implement one and use it
everywhere.

The decompressed engine holds its own resources — `thng`/`cami`, `ttsc`,
`gtse` and the string "MacinTalk Español Mexicano". The whole pipeline now runs
in pure Python on the `.smi.bin` downloads with no emulator; `ospextract` gains
NDIF + tome + InstaCompOne and can carve the `cami` engine and the Carlos /
Catalina voices (concatenating Catalina's two tome pieces).

## How the integration works, now that it is done

* Files are classified by **type/creator** (`thng`/`ttvf` + `cami`) from the
  tome's own section catalog, never by filename — "Español" carries MacRoman
  `0x96` and would defeat any name match. The tome section also carries the
  clean voice name ("Carlos", "Catalina"), which becomes the rom voice folder.
* A file split across tomes (Catalina) is reassembled **head-first**: the head
  piece is the one whose decoded first 16 bytes are a valid resource-fork
  header, and that header's `mapOffset + mapLen` gives the assembled total the
  joined pieces must sum to — a self-checking rule, no hardcoded piece order.
* `cami` has its **own** `ENGINE_FILES`/`VOICE_PARTS` in `voices.py` — no
  `datafork.bin`, entry code `gtse 99` — so reusing `gala`'s list cannot gate a
  working engine off. It is written to its own rom folder, `macintalkespanol`.
* `cami` is a distinct engine from `gala`; voice→engine routing by
  `ttvd.creator` separates them, and `outspoken.py` gives the Spanish voices an
  `es` language tag from the `ttvd`. Text stays MacRoman (`á é í ó ú ñ ¿ ¡` are
  all in it). One `macintalkpro.Engine` drives both — see
  [`cami-engine.md`](cami-engine.md).
* The whole set is enforced at the `ospextract` layer, so the command-line tool
  and the Tools-menu dialog both insist on all three floppies alike: a missing
  disk is named, with why (Catalina spans two of them).

Nothing here or in any release contains a byte of the engine — this is the
recipe, the same rule the rest of the project keeps.
