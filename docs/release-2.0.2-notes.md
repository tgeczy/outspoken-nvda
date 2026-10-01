# outSPOKEN 2.0.2

**An Android release.** Capital letters can be heard on Android, and engine data
you no longer want can be removed from the phone. The NVDA add-on, the SAPI
voices and the Linux builds are unchanged.

## Capital letters are heard again

The Android app gave no sign of a capital letter, and the pitch a calling app
asked for was ignored entirely — which meant **the system pitch slider**, in
Android's own Text-to-speech settings, did nothing either. If you had given up
on that slider, it is worth another try.

The NVDA add-on has raised capitals since 2.0.0, and the engines have all been
able to change pitch since before that. The gap was one number the app never
passed along: it asked the host to apply the pitch offset and always told it
zero. The host already knew what to do with a real one — it has carried the
add-on's own capital-pitch offset from the beginning, including the part where
MacinTalk 1 moves its pitch in hertz while the Speech Manager engines move
theirs in musical steps.

So the offset now uses the same scale the NVDA add-on uses, and a capital is
raised by the same amount in both. Nothing changes for an app that never asks
for a pitch, which is most of them.

Measured on the Nothing Phone through Android's own speech client — not through
a shortcut into the engine, since the gap was in the layer a shortcut would have
skipped — and **on all five engines**: MacinTalk 1, 2, 3, Pro, and Pro's Spanish
voices. Each one speaks differently at normal, raised and lowered pitch, and
coming back to normal reproduces the original audio exactly. That last check is
the capital-letter case itself: one word raised, the next back down, with
nothing left behind to drift the pitch of everything after it.

The host's own pitch arithmetic is held to the add-on's, value for value, by
`tools/settings_oracle.py` — 3989 cases, no disagreement — which is why one
setting means one thing on NVDA, SAPI, Android and Linux alike.

## Removing engine data from the phone

Engine settings could already switch an engine off, which hides its voices and
keeps its files. This is the other half, for when you have decided you do not
want an engine on the phone at all. Until now the only way to get the space back
was a file manager pointed into an app's private storage, which on a modern
Android is no way at all.

On the Setup page, **Remove engine data** lists what is installed with the space
each part takes, and you can check off any number at once. The engines are
listed one by one, and the voices appear as an item of their own, marked as
shared by every engine — because that is what they are. The confirmation names
what is going, says what it frees, and says what the result will be: removing
the voices leaves engines that cannot speak, removing the last engine leaves
voices with nothing to read them, and removing everything leaves the app with
nothing to offer. Whichever it is, it says so before you agree, and it says
plainly that your own copy of the data is untouched, so you can import it again
whenever you like.

Every copy goes, including one sitting in the folder a PC's file window shows.
That matters: anything left there is copied back into the app the next time it
runs, so a removal that missed it would quietly undo itself.

Tested on the Nothing Phone by removing MacinTalk 2 while the engine was live
and speaking, then restarting the app twice and walking back through Setup,
where a missed copy would have reappeared. It stayed gone, the shared voices and
the other four engines were untouched, and the voice list dropped from 36 to 26
exactly as it should.

Panthera 3.3.1 gains the same two things on the same day, so the two projects
stay in step.

## Everything else

No change to the NVDA add-on, the SAPI voices or the Linux builds. The NVDA
add-on and the SAPI installer attached below are the 2.0.1 files, unchanged and
offered for convenience; your add-on and SAPI installs will not offer you an
update, and do not need one.

Nothing of Apple's or Berkeley's is included. Keep using your extracted data.

## Updating

On Android, **Check for updates** on the app's Setup page, or install the APK
below. Nothing to do on NVDA or SAPI.
