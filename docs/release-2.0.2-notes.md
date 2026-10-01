# outSPOKEN 2.0.2

Pitch changes are heard on Android at last, engine data you no longer want can be
removed from the phone, and **every updater now knows which version it is
actually looking at** -- which is why this release carries a new add-on and a new
SAPI installer even though the speech they produce has not changed.

## Pitch changes are heard again

**The Android app ignored the pitch a calling app asked for**, and told the host
the offset was zero every time. So nothing that asked for a pitch got it: not
**the system pitch slider** in Android's own Text-to-speech settings, and not any
pitch change a screen reader makes to tell you something. If you had given up on
that slider, it is worth another try.

The easiest one to hear is **deleting text**: TalkBack speaks the character it
removed at a raised pitch, and on outSPOKEN that character used to come back at
the same pitch as everything else. Tomi confirmed it by ear this way, on this app
and on Panthera's. For capital letters specifically, how TalkBack marks them is
TalkBack's own setting rather than ours -- saying the word "capital" is what it
does by default, and it does not announce every capital as you type the way
VoiceOver does. If you would rather hear them by pitch, TalkBack can do that: in
TalkBack settings, under **Verbosity** or **Keyboard feedback** depending on your
version, set the capital letters option to change pitch. It reaches the engine by
the same route a deleted character does, so if one is audible the other is too.

The NVDA add-on has raised pitch on request since 2.0.0, and the engines have all
been able to change pitch since before that. The gap was one number the app never
passed along. The host already knew what to do with a real one -- it has carried
the add-on's own offset from the beginning, including the part where MacinTalk 1
moves its pitch in hertz while the Speech Manager engines move theirs in musical
steps -- so the offset now uses the same scale the add-on uses, and the same
request is raised by the same amount in both. Nothing changes for an app that
never asks for a pitch, which is most of them.

Measured on the Nothing Phone through Android's own speech client -- not through
a shortcut into the engine, since the gap was in the layer a shortcut would have
skipped -- and **on all five engines**: MacinTalk 1, 2, 3, Pro, and Pro's Spanish
voices. Each one speaks differently at normal, raised and lowered pitch, and
coming back to normal reproduces the original audio exactly. That last check is
the one that matters for a raised character: one word up, the next back down,
with nothing left behind to drift the pitch of everything after it.

The host's own pitch arithmetic is held to the add-on's, value for value, by
`tools/settings_oracle.py` -- 3989 cases, no disagreement -- which is why one
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

## Every updater reads the version off the file, not off the release

**Check for updates could tell you a version was available and then hand you the
one you already had** -- again and again, every time you pressed it.

A release carries four things that do not move in step: the add-on, the SAPI
installer, the Android APK and the Linux tarballs. Only one release can be
GitHub's "latest". All three updaters read the version from the release's tag,
which names the release and not the file inside it, so a release tagged newer than
its add-on claimed an add-on update that did not exist -- and kept claiming it,
because what you had installed never caught up with a tag.

The way round it was to leave an Android-only release unmarked, so the desktop
updaters never saw it. That worked and it cost something real: the newest release
was then findable only by knowing it was there, and anyone who went to "latest"
landed on an older set of files. For somebody moving through a releases page with
a screen reader, hunting for which of several entries holds the current download
is work nobody should have to do.

So each thing is now versioned by **its own filename**.
`outspoken-2.0.2.nvda-addon` says 2.0.2 wherever it sits, and the updaters walk
the recent releases for the newest file of their own kind. A release can therefore
keep older files alongside new ones -- so that everything for a version is in one
place -- and still never offer you something you have. From now on the newest
release is simply the latest one, with every file in it.

The SAPI check also reads the release list as data rather than searching the text
for the first `-setup.exe` it could find, which was whichever release GitHub
happened to list first and need not have been this project's installer at all.

Nothing about this changes what the voices sound like.

## Everything else

Linux support is unchanged, and the tarballs attached here are the 2.0.1 builds.

Nothing of Apple's or Berkeley's is included. Keep using your extracted data.

## Updating

Press **Check for updates** in the speech data manager (NVDA's Tools menu) or in
the SAPI settings window, and this release installs itself; on Android, Check for
updates on the app's Setup page. The NVDA add-on, the SAPI installer, the APK and
the Linux tarballs are all attached below.
