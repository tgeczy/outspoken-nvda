# -*- coding: utf-8 -*-
"""Build the `.nvda-addon`, and refuse to build one that ships an engine.

An NVDA add-on is a zip with a different extension, so this is short. The part
worth having is the refusal: **nothing in MacinTalk, outSPOKEN or any voice may
leave this machine.** Those belong to Katz and Barton, to Berkeley Systems and
to Apple; the user supplies them from their own copy, and the whole arrangement
fails the first time one of them lands in a release.

`.gitignore` protects the repository. This protects the thing people actually
download, which git never sees.

Both binaries go in. NVDA was a 32-bit process for most of its life, and an
add-on carrying only the 64-bit build fails on the rest with
"[WinError 193] %1 is not a valid Win32 application" -- reported by a user as
"it loads but there is no speech", because the synthesizer appeared in the list
and only failed once it was selected.

    sh build.sh                 # produces both DLLs
    py -3 tools/package.py      # produces outspoken-<version>.nvda-addon
"""
import os
import sys
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ADDON = os.path.join(ROOT, "addon")
BUILD = os.path.join(ROOT, "build")

#: The two binaries the add-on cannot run without, and where each belongs.
DLLS = [("osp_host.dll", "synthDrivers/_outspoken/osp_host.dll"),
        ("osp_host_x86.dll", "synthDrivers/_outspoken/osp_host_x86.dll")]

#: Anything matching these must never be packaged. Extensions catch the bulk;
#: the names catch a file someone renamed without thinking.
FORBIDDEN_EXT = (".bin", ".rsrc", ".hfv", ".dsk", ".img", ".wav", ".sit")
FORBIDDEN_NAME = ("drvr", "talk", "rulz", "dict", "phnm", "cecy", "ttv",
                  "ttsr", "ttsd", "ttss", "gtse", "gtss", "outspoken.bin",
                  "cmudict")

SKIP_DIRS = {"__pycache__", ".git", "rom"}


def forbidden(rel, name):
    low = name.lower()
    if low.endswith(FORBIDDEN_EXT) and not low.endswith(".dll"):
        return "%s looks like extracted engine data" % rel
    stem = os.path.splitext(low)[0]
    for bad in FORBIDDEN_NAME:
        if stem.startswith(bad):
            return "%s is named like an engine resource" % rel
    return None


def manifest_version():
    path = os.path.join(ADDON, "manifest.ini")
    for line in open(path, encoding="utf-8"):
        if line.strip().startswith("version"):
            return line.split("=", 1)[1].strip()
    raise SystemExit("no version in manifest.ini")


def main():
    version = manifest_version()
    out = os.path.join(ROOT, "outspoken-%s.nvda-addon" % version)

    files, refused = [], []
    for dirpath, dirs, names in os.walk(ADDON):
        dirs[:] = [d for d in dirs if d not in SKIP_DIRS]
        for n in names:
            full = os.path.join(dirpath, n)
            rel = os.path.relpath(full, ADDON).replace("\\", "/")
            why = forbidden(rel, n)
            if why:
                refused.append(why)
            else:
                files.append((full, rel))

    # Both binaries, taken from build/ rather than from whatever happens to be
    # sitting in the add-on folder after a deploy.
    missing = []
    for name, rel in DLLS:
        src = os.path.join(BUILD, name)
        if os.path.isfile(src):
            files = [(f, r) for f, r in files if r != rel]
            files.append((src, rel))
        else:
            missing.append(name)

    if refused:
        print("REFUSING to package. These are not ours to distribute:")
        for r in sorted(set(refused)):
            print("   " + r)
        print("\nThe engine is supplied by the user, from their own copy.\n"
              "See README.md and tools/extract_rom.py.")
        return 1
    if missing:
        print("missing %s -- run `sh build.sh`, which produces both"
              % " and ".join(missing))
        return 1

    with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
        for full, rel in sorted(files, key=lambda x: x[1]):
            z.write(full, rel)

    print("wrote %s" % os.path.relpath(out, ROOT))
    print("  %d files, %d KB" % (len(files), os.path.getsize(out) // 1024))
    for _full, rel in sorted(files, key=lambda x: x[1]):
        if rel.endswith(".dll"):
            print("  %s" % rel)
    return 0


if __name__ == "__main__":
    sys.exit(main())
