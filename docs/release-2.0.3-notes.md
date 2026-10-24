# outSPOKEN 2.0.3

**A battery fix for the SAPI voices.** Only the SAPI installer is new; the voices
sound exactly as they did, and nothing changes for NVDA or Android.

## What was wrong

The SAPI voices speak through a small background program, `osp_host.exe`, which
a screen reader or a program like Balabolka starts when it first uses an outSPOKEN voice
and keeps for as long as it holds that voice -- often the whole session. Between
utterances that program had nothing to do, but it checked for new text every
2 milliseconds anyway, which Windows 11 rounds to about **63 times a second**,
for as long as it was running. Each check costs almost nothing; waking that
often is what keeps a laptop's processor from settling into its low-power
states.

Now it sleeps until there is something to say, and the request wakes it the
moment it arrives. The same idle host wakes about once a second. A short
request also gets its first sound a little sooner than before (a median of
20 ms against 29 ms on the test machine), because the old check could
oversleep by a whole timer tick before noticing new text.

This came to light while fixing the same kind of bug in Panthera, its sibling
add-on, after Christopher reported Panthera draining a phone's battery. On
outSPOKEN only the SAPI program had it: the NVDA driver waits for its work
without checking on a timer, and the Android app has no such loop at all.

Measured on Windows 11 with Fred: the audio is byte-identical to 2.0.2's,
including speech after the host has sat idle, and the 32-bit host's to the
64-bit host's; both idle at about one wakeup a second. The host still renders
byte-for-byte what the protocol's Python reference does for every engine
family. Windows 7 was not available to test.

## Everything else

The NVDA add-on and the APK attached here are the 2.0.2 files, unchanged, so
their update checks stay quiet: each one reads the version off its own file.

The Linux tarballs are built for 2.0.3 as this release is published. The only
change in them is the same fix to `osp_host --serve`, which matters only to
something you have connected to it yourself; the 2.0.2 tarballs stay alongside.

Nothing of Apple's or Berkeley's is included. Keep using your extracted data.

## Updating

Press **Check for updates** in the SAPI settings window and this release
installs itself, or run `outspoken-sapi-2.0.3-setup.exe` from the files below.
