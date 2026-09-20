# outSPOKEN 2.0.1

**A startup fix.** Nothing about the voices changes.

## What was wrong

2.0.0 took three to five seconds to load the synthesizer in NVDA on some
machines, before any voice spoke. The host builds its voice list by walking
the folders it is told to search, and one of those is NVDA's whole
configuration folder. On a machine that also has Panthera's speech data, that
folder holds gigabytes of Mac OS X voice files under the shared `macintalk`
folder, plus every add-on's code, and the walk went through all of it at
every load. The 1.2.x catalogue only ever looked in a few fixed places, so
it never paid this. The SAPI engine and the Android app read far smaller
folders, which is why neither showed it.

## What changed

The walk no longer enters folders that can never hold these engines and are
known to be large: NVDA's `addons`, `profiles`, `speechDicts`, `scratchpad`
and `updates`, Panthera's `tiger`, `leopard`, `snowleopard` and `lion`, the
data managers' backup folders, and it goes no deeper than the layouts need.
Measured on the development machine, the scan of the configuration folder
went from 643 ms to 33 ms, and the whole synthesizer load to about a tenth
of a second. The voices found are exactly the same; the catalogue is still
held to the Python reference, entry for entry, and new tests plant an
engine where the walk must not look and where it must.

The same host program is inside the SAPI installer, so the settings window's
listing and the engine's start share the fix. The Android app is unchanged
and stays at 2.0.0.

## Updating

Press **Check for updates** in the speech data manager (NVDA's Tools menu)
or in the SAPI settings window, and this release installs itself. The NVDA
add-on, the SAPI installer and the Linux tarballs are attached below.
