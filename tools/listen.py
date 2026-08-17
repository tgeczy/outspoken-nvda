# -*- coding: utf-8 -*-
"""Say whether a rendered WAV is speech, noise, or silence -- without ears.

"It made audio" is not the same claim as "it speaks", and this project has
already been fooled once: a broken render of a 2.23 s sentence came back in
0.18 s and was mistaken for a second voice.  So before anybody says an engine
works, run this.

Speech has a shape.  It is not constant -- an envelope that never changes is a
tone or hiss, not words -- and it is not sparse: a handful of live samples in
a sea of silence is a click.  The three numbers below separate those cases,
and the thresholds are deliberately loose, because the job here is to catch
"this is obviously not speech", not to grade a voice.

    py -3 tools/listen.py build/pro-spoken.wav
"""
import struct
import sys
import wave


def analyse(path):
    with wave.open(path, "rb") as w:
        channels, width, rate, frames = (w.getnchannels(), w.getsampwidth(),
                                         w.getframerate(), w.getnframes())
        raw = w.readframes(frames)
    if width == 1:                       # 8-bit is unsigned, centred on 128
        samples = [b - 128 for b in raw]
    else:
        n = len(raw) // 2
        samples = list(struct.unpack("<%dh" % n, raw[:n * 2]))
    return channels, width, rate, samples


def envelope(samples, rate, ms=20):
    """Mean absolute amplitude per 20 ms window -- the shape of the speech."""
    step = max(1, int(rate * ms / 1000))
    out = []
    for i in range(0, len(samples) - step, step):
        window = samples[i:i + step]
        out.append(sum(abs(s) for s in window) / float(step))
    return out


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 1
    path = sys.argv[1]
    channels, width, rate, samples = analyse(path)
    if not samples:
        print("%s is empty" % path)
        return 1

    peak = max(abs(s) for s in samples)
    live = sum(1 for s in samples if abs(s) > peak * 0.02)
    env = envelope(samples, rate)
    quiet = sum(1 for e in env if e < peak * 0.02)
    loud = sum(1 for e in env if e > peak * 0.20)

    print("%s" % path)
    print("  %d channel(s), %d-bit, %d Hz, %.2f s"
          % (channels, width * 8, rate, len(samples) / float(rate)))
    print("  peak %d, %d of %d samples live (%.1f%%)"
          % (peak, live, len(samples), 100.0 * live / len(samples)))
    print("  envelope: %d windows, %d quiet, %d loud"
          % (len(env), quiet, loud))

    # Silence, a click, a flat tone, or speech.
    if peak < 4:
        print("\n  SILENCE -- nothing was rendered")
        return 1
    if live < len(samples) * 0.05:
        print("\n  A CLICK -- almost every sample is silent")
        return 1
    if not env:
        print("\n  TOO SHORT to judge")
        return 1
    if quiet == 0 and loud == len(env):
        print("\n  FLAT -- constant amplitude, so a tone or hiss, not words")
        return 1
    if loud == 0:
        print("\n  NOTHING LOUD -- there is signal but nothing that carries")
        return 1
    print("\n  SPEECH-SHAPED: it starts, stops, and varies. Worth listening to.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
