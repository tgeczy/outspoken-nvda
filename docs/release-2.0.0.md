# outSPOKEN 2.0.0 — the host becomes the engine

**Draft. Nothing here has shipped.** This is the plan for the release, written
before the work so the work has something to be measured against, and kept
current as it lands. The `<!-- HANDOVER -->` block at the end says where things
stand for whoever picks it up next — the other agent reads it too.

## What earns the major number

1.2.x is a Windows NVDA add-on with a SAPI bridge that runs the NVDA driver
under an embedded Python. 2.0.0 is the same five engines and thirty-six voices
on **Windows, Linux and Android**, from one native host that owns everything
between the text and the PCM. That is the same reasoning that made Panthera
3.0 a major: one release adds two platforms and moves shared logic out of the
drivers into the host, so every front end improves at once and none can drift.

The problem here is differently shaped from Panthera's, and mostly easier:

* **No AAC decoder.** Nothing MacinTalk shipped in 1984–96 is compressed.
  Every engine hands back 8-bit unsigned PCM at 22254.5 Hz and that is the
  whole audio path.
* **No native code to load.** The engines are 68000 machine code and the host
  already runs them under Musashi, Karl Stenerud's interpreter, which is plain
  C. It compiles for any processor. There is no i386-guest-on-x86-host fast
  path to protect and no Unicorn or Box64 to carry — a Linux build on any
  architecture and an Android build on any ABI are the *same* build.
* **More engines, more front ends.** Five synthesisers behind one Component
  Manager, a 1984 driver that speaks only phonemes and needs the NRL rules run
  for it, and number reading in **English and Spanish**. All of that is Python
  today and all of it moves.

## The shape

One C codebase, `src/osp_host*.c`, built three ways from one source list:

| build | who loads it | why it exists |
|---|---|---|
| `osp_host.dll` / `osp_host_x86.dll` | NVDA, in-process over ctypes — as today | keeps the add-on alive on secure screens, where NVDA drops every `.exe` |
| `osp_host.exe` (Windows), `osp_host` (Linux ELF) | the SAPI DLL as a child process; the shell | `--serve` speaks the `OSP4` protocol the SAPI DLL already speaks; `--render`, `--list`, `--capabilities` for scripts and for Linux integrations |
| `libosp_host.so` | the Android app over JNI | one worker process per engine, the Panthera shape |

The host's public surface becomes *text in, PCM out*: open a data root, list
the voices in it, select one, set rate/pitch/volume/inflection on the driver's
own 0–100 scales, speak, pull audio as it renders, cancel. The low-level
surface the probes and tests use — registers, memory, resources, component
calls — stays exported, because it is how every engine here was debugged and
will be again.

**Python is demoted from implementation to specification and oracle.** The
engine modules, `nrl.py` and `numwords.py` stay in the repository, readable,
and keep running in the tests as the reference the C is diffed against. They
stop running at speech time. Panthera's one-source rule applies verbatim:
where the port reproduces a quirk of the reference, it reproduces it, and a
fix lands in Python and C in one commit or not at all.

## The gate

**Byte-identical PCM between the Python-driven render and the C-driven
render**, for every installed voice, across a grid of rate × pitch ×
inflection × volume, on English and Spanish text, cancelled and uncancelled.
`tools/render_oracle.py` runs both and exits with the number of disagreements;
it needs engine data and so runs on the machines that legitimately have some,
never in CI. It is built **before** the first engine moves, so each engine is
ported against a harness that already fails.

`tests/test_sapi_serve.py` already asserts that the SAPI bridge is byte-for-
byte the NVDA driver. That assertion has to stay green through every phase; it
is the oracle in disguise.

The Tab-hold latency test stands as always: no merge and no release until every
engine passes it, including MacinTalk Pro at its slowest.

## What moves, and what stays

| today, in Python | tomorrow, in C | oracle |
|---|---|---|
| `numwords.py` — cardinals, ordinals, decimals, digit-by-digit, `en` and `es` | `osp_numbers.c` | `tools/numbers_oracle.py`, data-free, runs in CI |
| `nrl.py` — the RULZ interpreter, Berkeley's dictionary respell, letter names | `osp_nrl.c` | `tools/nrl_oracle.py`, needs the user's `RULZ`, local |
| `engine.py` — the 1984 `.sp` driver: DCE, hook, MACSTARTSOUND, Prime, stop flag, `_tidy`, `_last_buffer` | `osp_engine_sp.c` | render oracle |
| `macintalk2.py` — Component Manager open, all ten voices registered, `cvox`, Fixed-point settings, the pump, `_drop_restated`, `_trim` | `osp_engine_mtk2.c` | render oracle |
| `macintalk3.py` — the same on an 040, streaming, `_quiet` | `osp_engine_mtk3.c` | render oracle |
| `macintalkpro.py` — forks, map entries, names, ticks, one voice per instance, `gala` and `cami` | `osp_engine_pro.c` | render oracle |
| each engine's `translate` — spoken punctuation, MacRoman | `osp_text.c` | render oracle |
| `outspoken.py` — the rate curve, pitch tenths, `.sp` hertz from base, the 8→16 gain tables | `osp_settings.c`, `osp_audio.c` | render oracle |
| `voices.py` — `ttvd` as `VoiceDescription`, `installed`, `voice_incomplete`, `ENGINE_FILES` | `osp_voices.c` | the existing two-copies test, extended to three |
| `sapi/osp_serve.py` — the `OSP4`/`OSPR`/`OSPC` protocol, seq-tagged cancel, claiming stdout | `osp_serve.c` | `test_sapi_serve.py` |

Stays in Python, because it is NVDA's or a tool's and not the engine's:
`outspoken.py`'s worker thread, speech-sequence handling, indexes, breaks and
player; `rom.py`'s search roots (NVDA's config folder is NVDA's business; the
host takes a data root and is told); the extractor and everything under it
(`ospextract.py`, `smi.py`, `insta3.py`, `rsrc.py`); the manager dialog; the
update check. The SAPI installer keeps its embeddable Python for the Extract
button only; speech no longer needs it.

Every `max_instr`, `defer_callbacks`, `cb_wait` and `auto_ticks` value crosses
over unchanged. They decide where callbacks land, and the PCM depends on them.

## Phases

Each phase ends green on the checks named, and each is a pull request against
`main` from `tgeczy/native-host`.

1. **A portable host, with no change in behaviour.** Vendor Musashi at its
   pinned commit with the one-line `m68kconf.h` change committed rather than
   applied by `sed`. `build_linux.sh` builds `libosp_host.so` on any Linux; a
   `linux.yml` workflow builds it in CI and runs `osp_selftest`, which carries
   its own inputs and touches no engine data. `osp.py` learns the `.so` name so
   the Python driver can render on Linux through the same binding, and a
   render on the Linux box matches Windows byte for byte. The Windows DLLs
   still render the reference WAVs identically.
2. **The text front end in C, with oracles.** `osp_numbers.c` first: it is
   data-free, so CI can diff it against `numwords.py` on every push. Then
   `osp_nrl.c`, with longest-focus-wins and file-order ties and the
   single-letter typing-echo path, diffed against `nrl.py` locally over a
   corpus and the rule file's own assertions.
3. **The engines, one at a time, in the order they were built.** `.sp`, then
   MacinTalk 2 (the Component Manager glue the next two reuse), then
   MacinTalk 3, then Pro with `cami`. Each lands only when the render oracle
   reports zero disagreements across the grid.
4. **Settings and audio into the host.** The 0–100 mappings and the widening
   with volume folded in, so SAPI, Linux and Android hear exactly what NVDA
   hears.
5. **Serve and CLI in C.** The same frames, the same seq-tagged cancel, fd 1
   claimed before the first request. `outspoken_sapi.cpp` launches
   `osp_host.exe --serve` where it launched `python.exe`. A Linux render of
   every voice matches Windows.
6. **NVDA on the high-level API.** Pull model — start, then pull chunks —
   because that is what JNI binds too. Cancel checked between callback rounds
   on the rendering thread: safe now, and real for MacinTalk 2, 3 and Pro for
   the first time, but only once the uncancelled path is proven identical.
   Tab-hold on every engine.
7. **Android, last and gated.** Cross-build the CLI with the NDK and *time*
   MacinTalk Pro on a phone and a watch before designing anything: 17× realtime
   under an interpreter on a desktop is the number to divide. Whether outSPOKEN
   is its own app or a fifth generation inside Panthera's is Tomi's call, asked
   when this phase starts and not before.

## Decisions taken as assumptions

* **Branch, not main.** outSPOKEN ships from `main` and this is weeks of churn;
  the repository's own precedent is `macintalk3-spike` → PR #3.
* **Musashi is vendored**, not fetched. It is MIT, about 700 KB of source, and
  the generated `m68kops.*` stay generated at build. The notice in
  `THIRD_PARTY_LICENSES.md` names the commit.
* **One engine per process stays.** The host is one CPU with global state, and
  making it re-entrant is a refactor this release does not need. Switching
  engines is a shutdown and an init, as today; Android runs one worker per
  engine, as Panthera does.
* **Nothing of Apple's or Berkeley's touches CI.** The oracles that need engine
  data run here and on the Linux box; CI runs the ones that carry their own
  inputs.

<!-- HANDOVER -->
## Where it stands

*Updated 2026-09-19, start of the work.*

* Branch `tgeczy/native-host` created from `main` at `740aaf6` (1.2.3).
* Plan agreed with Tomi on 2026-09-19: parity with Panthera 3.x, numbers in
  both languages in C, Musashi compiled into the library rather than driven
  from Python.
* **Phase 1, Windows half done 2026-09-19.** Musashi vendored at
  `313ebf1` with the hook change committed; `build.sh` no longer fetches or
  `sed`s. `src/osp_host.h` declares the C API and the host includes it.
  `src/osp_selftest.c` passes 12/12 on Windows through `build.sh selftest`.
  Both DLLs rebuilt from the vendored tree render **all 36 voices
  byte-identical** to the binaries they replaced (scratch check, one fixed
  utterance per voice, default settings). `build_linux.sh` and
  `.github/workflows/linux.yml` are written and **unverified**: no compiler
  on this Windows box outside MSVC (Cygwin here has no gcc) and the VM was
  down. First proof will be CI on push, then a render on the VM.
* Not started: the render oracle harness (`tools/render_oracle.py`), and
  everything from Phase 2 on.
* Engine data on this machine: `rom/` in the repository (ignored) and
  `%APPDATA%\nvda\macintalk\outspoken`. The Linux box is `coconut`.
