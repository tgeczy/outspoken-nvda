# outSPOKEN on Linux

The MacinTalk engines — the 1984 driver, MacinTalk 2, MacinTalk 3 and
MacinTalk Pro in English and Mexican Spanish — as one native program and one
shared library, built from the same source the Windows NVDA add-on and the
SAPI 5 engine run. Every voice renders byte for byte what it renders on
Windows.

This tarball contains **no engine data**. The engines belong to Apple and to
Berkeley Systems; you supply them from your own copy of the software, and the
extractor in the source tree stages them. See "Your own data" below.

## What is in the tarball

| file | what it is |
|---|---|
| `osp_host` | the program: list voices, render text to a WAV file, or serve speech over stdin/stdout |
| `libosp_host.so` | the same engine as a library, for anything that would rather call it directly |
| `include/osp_host.h` | the library's C interface, documented in place |
| `README.md` | this file |
| `LICENSE`, `THIRD_PARTY_LICENSES.md` | outSPOKEN is MIT; the 68000 interpreter inside it, Musashi, is MIT too |
| `sources/outspoken-nvda.tar.gz` | the source tree this build came from, interpreter included; `sh build_linux.sh` rebuilds it with GCC or Clang and nothing else |

Built on Ubuntu 22.04, so it runs on glibc 2.35 or newer, on x86-64 and on
64-bit ARM.

## Your own data

`osp_host` looks for extracted engines under a *data root*: a folder whose
`macintalk/outspoken` subfolder holds the engine folders the extractor
creates. On Windows that folder is NVDA's configuration folder; here it is
whatever you pass:

```
osp_host --list ~/outspoken-data
```

To extract from a disk image, a `.bin` or the Spanish floppy set, run the
extractor from the source tree with any Python 3 (it is pure Python):

```
tar -xzf sources/outspoken-nvda.tar.gz
python3 outspoken-nvda/tools/extract_rom.py <your image or folder> --out ~/outspoken-data/macintalk/outspoken
```

`extract_rom.py --help` lists the formats it reads.

## Rendering

```
osp_host --list ~/outspoken-data
osp_host --render --config ~/outspoken-data --voice mtk2:Ben \
         --text "Hello there. You owe 1,234 dollars." --output hello.wav
osp_host --render --config ~/outspoken-data --voice cami:Carlos \
         --input texto.txt --output carlos.wav --rate 60 --pitch 50 --numbers words
```

`--rate`, `--pitch` and `--volume` take the NVDA slider's 0–100; `--numbers`
is `off`, `words` or `digits`. The output is 16-bit mono at 22254 Hz, which
is the rate the engines run at. `osp_host --help` has the rest; `--root DIR`
adds a folder to search.

## Serving

`osp_host --serve <data root>` speaks the same request/response protocol the
Windows SAPI engine uses over its pipe — a request is a voice id, the three
sliders and MacRoman text; the reply streams 16-bit PCM in chunks and can be
cancelled by sequence number. `sapi/osp_serve.py` in the source tree is the
protocol's specification, and `tests/test_serve_hosts.py` holds the program
to it. That is the seam for a speech-dispatcher module or any other front end
that would rather keep the engine in a separate process.

## Linking

`libosp_host.so` exports everything the program uses: open an engine from the
catalogue, set rate, pitch, inflection and volume, translate, speak, pull audio
as it renders, cancel. `include/osp_host.h` documents each call, including the
"size needed" contract the text and catalogue calls share.
