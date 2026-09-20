# outSPOKEN 2.0.0 — the host becomes the engine

**Shipped: 2.0.0 was published on 2026-09-20.** This was the plan for the
release, written before the work so the work had something to be measured
against, and kept current as it landed; it stays as the record. The `<!-- HANDOVER -->` block at the end says where things
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
   under an interpreter on a desktop is the number to divide. **Tomi's call,
   2026-09-19: its own app, not a fifth generation inside Panthera's.** The
   app's shape mirrors Panthera's: the voice and engine tabs, the setup flow
   and the zip import stay; what changes is the list of engines and the
   settings each shows. Data arrives as a folder copied over MTP or as a zip
   that holds `outspoken`, `outspoken-data`, or the bare engine folders --
   all three are to be accepted.

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
  `src/osp_selftest.c` passes 12/12 on Windows through `build.sh selftest`
  and in CI on x86-64 and ARM64 Linux (run 35447036401, first push). The DLL
  rebuilt from the vendored tree renders **all 36 voices byte-identical** to
  the 1.2.0-era binary of 2026-08-30, itself built after the last host
  source change (scratch check, one fixed utterance per voice, default
  settings). **Linux, verified 2026-09-19:** on `coconut` (Ubuntu 22.04,
  GCC 11, x86-64) the `.so` built first time, the self-test passed, and the
  Python engines driving it rendered **all 36 voices byte-identical to
  Windows**, Carlos and Catalina included. Phase 1 is complete.
* **A trap, found the hard way:** `osp.py` prefers a DLL beside the module,
  and the repository had a three-week-old copy there from a deploy, so a
  day's checks ran against it while `build/` held the binary under test.
  `OSP_HOST_DLL` now names the library outright; `tests/conftest.py` sets it
  to `build/`, and the tools honour it. Set it when running anything by hand.
* **Phase 2 complete 2026-09-19.** `src/osp_numbers.c` ports
  `numwords.normalise` for both languages and both styles;
  `tools/numbers_oracle.py` diffs it over 6140 generated cases (exit code =
  disagreements), zero, on the 64-bit and 32-bit pairs and on the Linux
  `.so`; it runs in `linux.yml`. `src/osp_nrl.c` ports `nrl.py` (rules,
  respelling, letter names); `tools/nrl_oracle.py` diffs it over 16544
  cases, zero on the first run; needs `RULZ`, so local only.
  `tests/test_numbers_c.py` and `tests/test_nrl_c.py` wrap both for the
  suite: 367 passed, 2 skipped.
* **The render oracle exists** (`tools/render_oracle.py`): a fixed script per
  engine instance, both sides in separate processes, diffed against each
  other and against a frozen baseline. `tests/baseline/renders.json` (64-bit)
  and `renders-x86.json` (32-bit) hold the 2026-08-30 binaries' hashes, 223
  renders each; the current builds match both in full.
* **A pre-existing 32/64 difference, not ours:** the 32-bit host renders
  English Pro (`gala`) differently from the 64-bit host on 14 of 223 steps,
  all longer utterances, by a few dozen samples; the August binaries do the
  same. `cami` and every other engine agree. Suspect the C runtime's
  transcendental functions behind the SANE traps; not measured. Each width
  is held to its own baseline meanwhile.
* **Phase 3, all four engines in C, 2026-09-19.** `src/osp_engine.c` (the
  manifest, file reading, folder listing, MacRoman names, one surface for
  all engines) and `osp_engine_sp.c`, `osp_engine_mtk2.c`,
  `osp_engine_mtk3.c`, `osp_engine_pro.c` (each Python module ported line
  for line; Pro covers `gala` and `cami`). The oracle: **all 36 voices,
  223 renders, byte-identical on the 32-bit pair**, each engine on its first
  run. **The 64-bit pair confirmed for all four as well** once the heap fix
  below let the 1984 engine's full grid finish: 763 renders on `.sp`
  (the 223-render script plus the 540-render grid), zero disagreements.
  Full grids for the other three engines and the Tab-hold test are still
  owed. `stop()` is the no-op it was for MacinTalk 2, 3 and Pro; real
  cancel is Phase 6.
  **Linux, same day:** all four engines through the `.so` on `coconut`,
  223 renders, byte-identical to the Python and to the frozen baseline.
* **The catalogue is in C** (`src/osp_voices.c`): the ttvd as
  VoiceDescription, the engine and voice-part gating, the ids NVDA persists
  and the labels it shows, and the manifest that opens each entry.
  `tools/catalogue_oracle.py` diffs entries, manifests and the skipped list
  against the Python: 36 entries, zero disagreements, first run;
  `tests/test_catalogue_c.py` wraps it. Search roots stay the caller's.
* **Phase 4 host side DONE 2026-09-19** (`src/osp_settings.c`): the rate
  curve, the pitch scale, the 1984 driver's hertz, `osp_apply_settings`
  with the RateCommand/PitchCommand offsets, and the 8-to-16 widening with
  volume and VolumeCommand folded in. `tools/settings_oracle.py` runs the
  driver's own methods against it over every slider value and offset: 3989
  cases, zero disagreements, first run; data-free, so it runs in CI.
  `tests/test_settings_c.py` wraps it. The NVDA driver still does its own
  arithmetic until Phase 6 moves it onto these calls.
* **Phase 5, step 1 DONE 2026-09-19: streaming in C.** The advisor's
  ordering: streaming had to exist in C before the SAPI launcher switches,
  or Pro would regress from a 30 ms first sound to a full-render wait.
  `osp_engine_speak_start` / `osp_engine_pull` / `osp_engine_cancel` on the
  engine layer, with `ospaudio.Stream` ported (the held-back piece, head
  trim first, tail trim last) and MacinTalk 3 and Pro split into begin,
  pump, quiet and drain steps that the blocking call and the pull path
  share. `tests/test_engine_pull.py`: pulled equals blocking on all four
  engines, the first piece arrives early, an abandoned utterance stops
  quickly and does not poison the next. This is also real cancel for
  MacinTalk 2, 3 and Pro from the rendering thread, ahead of Phase 6.
  Measured on the way: MacinTalk 2 carries something of the previous
  utterance into the next (the first "quick brown fox" after "a, b, c, d,
  e" is 8 bytes longer than every repetition after it), in the Python
  reference too; the tests compare like positions.
* **Phase 5, steps 2 to 4 DONE 2026-09-19.** `src/osp_plat.h` with
  `osp_plat_win.c` / `osp_plat_posix.c` (threads, the mutex, binary stdio,
  claiming stdout, the registry, the environment; their own translation
  units so `<windows.h>` never meets the host's names). `src/osp_roots.c`
  is `rom.search_roots()` minus `migrate()`, diffed by
  `tools/roots_oracle.py` (data-free, in CI). `src/osp_serve.c` speaks the
  `OSP4`/`OSPR`/`OSPC` protocol exactly as `osp_serve.py` does -- reader
  thread, seq-tagged cancel, fd 1 claimed before the first request,
  streamed chunks. `src/osp_main.c` is the program: `--serve <config>`,
  `--list <config>`, `--render`, `--capabilities`; `build.sh` makes
  `osp_host.exe` and `osp_host_x86.exe`, `build_linux.sh` makes
  `build/linux/osp_host`. **The swap gate is green:**
  `tests/test_sapi_serve.py` now holds both serve hosts to the in-process
  driver, and `tests/test_serve_hosts.py` compares the two hosts with each
  other over the wire on every engine family, with settings changes, a
  cancel by seq, and the listing. All byte-identical.
* **Phase 6 DONE 2026-09-19: the NVDA driver on the host.** `outspoken.py`
  opens engines through `osp.HostEngine` (catalogue, open, select,
  settings, apply, volume, numbers, translate, speak_start/pull/cancel,
  widen) and no Python engine module runs at speech time. Held by
  `tools/driver_oracle.py`: the bytes fed to the player for nine fixed
  sequences on one voice per family, frozen per width
  (`tests/baseline/driver-feed.json`, `driver-feed-x86.json`) and
  cross-checked with `--driver-rev 0607ca4` against the 1.2.x driver on
  the same host: **equal on 40 of 45 cases, the five "spelling" cases
  differ by design**. (The first frozen baseline had English Pro from the
  32-bit host — the pre-existing 32/64 Pro difference again — which is why
  there is one per width now.) Indexes go on the audio queue on the side
  of the run's audio their place in the sequence says, and the feeder
  reports them when playback reaches them through `feed(onDone=)`, probed
  once (`tests/test_marks.py`, ported from Panthera's; `tests/test_earcons.py`
  passes and lost its expected-failure mark). `CharacterModeCommand` is
  declared and flushes each character as its own utterance. The completion
  notice moves to the feeder, after `idle()` and after the held marks, and
  is owed for every sequence, empty ones included. A cancel mid-sequence
  abandons the rest of it, audio is tagged with the cancel count it was
  rendered under and the feeder drops a stale piece (Panthera's window,
  now one per streamed piece). `_gainTables`, `_to16`, `_pitchTenths`,
  `_baseHz` and `_applySettings` stay as the reference the settings
  oracle runs. Suite: 403 passed on each width (the 32-bit run has its one
  environmental `wx` failure).
* **The SAPI launcher swapped, same day.** `outspoken_sapi.cpp` launches
  `osp_host.exe --serve <dataRoot>` beside it, falling back to
  `osp_host_x86.exe` (a 32-bit Windows gets only that one); `register.ps1`
  and `settings.ps1` list voices with `osp_host --list`; `build.ps1`
  stages both programs from `build/` and keeps the embeddable Python for
  the Extract button only; `installer.iss` ships both. Built and staged:
  `C:\outspoken\sapi\out\outspoken-sapi-2.0.0-setup.exe`. `osp_serve.py`
  stays as the specification. **Tomi's Tab-hold test on every engine,
  through the add-on and through the installer, is the gate that remains.**
* **Release mechanics, same day.** Versions 2.0.0 in `addon/manifest.ini`
  and `installer.iss`; `tools/package.py` now refuses a `.exe` inside the
  add-on outright (secure screens) and allows the `advapi32.dll` import
  the roots port added; `linux.yml` packages
  `outspoken-linux-<arch>.tar.gz` (program, library, header, licences,
  source archive, `docs/linux.md` as its README) and attaches it to a
  release on publish or on `gh workflow run linux.yml -f release_tag=v2.0.0`.
  User-facing notes: `docs/release-2.0.0-notes.md`.
* **PUBLISHED 2026-09-20, 03:45 MT.** Tomi's ear passed on all four fronts
  -- NVDA, SAPI (twice, the settings layer and the upgrade path on the Rog
  Ally), Android (the release build fed through the zip picker on the
  Nothing Phone) -- and he said release. PR #4 merged into `main` as
  54455bc, the release retargeted to `main` and published as latest:
  https://github.com/tgeczy/outspoken-nvda/releases/tag/v2.0.0 with the
  add-on, the SAPI installer, the signed APK and both Linux tarballs, every
  one rebuilt from the final source. Left for after: the Android device
  suite, the watch, the `[[sync]]` interior marks below.
* **Out for Tomi's ear, 2026-09-19 evening.** PR #4 (`tgeczy/native-host`
  → `main`). Draft release `v2.0.0`, target the branch head, carrying
  `outspoken-2.0.0.nvda-addon`, `outspoken-sapi-2.0.0-setup.exe` and the
  two Linux tarballs from the dispatched workflow. **To publish:** merge
  the PR, `gh release edit v2.0.0 --target main` so the tag lands on
  `main` like every Panthera tag since 3.0.0, then publish; the published
  event re-runs `linux.yml`, which leaves the attached tarballs alone. The
  update checkers see nothing while it is a draft.
* **Phase 7's gate measured 2026-09-19, and passed by a distance.** The
  host cross-built for arm64 Android with NDK r27c in one clang command
  (`--target=aarch64-linux-android24`, the `build_linux.sh` sources and
  flags, no source change) and ran on the Nothing Phone from
  `/data/local/tmp`: all 36 voices listed, and the three-sentence test
  text rendered **byte-identical to the Windows host on every engine
  family** (five WAVs, five matching hashes). Render speed with process
  start subtracted, phone against desktop: Pro 52x realtime against 39x,
  Carlos 61x against 41x, MacinTalk 2 and the 1984 driver far above both;
  MacinTalk 3 read 10x on one run against 32x and wants re-measuring.
  Process start plus engine open is 0.04–0.11 s on the phone. So the
  engine side of Android is done; what remains is the app itself, a fork
  of Panthera's (16 Kotlin files, a 171-line JNI whose shape is exactly
  `osp_engine_*`) with the engine list, the settings, one worker per
  engine kind, the `outspoken` data folder, the zip import and Direct
  Boot storage. Not in 2.0.0; Tomi decides whether 2.0.0 waits for it.
* **Phase 7 BUILT 2026-09-19, the same evening: Tomi held 2.0.0 for it.**
  `src/platforms/android` is Panthera's app at `pantheraspeech/v3.2.0`
  with the engine swapped -- `docs/android.md` is the account. One worker
  process for every engine (the host interrupts, so nothing is retired to
  cancel), the catalogue as the voice list, the zip import and the move
  into protected storage rewritten around the extractor's folders with
  `voices` merged, Spanish as a locale, the desktop's sliders per family.
  `build_android.sh` cross-builds `liboutspoken.so` (both ABIs) from the
  Linux sources plus a C JNI; `gradlew assembleDebug` built first time;
  26 JVM tests pass. **On the Nothing Phone:** the preview renders
  byte-identical to the desktop at the same settings (42870 samples both,
  1984 Female, the phone's 215% speech rate being the desktop's slider at
  78), and with `tts_default_synth` set to `com.outspoken.tts` TalkBack's
  first request (Fred, 15 characters) streamed in ~100 ms. Tomi's first
  hearing: "super high pitched and sped up" -- the 1984 Female at 499 wpm,
  which is what 215% of the desktop default is; the default voice is Fred
  since. **Tomi's ear on the phone, the same evening: "Android's perfect.
  no lag when swiping, no oddities in speech. pro works, and the other
  engines do too."** Left: the device suite (`androidTest`) not ported;
  release signing (`signing.properties`, the same convention as
  Panthera's) and the APK on the draft as `outspoken-2.0.0.apk`; the watch
  untried.
* **SAPI aligned to Panthera 3.2.0's settings model, 2026-09-19 afternoon**
  (Tomi: "better to get things right for 2.0 than to rush it"). What was
  already there, since 1.2.x: tokens and the machine-wide DataPath in HKLM
  through both views, "Move engines for all users" with Panthera's plan
  classifier and dialogs, the shared-root ACL, `Offer-Rebind`. What was
  added: `sapi/settings.cpp/.h` (Panthera's reader, namespace and names
  changed; `OUTSPOKEN_SAPI_SETTINGS_USER/_MACHINE` overrides),
  `settings_test.cpp` (34 checks, run by `build.ps1`), the DLL reading
  Diagnostics and ReadTimeoutMs through it plus two new settings,
  **Inflection** and **NumberStyle** (`words`|`digits`), carried to the
  host as `osp_host --serve <root> --inflection N --numbers MODE`
  (`osp_serve_set_defaults`; `osp_serve.py` takes the same flags) with the
  resident host respawned when the launch line changes; the tool's three
  controls, `settings_common.ps1` (the file helpers in PowerShell 2.0's
  dialect, dot-sourced by both scripts), `Move-SettingsOutOfRegistry`,
  `Confirm-SharedLogging`, the elevated trips granting the ProgramData
  folder and mirroring this person's settings into the machine file,
  `installer.iss` `[Dirs]` users-modify. `tests/test_serve_hosts.py` holds
  both hosts to the flags. The sign-in screen itself is untested from here.
  Later the same night, Tomi pointed at Panthera's `a086aa9` (its 3.2.0
  r2, on `main` after the tag): an installer upgrade rebuilt every voice
  token from whatever data the elevated account could see. Ours had the
  same fault and a second one -- the elevated register never read the
  machine-wide DataPath it wrote. Ported: `installer.iss` snapshots its
  uninstall key in both views in `InitializeSetup` and runs
  `register.ps1 -RegisterServer` (COM classes only) on an upgrade,
  `-Register` on a fresh install; `register.ps1` resolves its root through
  HKCU, then the machine HKLM (both views), then NVDA's folder, and its
  token pass is a function the upgrade path skips.
  `tests/test_sapi_installer_upgrade.py` is Panthera's isolated Inno probe
  with our script: five cases, both views, empty and custom selections
  kept on an upgrade, registered afresh on a first install.
* **Parked, with the probe written down:** an index *between* words of one
  run is still reported at the head, because its position in the audio is
  not known. The engine can say: MacinTalk 2, 3 and Pro honour `[[sync
  ID]]` in the text and the Speech Manager calls `soSyncCallBack` when the
  engine reaches it. The probe: set `soSyncCallBack` through
  SetSpeechInfo to a stub in guest memory that the host's trap hook
  recognises, break the render there, record `osp_pcm_len()` as the mark's
  byte position, and never put a sync after final punctuation (it would
  split the sentence's prosody). Hexadecimal IDs. Measured before shipped,
  and by ear, since a sync may itself cost a pause.
* The text calls take MacRoman bytes and return the size needed (`> cap`
  means retry); the NRL calls answer -2 when their table is not loaded;
  `osp_engine_open` answers -100 for an engine not yet ported.

## Found along the way

* **The engines leaked their whole heap, and then went silent** (found
  2026-09-19 by the render oracle's full grid, which wedged on its 133rd
  render). The host's `_DisposePtr` and `_DisposeHandle` were no-ops on a
  bump allocator. The 1984 driver allocates one block per utterance
  (3,688 bytes for a forty-character sentence) and disposes of it; MacinTalk
  Pro allocates and disposes about 57 KB per utterance. So the 512 KB heap
  was full after 123 short utterances and Pro's 10 MB after about 140, at
  which point `_NewPtr` answered memFullErr, the engine spun until its
  instruction budget (seven seconds of nothing) and every utterance after it
  was silent, until a voice switch rebuilt the engine. MacinTalk 2 and 3
  allocate nothing per utterance and were never affected. Shipped in every
  release so far; nobody reported it. **Fixed in the host core**: dispose is
  real and the heap top rolls back over freed blocks, so no address changes
  and every frozen baseline still holds to the byte. `osp_selftest` checks
  the dispose; `tests/test_heap_reuse.py` speaks 200 utterances on the 1984
  engine and 40 on Pro with a stable heap and identical output.

## Asked for along the way (Phase 6, the NVDA driver)

* **Spelling markers must not be blocked** (Tomi, 2026-09-19). NVDA wraps
  single-character strings in `CharacterModeCommand(True/False)`
  (`source/speech/speech.py`, `_getSpellingSpeechAddCharMode` in 2026.2),
  with `PitchCommand` around capitals. **DONE in Phase 6:** declared in
  `supportedCommands`; while it is on, coalescing is suspended and each
  string is flushed as its own utterance. The letter's name is the engine's
  own affair as before (`.sp` already spells a lone letter through
  `letter_name`; the Speech Manager engines say a single letter as a
  letter). `[[char LTRL]]` was not measured and is not used.
* **Prosody across say-all** (Tomi, same day): engines should carry their
  prosody from one say-all chunk into the next without depending on the
  coalescing of adjacent strings. Tomi, later the same day: it matters "a
  bit less" here than in Panthera, but prosody should still continue with
  markers. **Parked** behind the `[[sync]]` probe above: with interior
  marks resolvable, a say-all chunk can stay one utterance across its
  indexes and the engine keeps its sentence prosody. Not for 2.0.0.
* **Earcons and Speech Rules must keep time** (Tomi, same day). The add-on
  at `C:\git\nvda-phonetic-punctuation` — formerly Phonetic Punctuation, in
  use, and the add-on behind Panthera's issue #21 — turns a punctuation
  mark, a role or a state into a `BaseCallbackCommand` (an `IndexCommand` by
  the time it reaches a driver) **followed by a `BreakCommand` sized to the
  sound**, and expresses headings and formatting as additive `PitchCommand`,
  `VolumeCommand` and `RateCommand` offsets, 0 meaning the user's setting
  again. The break and the prosody are already honoured here. The index is
  not kept in time: `_flush` reports every index it has collected *before*
  it renders the run, so for `A | index | break | B` the earcon sounds over
  the start of A and the silence lands after A. Measured, not just read:
  `tests/test_earcons.py` speaks `hello | index 7 | break 300 | world` and
  records `index 7, feed hello, feed 300 ms of silence, feed world`; it
  asserts the required order and was a strict expected failure until Phase
  6 landed the fix, when the mark came off. **DONE.** The design below is
  what was built; `tests/test_marks.py` measures each case. That is Panthera's
  breathing-on fault, and outSPOKEN has no breath to buy with it: no engine
  here breathes, the cross-index sentence joiner Panthera needs never comes
  here, and the adjacent-string coalescing of 0.8.0 is a different mechanism
  that stays (the bullet above uses the same word for it). So take
  Panthera's breathing-off path unconditionally, with no checkbox:
  `speech_pipeline.py`'s `_feedPiece`/`_played`/`_markReached`/`_marksDrained`
  and `panthera/tests/test_marks.py` are the model. A head index (before the
  run's text) reports when the run starts to play, which keeps say-all one
  line ahead as today; a tail index (text before it, a break, a prosody
  change or the end after it) reports when the speech has played and
  *before* the appended silence, so NVDA's next push renders under the
  pause; an interior index stays at the head, its position unknown. A break
  must keep forcing a flush — the add-on's trailing break is what makes a
  tail possible. The traps are Panthera's: `cancel()` bumps a generation,
  because NVDA's `stop()` drops `onDone` callbacks without firing them;
  drain the marks after `idle()` so every index precedes
  `synthDoneSpeaking`; never take the player lock inside the callback, WASAPI
  fires it inside `feed()` on the feeder thread. The manifest admits 2023.1,
  which predates `onDone`, so probe `inspect.signature(feed)` once and fall
  back to reporting at dequeue. Tests: the `FakeWavePlayer` in
  `tests/conftest.py` learns `onDone` and mirrors NVDA's timing, as
  Panthera's did; the cases are the earcon shape above, a say-all head
  index, and a cancel mid-run that loses no index. Python only, so it could
  ship ahead of 2.0.0 if wanted.
* **NVDA stays in-process.** Tomi, 2026-09-19: the DLL-and-Python route is
  what keeps secure screens working, and it always has; the separate host
  process is for SAPI, Linux and Android only.
* Engine data on this machine: `rom/` in the repository (ignored) and
  `%APPDATA%\nvda\macintalk\outspoken`. The Linux box is `coconut`.
