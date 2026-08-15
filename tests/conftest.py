# -*- coding: utf-8 -*-
"""Enough of NVDA to import and drive the synthesizer outside NVDA.

The driver is the part of this project most likely to be wrong in ways the
engine cannot be blamed for -- two threading races and a stream-teardown
problem all reached Tomi before they reached a test. So the fakes here model
the behaviour those bugs turned on, and nothing else:

* `WavePlayer.idle()` blocks until the audio drains, and `stop()` cuts it
  short. An earlier mock had a non-interruptible `idle()` and reported 355 ms
  of latency for a driver that was already correct.
* `stop()` costs `STREAM_START` on the next `feed()`, because tearing the
  output stream down and starting it again is what made short utterances lag
  while whole sentences were fine.

Tests that need the real engine are skipped when the ROM is absent, since it is
never in the repository.
"""
import os
import sys
import threading
import time
import types

import pytest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ADDON = os.path.join(ROOT, "addon")

#: What a real output stream costs to start after being stopped.
STREAM_START = 0.12


class FakeWavePlayer(object):
    def __init__(self, *a, **k):
        self.fed = 0
        self.bytes = 0
        self.stops = 0
        self.idles = 0
        self.startups = 0
        self._lock = threading.Lock()
        self._until = 0.0
        self._running = False

    def feed(self, data):
        with self._lock:
            self.fed += 1
            self.bytes += len(data)
            now = time.perf_counter()
            if not self._running:
                self._running = True
                self.startups += 1
                now += STREAM_START
            self._until = max(self._until, now) + len(data) / 2.0 / 22254.0

    def stop(self):
        with self._lock:
            self.stops += 1
            self._until = 0.0
            self._running = False

    def idle(self):
        self.idles += 1
        while True:
            with self._lock:
                left = self._until - time.perf_counter()
            if left <= 0:
                with self._lock:
                    self._running = False
                return
            time.sleep(min(left, 0.005))

    def pause(self, switch):
        pass

    def close(self):
        self.stop()


def _stage_rom(cfg_dir):
    """Copy the engine into a fake config dir, if it can be found at all.

    Uses tools/paths.py, so $OUTSPOKEN_ROM works here exactly as it does for
    the command-line tools. Absent, the engine tests skip.
    """
    import shutil
    sys.path.insert(0, os.path.join(ROOT, "tools"))
    try:
        import paths
    except Exception:
        return
    dest = os.path.join(cfg_dir, "outspoken-roms", "macintalk1")
    for name in ("DRVR_1030.bin", "TALK_1001.bin", "RULZ_1129.bin"):
        src = paths.find(name)
        if not src:
            continue
        os.makedirs(dest, exist_ok=True)
        target = os.path.join(dest, name)
        if not os.path.exists(target):
            shutil.copyfile(src, target)


def _install_fake_nvda():
    """Put the modules the driver imports into sys.modules."""
    if "synthDriverHandler" in sys.modules:
        return

    nvwave = types.ModuleType("nvwave")
    nvwave.WavePlayer = FakeWavePlayer
    nvwave.AudioPurpose = type("AudioPurpose", (), {"SPEECH": 1})
    sys.modules["nvwave"] = nvwave

    logh = types.ModuleType("logHandler")
    class _Log(object):
        def __init__(self):
            self.messages = []
        def _rec(self, level, msg, *a, **k):
            self.messages.append((level, msg % a if a else msg))
        def info(self, m, *a, **k): self._rec("info", m, *a)
        def debug(self, m, *a, **k): self._rec("debug", m, *a)
        def warning(self, m, *a, **k): self._rec("warning", m, *a)
        def error(self, m, *a, **k): self._rec("error", m, *a)
    logh.log = _Log()
    sys.modules["logHandler"] = logh

    cfg = types.ModuleType("config")
    cfg.conf = {"audio": {"outputDevice": "default"},
                "speech": {"outputDevice": "default"}}
    sys.modules["config"] = cfg

    # A throwaway NVDA config directory, with the engine staged into it from
    # wherever the developer keeps theirs. That exercises the real lookup --
    # config dir, `outspoken-roms`, recursive -- instead of bypassing it.
    cfg_dir = os.path.join(ROOT, "build", "test-config")
    _stage_rom(cfg_dir)
    gv = types.ModuleType("globalVars")
    gv.appArgs = type("_A", (), {"configPath": cfg_dir, "secure": False})()
    sys.modules["globalVars"] = gv

    speech = types.ModuleType("speech")
    commands = types.ModuleType("speech.commands")
    class IndexCommand(object):
        def __init__(self, index):
            self.index = index
    commands.IndexCommand = IndexCommand
    speech.commands = commands
    sys.modules["speech"] = speech
    sys.modules["speech.commands"] = commands

    sdh = types.ModuleType("synthDriverHandler")
    class _Setting(object):
        def __init__(self, *a, **k): pass
    class VoiceInfo(object):
        def __init__(self, id, name, language=None):
            self.id, self.name, self.language = id, name, language
    class SynthDriver(object):
        VoiceSetting = RateSetting = PitchSetting = VolumeSetting = _Setting
        def __init__(self): pass
    class _Notifier(object):
        def __init__(self): self.count = 0
        def notify(self, **k): self.count += 1
    sdh.SynthDriver = SynthDriver
    sdh.VoiceInfo = VoiceInfo
    sdh.synthDoneSpeaking = _Notifier()
    sdh.synthIndexReached = _Notifier()
    sys.modules["synthDriverHandler"] = sdh


_install_fake_nvda()
for p in (os.path.join(ADDON, "synthDrivers"),
          os.path.join(ADDON, "synthDrivers", "_outspoken"),
          os.path.join(ROOT, "tools")):
    if p not in sys.path:
        sys.path.insert(0, p)


@pytest.fixture(scope="session")
def rom_files():
    """The three engine files, or skip -- they are never in the repository."""
    import rom as rom_mod
    found, missing = rom_mod.find()
    if missing:
        pytest.skip("engine not present (%s); run tools/extract_rom.py"
                    % ", ".join(missing))
    return found


@pytest.fixture(scope="session")
def rules():
    import nrl
    import paths
    p = paths.find("RULZ_1129.bin")
    if not p:
        pytest.skip("RULZ not present; run tools/extract_rom.py")
    return nrl.Rules(open(p, "rb").read())


@pytest.fixture
def driver():
    import outspoken
    d = outspoken.SynthDriver()
    yield d
    d.terminate()
