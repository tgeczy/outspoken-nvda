# -*- coding: utf-8 -*-
"""Is there a newer release than the one running?

Ported from Panthera, where Sean asked for a check-for-updates button rather
than having to watch the repository; this add-on's users watch the same
repositories.

**Only ever when somebody presses the button.**  Nothing here runs on a
timer and nothing runs at start-up.  A screen reader that quietly contacts a
server every launch is telling that server when its owner sits down at their
machine, and nobody asked it to.  The button is the consent.

**And never on a secure screen.**  There NVDA is SYSTEM on the sign-in
desktop; reaching onto the network as SYSTEM, from a dialog nobody can be
logged in to have opened, is not a thing this add-on should be able to do.
The caller hides the button there; this module refuses as well, because a
guard that lives only in the caller is a guard that moves.

The version comparison is pure and the fetch is not, so the comparison is
tested and the fetch is kept to the smallest piece of code that can be
wrong.

Flat imports and no package, like every module in this folder -- the SAPI
bridge stages these files loose next to an embeddable Python whose `._pth`
locks `sys.path`, and a relative import would be the first thing to break
there.
"""
import os
import re

#: GitHub's own "newest published release" endpoint.  It excludes drafts and
#: prereleases without being asked to, which is exactly the wanted behaviour:
#: a draft under test must never advertise itself to everybody.
LATEST_API = ("https://api.github.com/repos/tgeczy/outspoken-nvda"
              "/releases/latest")

#: Where to send somebody who wants it.  The human page, not the API.
LATEST_PAGE = "https://github.com/tgeczy/outspoken-nvda/releases/latest"

#: This repository tags releases `v1.1.1`; hand-typed and future shapes may
#: drop the `v` or add a suffix.  Anything after the numbers -- `-rc1` -- is
#: matched but not compared: a prerelease is not offered by this endpoint
#: anyway, and a suffix must never make a version look *newer*.
_VERSION = re.compile(r"(\d+(?:\.\d+)*)")


def parse_version(text):
    """-> a tuple of ints, or None if there is no version in `text`."""
    if not text:
        return None
    found = _VERSION.search(str(text))
    if not found:
        return None
    try:
        return tuple(int(part) for part in found.group(1).split("."))
    except ValueError:
        return None


def is_newer(latest, installed):
    """-> True when `latest` is a strictly higher version than `installed`.

    Padded to the same length so 1.2 beats 1.1.9 and 1.1 ties 1.1.0.  Either
    side unparseable is False: an add-on that cannot tell should say nothing
    rather than announce an update that may not exist.
    """
    a, b = parse_version(latest), parse_version(installed)
    if not a or not b:
        return False
    width = max(len(a), len(b))
    a = a + (0,) * (width - len(a))
    b = b + (0,) * (width - len(b))
    return a > b


def installed_version():
    """-> the running add-on's version, or None.

    `addonHandler` is the right answer inside NVDA and absent everywhere
    else, so the manifest two levels up is the fallback -- which is what
    `addonHandler` would have read anyway.
    """
    try:
        import addonHandler
        addon = addonHandler.getCodeAddon()
        if addon and addon.version:
            return addon.version
    except Exception:
        pass
    here = os.path.dirname(os.path.abspath(__file__))
    manifest = os.path.join(os.path.dirname(os.path.dirname(here)),
                            "manifest.ini")
    try:
        with open(manifest, encoding="utf-8") as f:
            for line in f:
                if line.strip().startswith("version"):
                    return line.split("=", 1)[1].strip()
    except (OSError, IndexError):
        pass
    return None


def _secure():
    """-> True on a secure screen, and True only then.

    Outside NVDA entirely -- the tests, the SAPI bridge, a command line --
    is not a secure screen, and only an NVDA that answers `secure` is
    treated as one.
    """
    try:
        import globalVars
        return bool(globalVars.appArgs.secure)
    except Exception:
        return False


def latest_release(timeout=10, opener=None):
    """-> (versionString, pageUrl), or (None, reason).

    `opener` exists for the tests: anything callable that takes a URL and
    returns bytes.  The default reaches the network, which is why it is the
    only part of this module that is not tested.
    """
    if _secure():
        return None, "not while NVDA is on a secure screen"
    try:
        import json
        if opener is None:
            import urllib.request

            def opener(url):
                request = urllib.request.Request(url, headers={
                    # GitHub refuses an unidentified caller, and this says
                    # who we are without saying anything about the machine.
                    "User-Agent": "outspoken-nvda-addon",
                    "Accept": "application/vnd.github+json",
                })
                with urllib.request.urlopen(request, timeout=timeout) as r:
                    return r.read()

        payload = json.loads(opener(LATEST_API).decode("utf-8"))
    except Exception as e:
        return None, str(e) or e.__class__.__name__
    tag = payload.get("tag_name") or payload.get("name")
    if not parse_version(tag):
        return None, "the newest release does not name a version"
    return tag, (payload.get("html_url") or LATEST_PAGE)
