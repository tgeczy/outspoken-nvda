# -*- coding: utf-8 -*-
"""The driver's own bytes: what outspoken.py feeds the player for a sequence.

The render oracle holds the engine layer and the settings oracle holds the
arithmetic; this holds the driver's sequence handling -- coalescing adjacent
strings, a BreakCommand's silence, a PitchCommand or VolumeCommand or
RateCommand part-way through, indexes, a spelling-shaped run of single
characters -- by capturing the bytes `SynthDriver` feeds `nvwave.WavePlayer`
for fixed sequences, one voice per engine family, and hashing them.

`tests/baseline/driver-feed.json` is those hashes as the 1.2.x driver
produced them, through the Python engine modules.  The 2.0 driver, on the
host's C engines, must match on every case whose semantics were not
deliberately changed.  Index timing changes nothing here (only when a
notification fires); spelling changes bytes only for the single-character
case, recorded on its own so that diff is a decision and not a surprise.

    py -3 tools/driver_oracle.py            # compare against the baseline
    py -3 tools/driver_oracle.py --freeze   # rewrite the baseline, deliberately

Needs the engine data under NVDA's configuration folder on this machine
(`%APPDATA%\\nvda`), so every engine family is present; runs here, not in CI.
"""
import hashlib
import json
import os
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
BASELINE = os.path.join(ROOT, "tests", "baseline", "driver-feed.json")
sys.path.insert(0, os.path.join(ROOT, "tests"))
sys.path.insert(0, os.path.join(ROOT, "addon", "synthDrivers", "_outspoken"))
sys.path.insert(0, os.path.join(ROOT, "addon", "synthDrivers"))

VOICES = ["male", "mtk2:Ben", "mtk3:Fred", "gala:Bruce", "cami:Carlos"]


def _config():
    appdata = os.environ.get("APPDATA")
    cfg = os.path.join(appdata, "nvda") if appdata else None
    if not cfg or not os.path.isdir(os.path.join(cfg, "macintalk", "outspoken")):
        return None
    return cfg


def make_driver():
    """The driver under the suite's NVDA stand-ins, on this machine's data."""
    import conftest
    conftest._install_fake_nvda()
    cfg = _config()
    if cfg is None:
        raise SystemExit("no engine data under NVDA's configuration folder")
    sys.modules["globalVars"].appArgs.configPath = cfg
    commands = sys.modules["speech.commands"]
    if not hasattr(commands, "CharacterModeCommand"):
        class CharacterModeCommand(object):
            def __init__(self, state):
                self.state = state
        commands.CharacterModeCommand = CharacterModeCommand
        # As NVDA does: the command also lives on the `speech` module's view.
    import outspoken
    return outspoken.SynthDriver(), commands


def cases(commands):
    C = commands
    return [
        ("text", ["Hello there. You owe 1,234 dollars."]),
        ("adjacent", ["link", "Home", " and ", "more"]),
        ("break", ["Hello", C.BreakCommand(300), "world"]),
        ("pitch", ["Hello ", C.PitchCommand(30), "A", C.PitchCommand(), " world"]),
        ("volume", ["Hello ", C.VolumeCommand(-40), "quietly", C.VolumeCommand(0), " loud"]),
        ("rate", ["Hello ", C.RateCommand(30), "faster"]),
        ("index", ["Hello", C.IndexCommand(1), " world", C.IndexCommand(2)]),
        ("empty", ["", "   "]),
        ("spelling", [C.CharacterModeCommand(True), "h", "e", "l", "l", "o",
                      C.CharacterModeCommand(False)]),
    ]


def render(driver, voice, seq):
    """-> the bytes the driver fed the player for `seq`, or None if absent."""
    import synthDriverHandler
    got = []
    real = driver._player.feed

    def feed(data, *a, **k):
        got.append(bytes(data))
        return real(data, *a, **k)

    driver._player.feed = feed
    try:
        if voice not in driver._get_availableVoices():
            return None
        driver._set_voice(voice)
        driver._set_rate(50); driver._set_pitch(50); driver._set_volume(100)
        driver._set_inflection(50)
        synthDriverHandler.synthDoneSpeaking.arm()
        last = [time.monotonic()]
        n = [0]
        driver.speak(list(seq))
        deadline = time.monotonic() + 60
        done = synthDriverHandler.synthDoneSpeaking.wait(60.0)
        # done fires when the worker drains; the feeder may still be feeding.
        while time.monotonic() < deadline:
            if len(got) != n[0]:
                n[0] = len(got); last[0] = time.monotonic()
            if done and driver._audioQueue.empty() and time.monotonic() - last[0] > 0.15:
                break
            time.sleep(0.02)
    finally:
        driver._player.feed = real
    return b"".join(got)


def capture(driver, commands, verbose=True):
    out = {}
    for voice in VOICES:
        for label, seq in cases(commands):
            pcm = render(driver, voice, seq)
            if pcm is None:
                continue
            out.setdefault(voice, {})[label] = [len(pcm), hashlib.md5(pcm).hexdigest()]
            if verbose:
                print("%-12s %-9s %8d %s" % (voice, label, len(pcm), out[voice][label][1][:12]), flush=True)
    return out


def main():
    freeze = "--freeze" in sys.argv
    driver, commands = make_driver()
    try:
        got = capture(driver, commands)
    finally:
        driver.terminate()
    if freeze:
        os.makedirs(os.path.dirname(BASELINE), exist_ok=True)
        with open(BASELINE, "w", encoding="utf-8") as fh:
            json.dump({"note": "Bytes the 1.2.x driver fed the player, per voice and "
                               "sequence, through the Python engine modules. Rewrite "
                               "only with --freeze, deliberately.",
                       "feeds": got}, fh, indent=1, sort_keys=True)
        print("baseline frozen: %s" % BASELINE)
        return 0
    with open(BASELINE, encoding="utf-8") as fh:
        want = json.load(fh)["feeds"]
    bad = 0
    for voice, labels in want.items():
        for label, val in labels.items():
            now = got.get(voice, {}).get(label)
            if now != val:
                bad += 1
                print("DIFFER  %s %s: frozen %s, now %s" % (voice, label, val, now))
    print("%d cases, %d disagreement(s)" % (sum(len(v) for v in want.values()), bad))
    return bad


if __name__ == "__main__":
    sys.exit(main())
