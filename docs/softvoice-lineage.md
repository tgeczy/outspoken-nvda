# MacinTalk and the Amiga narrator are the same program

Tomi's ear got here first. Once our host produced intelligible speech, the voice
was recognisably the **Amiga narrator** — and [@pitermach](https://dragonscave.space/@pitermach)
pointed out there is an NVDA add-on built on it. Comparing the binaries turns
that resemblance into a measurement.

## The measurement

| | shared with the Amiga file | of |
|---|---|---|
| `DRVR 1030` (MacinTalk) vs `narrator.device` | **14,416 bytes, 67.8%** | 21,272 |
| `RULZ 1129` (letter-to-sound) vs `translator.library` | **4,166 bytes, 60.2%** | 6,922 |

Counting only identical runs of 24 bytes or more (16 for the rules), the largest
single run being **5,540 bytes**. Both files are 68000, so code matches as
readily as data does.

The phoneme table is byte-identical, padding and all:

```
MacinTalk +$159C   ' \0.\0?\0,\0-\0(\0)\0\0\0\0\0IYIHEHAEAAAHAOUHAXIXERUXQXOHRXLXEY\0\0AY...'
narrator  +$00EB4  ' \0.\0?\0,\0-\0(\0)\0\0\0\0\0IYIHEHAEAAAHAOUHAXIXERUXQXOHRXLXEY\0\0AY...'
```

111 entries, recovered by breaking at +$16D8 and reading `d7`:

```
  .  ?  ,  -  (  )        IY IH EH AE AA AH AO UH AX IX ER UX QX OH RX LX
  EY AY OY AW OW UW WH R  L  W  Y  M  N  NX NH DX Q  S  SH F  TH Z  ZH V
  DH CH J  /H /M /B /R /C B  D  G  GX GH P  T  K  KX KH UL UM UN IL IM IN
  0 1 2 3 4 5 6 7 8 9
```

`UX QX RX LX IX`, the `/H /M /B /R /C` clicks, the `UL UM UN IL IM IN`
syllabics and digits 0–9 for stress are the Amiga narrator's documented set.

## The corroborating fingerprint

`RULZ`'s exception dictionary — in a **Macintosh** speech synthesiser — contains:

```
[AMIGA]=AHMIY5GAH
[ATARI]=AHTAA4R...
[SOFTVOICE]=SAA4FTVOYS
```

Apple had no reason to teach MacinTalk to say "Amiga" and "Atari". A vendor
shipping the same engine to all three did. That vendor names itself in its own
dictionary: **SoftVoice, Inc.** This is consistent with the published history of
both products, though the binaries alone prove common source rather than
authorship.

## What it means for this project

**The architecture is settled, and it is the Amiga's.** Two components, not one:

* `narrator.device` speaks phonemes — our `DRVR 1030` does the same, and its
  `Prime` accepts **nothing but phonemes**. Breaking at +$332 shows the scanner
  at +$167C stopping at character 2 of `"This apple is."` and consuming all 17
  of `"DHIHS KAA1PIY IHZ"`. The English branch at +$33A is not reachable from
  `Prime`; the `RULZ` probe in `DriverOpen` sits in a path nothing calls.
* `translator.library` converts English to phonemes — outSPOKEN's own Pascal
  code plus `RULZ` did that job, outside the driver.

So the add-on needs its own English front end. That is good news rather than
bad, because it can be **our code over the user's data**: `RULZ` is a table of
32-bit offsets followed by rules in the classic NRL notation,

```
left [ focus ] right = phonemes \
#[SION]=ZHUN\  [SIN] =SAY2N\  ^[SION]=SHUN\  [S]S=\
```

which is a well-documented published algorithm (Elovitz et al., NRL Report
7948, 1976). We write the interpreter; the rules come from the ROM the user
supplies. Nothing needs vendoring, which keeps the repository's rule intact.

The alternative — running the Amiga `translator.library` under Musashi, since we
already host 68000 — would work, but it drags a second engine's redistribution
question in behind it for no capability we cannot write ourselves.

## A note on the other add-on

The existing Amiga Narrator add-on reportedly cuts phrases off. We hit the
mirror-image bug from the same contract: with a stop-speech callback that never
reported "stop", playback ran **past** the end of the frame block instead of
short of it (377 seconds for a three-word phrase). See
[`frame-format.md`](frame-format.md) — the terminator is bit 7 of `f[0]`, and it
reaches the synthesiser only through the callback. Whether that is the same root
cause there is unverified speculation; the code is a closed-source DLL and we
have not looked inside it.
