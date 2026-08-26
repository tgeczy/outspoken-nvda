# -*- coding: utf-8 -*-
"""The outSPOKEN engines behind a pipe: the NVDA driver, unmodified, serving.

The SAPI engine DLL launches this under the embeddable Python it installs
beside it, and speaks the same framed protocol the Panthera engine speaks to
its host.  Everything that decides how speech sounds -- NRL rules, MacinTalk
2 command building, number reading, the 8-to-16 widening with volume folded
in -- runs in the same modules NVDA users run, byte for byte.  That is the
whole design: there is no port to drift, because there is no port.

Requests arrive on stdin, framed:

    'OSP4' | rate | pitch | volume | namelen | textlen | name | text

rate/pitch/volume are the driver's own 0-100 integers.  The response is

    'OSPR' | status

followed by PCM in chunks as the engine produces them -- u32 frame count,
then frames*2 bytes of 16-bit mono at 22254 Hz -- and a zero frame count to
finish.  status != 0 means the utterance failed and no audio follows.

`--list` prints one voice per line, "id<TAB>name", for token registration.
"""
import os
import struct
import sys
import threading
import types

HERE = os.path.dirname(os.path.abspath(__file__))
RATE = 22254

REQ = 0x4F535034            # 'OSP4'
RSP = 0x4F535052            # 'OSPR'


def _install_fakes(data_root, player_cls):
    """Enough of NVDA to import and drive the driver outside NVDA.

    The same trick the test suite uses, trimmed to what the import chain
    needs and pointed at the SAPI data root instead of a test directory.
    """
    nvwave = types.ModuleType("nvwave")
    nvwave.WavePlayer = player_cls
    nvwave.AudioPurpose = type("AudioPurpose", (), {"SPEECH": 1})
    sys.modules["nvwave"] = nvwave

    logh = types.ModuleType("logHandler")

    class _Log(object):
        DEBUG = 10

        def _drop(self, *a, **k):
            pass
        info = debug = warning = error = _drop

        def isEnabledFor(self, level):
            return False
    logh.log = _Log()
    sys.modules["logHandler"] = logh

    cfg = types.ModuleType("config")
    cfg.conf = {"audio": {"outputDevice": "default"},
                "speech": {"outputDevice": "default"}}
    sys.modules["config"] = cfg

    gv = types.ModuleType("globalVars")
    gv.appArgs = type("_A", (), {"configPath": data_root, "secure": False})()
    sys.modules["globalVars"] = gv

    speech = types.ModuleType("speech")
    commands = types.ModuleType("speech.commands")
    for name in ("IndexCommand", "BreakCommand", "PitchCommand",
                 "VolumeCommand", "RateCommand"):
        cls = type(name, (), {"__init__":
                              lambda self, value=0: setattr(self, "value",
                                                            value)})
        setattr(commands, name, cls)
    speech.commands = commands
    sys.modules["speech"] = speech
    sys.modules["speech.commands"] = commands

    sdh = types.ModuleType("synthDriverHandler")

    class _Setting(object):
        def __init__(self, *a, **k):
            pass

    class VoiceInfo(object):
        def __init__(self, id, name, language=None):
            self.id, self.name, self.language = id, name, language

    class SynthDriver(object):
        VoiceSetting = RateSetting = PitchSetting = _Setting
        VolumeSetting = InflectionSetting = _Setting

        def __init__(self):
            pass

    class _Notifier(object):
        def __init__(self):
            self._event = threading.Event()

        def notify(self, **k):
            self._event.set()

        def wait(self, timeout):
            return self._event.wait(timeout)

        def arm(self):
            self._event.clear()
    sdh.SynthDriver = SynthDriver
    sdh.VoiceInfo = VoiceInfo
    sdh.synthDoneSpeaking = _Notifier()
    sdh.synthIndexReached = _Notifier()
    sys.modules["synthDriverHandler"] = sdh

    asu = types.ModuleType("autoSettingsUtils")
    ds = types.ModuleType("autoSettingsUtils.driverSetting")
    ds.DriverSetting = _Setting
    ds.BooleanDriverSetting = _Setting
    ds.NumericDriverSetting = _Setting
    asu.driverSetting = ds
    sys.modules["autoSettingsUtils"] = asu
    sys.modules["autoSettingsUtils.driverSetting"] = ds

    import builtins
    if not hasattr(builtins, "_"):
        builtins._ = lambda s: s

    #: Installed, the driver package sits beside this script; in the
    #: repository it is one level up under addon/.
    base = HERE if os.path.isdir(os.path.join(HERE, "synthDrivers")) \
        else os.path.join(os.path.dirname(HERE), "addon")
    for p in (os.path.join(base, "synthDrivers"),
              os.path.join(base, "synthDrivers", "_outspoken")):
        if p not in sys.path:
            sys.path.insert(0, p)


class _StreamPlayer(object):
    """A WavePlayer whose feed() is the wire.

    The driver hands over 16-bit PCM as each engine buffer completes, so
    forwarding from feed() streams the utterance -- first sound reaches
    SAPI while the rest is still rendering, the same benefit the Panthera
    host's streaming protocol has.
    """
    out = None

    def __init__(self, *a, **k):
        pass

    def feed(self, data):
        if _StreamPlayer.out is not None and data:
            _StreamPlayer.out.write(struct.pack("<I", len(data) // 2) + data)
            _StreamPlayer.out.flush()

    def stop(self):
        pass

    def idle(self):
        pass

    def pause(self, switch):
        pass

    def close(self):
        pass


def _exact(stream, n):
    buf = b""
    while len(buf) < n:
        chunk = stream.read(n - len(buf))
        if not chunk:
            return None
        buf += chunk
    return buf


def main():
    args = sys.argv[1:]
    listing = "--list" in args
    if listing:
        args.remove("--list")
    data_root = args[0] if args else os.path.join(
        os.environ.get("APPDATA", ""), "nvda")

    _install_fakes(data_root, _StreamPlayer)
    import synthDriverHandler
    import outspoken

    driver = outspoken.SynthDriver()
    try:
        if listing:
            for vid, info in driver._get_availableVoices().items():
                print("%s\t%s" % (vid, info.name))
            return 0

        stdin = sys.stdin.buffer
        stdout = sys.stdout.buffer
        while True:
            head = _exact(stdin, 24)
            if head is None:
                return 0
            magic, rate, pitch, volume, nv, nt = struct.unpack("<IiiiII",
                                                               head)
            if magic != REQ:
                return 2
            name = _exact(stdin, nv)
            text = _exact(stdin, nt)
            if name is None or text is None:
                return 0
            status = 0
            try:
                voice = name.decode("utf-8")
                if voice and driver._get_voice() != voice:
                    driver._set_voice(voice)
                driver._set_rate(max(0, min(100, rate)))
                driver._set_pitch(max(0, min(100, pitch)))
                driver._set_volume(max(0, min(100, volume)))
            except Exception:
                status = 1
            stdout.write(struct.pack("<Ii", RSP, status))
            stdout.flush()
            if status:
                continue
            #: PCM flows from feed() while the driver renders; the done
            #: notification says the last buffer has been fed.
            synthDriverHandler.synthDoneSpeaking.arm()
            _StreamPlayer.out = stdout
            try:
                driver.speak([text.decode("utf-8", "replace")])
                if not synthDriverHandler.synthDoneSpeaking.wait(120.0):
                    driver.cancel()
            finally:
                _StreamPlayer.out = None
            stdout.write(struct.pack("<I", 0))
            stdout.flush()
    finally:
        driver.terminate()


if __name__ == "__main__":
    sys.exit(main())
