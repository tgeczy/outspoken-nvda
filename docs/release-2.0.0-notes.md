# outSPOKEN 2.0.0

**The engines are native now — on Windows, on Linux, and in every front end
at once.** The same five MacinTalk synthesizers and the same thirty-six
voices, rendered byte for byte as before, from one native host that owns
everything between the text and the sound. Nothing about the voices changes;
almost everything underneath them does.

## What the major number is for

Until now the NVDA add-on ran the engines through a small emulator DLL and a
lot of Python: the Berkeley pronunciation rules, number reading in English and
Spanish, the Speech Manager glue that talks to MacinTalk 2, 3 and Pro, the
settings arithmetic, the audio conversion. The SAPI engine ran that same
Python in a helper process. 2.0.0 moves all of it into the host itself, in
plain C, with the 68000 interpreter (Karl Stenerud's Musashi) compiled in.

* **NVDA** loads the host as a DLL, exactly as before — which is what keeps
  these voices alive on the sign-in screen and the other secure desktops,
  where NVDA drops every add-on that needs a program of its own.
* **SAPI 5** launches `osp_host.exe` in place of the Python helper. It is the
  same code the add-on loads, so the SAPI voice is still identical to the NVDA
  voice — the tests that assert that byte for byte are still green, and a new
  one holds the native host to the Python bridge it replaced.
* **Linux**, for the first time: `osp_host` renders any voice to a WAV file,
  lists what you have extracted, or serves speech over a pipe for whatever
  front end you build on it; `libosp_host.so` is the same engine as a
  library. Built for x86-64 and 64-bit ARM on Ubuntu 22.04, attached below,
  with the source it came from and a README inside.

Nothing here is a rewrite by ear. Each piece of Python became the
specification for its C, and an oracle diffs the two: 6,140 number-reading
cases, 16,544 pronunciation-rule cases, the settings arithmetic over every
slider value, the voice catalogue, and every voice rendered across a grid of
rate, pitch, inflection and volume — thousands of renders per engine, zero
disagreements, on 64-bit Windows, 32-bit Windows and Linux. The Python stays
in the repository, readable, and keeps running in the tests as the reference.

## Found on the way, and fixed

**The 1984 voices and MacinTalk Pro went silent after a hundred or so short
utterances.** The host never gave memory back when an engine freed it, so the
original driver's heap filled after about 120 utterances and Pro's after
about 140, at which point the engine could not allocate, spun for seven
seconds, and said nothing further until you changed voice. MacinTalk 2 and
3 were never affected. It has been there since the first release and nobody
reported it — the grid of renders that proved the port found it, on its
133rd render. Memory is freed properly now, in every front end at once.

## What NVDA users will notice

* **Sounds land where they belong.** An index — NVDA's own spelling-error and
  indentation sounds, the say-all cursor, an earcon from the Earcons and
  Speech Rules add-on — is now reported when its audio has actually played,
  not when it was rendered. An earcon placed after a word sounds after the
  word, with its pause after the sound, instead of over the start of the word.
* **Spelling is honoured.** NVDA marks each spelled character with a spelling
  command; the driver now accepts it and speaks each character as its own
  utterance instead of running the letters together.
* **Interrupting is real for every engine.** MacinTalk 2, 3 and Pro could not
  be stopped mid-render before; a long sentence finished rendering after you
  had moved on. Now a cancel abandons the render within a buffer, and the
  rest of a cancelled sequence is abandoned with it.
* NVDA is told an utterance is finished when it has finished *sounding*, and
  after every index in it — which is what a configuration-profile switch
  waits for.

## Unchanged, on purpose

No engine data ships with anything here — not the add-on, not the SAPI
installer, not the Linux tarball — and the packaging refuses to build a
release that contains any. You supply your own copy and the extractor stages
it. The add-on still contains no program file, only the DLL, for the sake of
the secure screens.

Android is not part of this release; it is its own app, later, on the same
host.

## Updating

Press **Check for updates** in the speech data manager (NVDA's Tools menu)
or in the SAPI settings window, and this release installs itself. The NVDA
add-on, the SAPI installer and the Linux tarballs are attached below.
