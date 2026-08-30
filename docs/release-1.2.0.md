# outSPOKEN 1.2.0

**Spanish. Carlos and Catalina — the Mexican Spanish MacinTalk Pro, running for
the first time outside a Macintosh.** A second Pro synthesiser Apple shipped in
1996, extracted from the floppies you already own, in pure Python, and speaking
through NVDA with no lag.

Nothing English needs re-extracting, and nothing English changes. This is
purely additive: the same Bruce, Agnes and Victoria, with two Spanish voices
sitting beside them.

## Carlos and Catalina

Apple sold a Mexican Spanish MacinTalk Pro — component manufacturer `cami`, a
separate synthesiser from the English `gala` — with two voices: **Carlos** (a
man) and **Catalina** (a woman). They speak Mexican Spanish, tagged so NVDA's
automatic language switching can reach for them, and they appear as **"Carlos
(MacinTalk Pro)"** and **"Catalina (MacinTalk Pro)"** in your voice list, next
to the English three.

It is the same 1990s concatenative Pro architecture, so one driver runs both —
they differ only in where they live, which resource is their entry code, and
that the Spanish engine keeps its whole lexicon in rule tables rather than in a
data fork. As far as we can establish, neither voice had run outside a
Macintosh before.

## Reading the floppies

The Spanish Pro was never on a System disk to be mounted. It came as three
**self-mounting floppy images** — `Mexican_TTS_1.5_1of3.smi.bin` and its `2of3`
and `3of3` — and the extractor now reads those `.smi.bin` files directly, the
same "point it at your own copy" path as everything else. Four wrappers sit
between the download and the speech, all peeled in pure Python with no emulator:
a MacBinary header, an NDIF disk image, an Apple Installer tome, and the
InstaCompOne codec. The whole recipe is in
[`docs/self-mounting-images.md`](self-mounting-images.md) and
[`docs/cami-engine.md`](cami-engine.md); as ever, the add-on ships none of it.

**All three floppies have to be in one folder**, because Apple split them that
way — the engine is on disk 1, Carlos on disk 3, and **Catalina's voice is
split across disks 1 and 2**. Point the extractor at one and it gathers its
siblings; point it at a lone disk and it now says exactly which one is missing
and why, rather than pretending nothing was there.

## Four bugs between the engine and the ear

Every one of these was found because a voice would not speak, and each was a way
the host had been quietly wrong that only the Spanish engine ever leaned on.
English Pro renders **byte-for-byte identical** before and after all four —
across the whole inflection and pitch range, not just at rest — so they change
only what was broken.

* **A stack drift in `_X2Fix`/`_X2Frac`.** The toolbox glue popped four bytes it
  should have left, and Carlos's voice-select read the wreckage as error 20125.
  English Pro had the identical drift and survived it by luck; the Spanish
  engine propagated it. Fixed, and gala is unchanged.
* **An allocator that zeroed what a real Mac leaves as garbage.** The Spanish
  front-end scans one word past a buffer into free memory, expecting the nonzero
  bytes an uninitialised heap holds; a zero-filled store sent it looping for
  tens of thousands of words. The host now leaves non-cleared allocations dirty,
  as the Memory Manager does.
* **An integer overflow that took NVDA down.** This one was not the Spanish
  engine's at all — it is a real crash English Pro's **Victoria** hits when a
  fast-typed utterance is cancelled mid-synthesis: a wild pointer near the top
  of memory wrapped a bounds check and became an access violation instead of the
  fault the host means to record. If you type fast into Victoria, that crash is
  gone.
* **The tome payload was read wrong.** Its InstaCompOne stream is a sequence of
  records, not one bitstream to scan for markers — a marker byte-pattern occurs
  inside raw audio, and a scan stops there and leaves the rest as garbage, which
  is why Catalina (the split voice) decoded to noise until the format was
  understood.

## The data manager tells the truth now

Two ways the Tools-menu extractor used to mislead, both fixed:

* **A refused import said nothing.** When it stopped — an incomplete floppy set,
  say — it reported a bare "0 resources installed" that read like success. It
  now shows the reason it stopped.
* **The overwrite prompt threatened everything.** Importing the Spanish floppies
  onto a machine with the English voices *adds* two and touches nothing else,
  yet the dialog warned it would "write over" your data. It now previews what a
  source will actually write and asks only about genuine conflicts, naming them
  — a first Spanish import asks nothing at all.

<!-- ─────────────────────────────────────────────────────────────────────────
     HANDOVER: the sections below are the other lane's to write.
     This release also carries the SAPI-5 lookups+port and the add-on's new
     "check for updates" (ospmanager + updates.py + sapi/settings.ps1, commits
     174f971 and d1f4bf8). Fill in the SAPI/updates story here; the voice
     engineering above is complete. — the voice-side session
────────────────────────────────────────────────────────────────────────── -->

## SAPI 5, and checking for updates

_(To be written by the SAPI/bridge session.)_ This release also brings the
matching SAPI-5 lookups and port that had been waiting on the Spanish voices,
and a "check for updates" action in the speech-data manager that asks GitHub for
the newest published release.

## Under the hood

* **318 tests, all green.** The new work is covered end-to-end: the `.smi.bin`
  pipeline reproduces the three carved forks byte-for-byte from the real
  floppies, and the driver renders both voices through the shipped rom layout.
* **The regression gate grew.** English Pro's byte-identity was being checked at
  rest only, which let a settings-path change reach a build; it now compares
  Bruce and Agnes across inflection 0/50/100 and a pitch offset, which is the
  path the host fixes actually touched.
* New modules `smi.py` and `insta3.py` carry the self-mounting-image pipeline
  and the InstaCompOne codec; `macintalkpro.py` drives both Pro engines from one
  `VARIANTS` table.

Credit for cracking the tome's record-stream format goes to Sol.
