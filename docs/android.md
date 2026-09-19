# outSPOKEN on Android

The MacinTalk voices as an Android text-to-speech engine, for TalkBack and
anything else that speaks through the system's TTS. `src/platforms/android`
is Panthera's Android app at its 3.2.0 release with the engine swapped: the
same Kotlin shape -- a public `TextToSpeechService`, a private worker process,
the Setup and Engine settings screens, the zip import, Direct Boot storage,
the update check -- on the native host this repository builds for every
platform. Nothing of Apple's or Berkeley's ships in the APK.

## How it runs

**One worker process for every engine.** Panthera keeps one worker per Mac OS
X generation and kills a worker to interrupt it, because its engine cannot be
stopped mid-utterance. This host can be: `osp_engine_stop()` is the one
cross-thread call, it makes a render in progress return early, and
`osp_engine_cancel()` abandons the rest between pull rounds. And the host
opens whichever engine a voice belongs to, closing the one before, in tens of
milliseconds on a phone. So `OutspokenWorkerService` is one process
(`:engine`) holding one open engine; `OutspokenWorkers` binds it once and
retires it only when its Binder is found dead. A stop from TalkBack goes
straight to the worker's stop flag from the Binder thread, then the cancel is
queued behind the pull in progress; the worker stays. Measured on the Nothing
Phone: a fifteen-character request for Fred renders and streams in about a
hundred milliseconds, and MacinTalk Pro renders at about fifty times real
time, byte-identical to the Windows host.

**The voice list is the host's catalogue.** `OutspokenNative.nativeScan` is
`osp_catalogue_scan` called in the app's own process -- a folder walk, no
guest code -- and it answers the same ids and labels the NVDA driver and the
SAPI engine use: `mtk3:Fred`, `gala:Bruce`, `cami:Carlos`. The worker scans
again when it opens a voice, because a manifest belongs to the process that
scanned it. Android's voice id is `outspoken-<family>-<name>`; the label is
"Fred (MacinTalk 3)", the SAPI token's shape. Picking a voice picks its
engine family, and each family remembers its own settings.

**Spanish is a locale.** Carlos and Catalina carry `es`, so `onGetVoices`
reports them as `es-MX`, `onIsLanguageAvailable` answers `spa`/`MEX`, and
`CHECK_TTS_DATA` lists `spa-MEX` beside `eng-USA` when they are present. A
request in Spanish gets a Spanish voice whatever the "use selected voice in
all apps" switch says.

**Text.** TalkBack hands over code points, so `Emoji.describe` (David
Sexton's BSD tables, from doubledroid) turns an emoji into words first; the
rest is the host's, exactly as on the desktop: UTF-8 in, MacRoman by
`osp_text_macroman`, then each engine's own translate -- numbers in English
or Spanish, punctuation, the 1984 rules. An utterance is kept whole; the host
streams it as the engine renders and a cancel lands inside the render, so
nothing is cut for the sake of interrupting it.

**Settings** are the desktop's sliders, 0-100, per family: rate (50 is the
voice's own speed), pitch, inflection, volume, and how numbers are read
(words, digits, or as the engine reads them). The speech rate an app asks
for -- the system's, or TalkBack's own -- is applied on top of the rate
slider as a percentage, 100 being normal: at the system's 215% the engine
speaks at what the desktop's slider would call 78. That is how every Android
engine treats the system rate, and it is why a first test at a screen
reader's pace sounds fast.

## Where the data goes

The app's device-protected storage, so the voices speak on the lock screen
after a reboot, before the phone is unlocked (Panthera's Direct Boot work,
carried over whole): `/data/user_de/0/com.outspoken.tts/files/outspoken-data`.
Two routes in, both ending there:

* **Extract engine from zip file** on the Setup page. Zip the `outspoken`
  folder the desktop add-on extracted -- `macintalk1`, `macintalk2`,
  `macintalk3`, `macintalkpro`, `macintalkespanol`, `voices`, whichever you
  have -- and choose the zip. `ZipImport` reads the central directory, finds
  those folders wherever they sit (at the top, inside `outspoken` or
  `outspoken-data`, inside `macintalk/outspoken` as on the desktop, or under
  any folder at all; an engine folder is known by its marker files, `Cecy_1.bin`
  and the like, whatever it is called), streams each into `<name>.importing`,
  and swaps them in at the end. Engine folders are replaced; `voices` is
  merged, voice folder by voice folder, because three engines share it and a
  zip may carry one new voice. A zip holding a `MacinTalk.SpeechSynthesizer`
  bundle is Panthera's, and is said so.
* **Copy by hand** over the PC's file window into the inbox the Setup page
  shows, `Android/data/com.outspoken.tts/files/outspoken-data`: the `outspoken`
  folder as it is, or its contents. The next time the app runs unlocked it
  moves everything into protected storage, folder by folder, stepping into a
  wrapper folder so the data lands flat, and merging `voices`.

The host finds engine files anywhere under a root but wants `voices` directly
in one, so `OutspokenEngine.candidateRoots` names each base root and any
wrapper folder one or two levels down that holds the data.

## Building

```
sh build.sh              # or build_linux.sh: generates third_party/musashi/m68kops.c
sh build_android.sh      # liboutspoken.so for arm64-v8a and armeabi-v7a, into jniLibs
cd src/platforms/android
./gradlew assembleDebug  # app/build/outputs/apk/debug/app-debug.apk
./gradlew testDebugUnitTest
```

`build_android.sh` uses the NDK's clang directly with `build_linux.sh`'s
sources and flags plus `app/src/main/cpp/outspoken_jni.c`, a C bridge over
`osp_*` -- no CMake, no C++, no libc++ in the APK. It also leaves an
`osp_host` program per ABI under `build/android/` for a shell check on a
device. Gradle checks the libraries are there and stages the licence notices
from `src/platforms/android/licenses` into the APK's assets.

A release is `./gradlew assembleRelease` with a `signing.properties` beside
`settings.gradle.kts` naming the key (`STORE_FILE`, `STORE_PASSWORD`,
`KEY_ALIAS`, `KEY_PASSWORD`; ignored by Git); without it the build stays
unsigned. The same key must sign every release. The application id,
`com.outspoken.tts`, is permanent once shipped.

## The debug route onto a phone

A debug build can be fed by adb without the file window:

```
adb install -r app/build/outputs/apk/debug/app-debug.apk
adb push <extracted outspoken folder> /data/local/tmp/osp/data/macintalk/
adb shell chmod -R a+rX /data/local/tmp/osp/data
adb shell "run-as com.outspoken.tts sh -c 'mkdir -p /data/user_de/0/com.outspoken.tts/files/outspoken-data && cp -r /data/local/tmp/osp/data/macintalk/outspoken/. /data/user_de/0/com.outspoken.tts/files/outspoken-data/'"
adb shell am start -n com.outspoken.tts/.SettingsActivity --ez autospeak true
adb shell settings put secure tts_default_synth com.outspoken.tts
adb logcat -s OutspokenTts:* OutspokenEngine:* Outspoken:*
```

In Git Bash on Windows set `MSYS_NO_PATHCONV=1` first, or every `/data/...`
argument is rewritten to a Windows path before adb sees it. `--es import
<path>` on the Settings activity imports a zip the app can read without the
confirm dialog. A release build cannot be fed this way: it takes the file
window or the zip picker, like a user's.

## Not carried over yet

Panthera's device suite (`EngineSmokeTest`, the worker, latency and zip
checks under `androidTest`) was written around its generations and is not
ported; the JVM tests are, adapted. The watch has not been tried.
