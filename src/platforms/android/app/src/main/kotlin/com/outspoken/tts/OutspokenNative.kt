// Raw JNI binding to liboutspoken.so -- the MacinTalk host, the same C that
// NVDA loads as a DLL and the SAPI engine runs as a program.  Not thread-safe:
// the one process-global engine is driven by one caller at a time.  Only
// nativeStop may be called concurrently; it sets a flag and enters no guest
// code.
package com.outspoken.tts

object OutspokenNative {
    init {
        System.loadLibrary("outspoken")
    }

    /** Scan the search roots, one per line, and answer the catalogue: one
     * voice per line, the fields tab-separated in the order id, label, kind,
     * creator, voice id, name, language, gender, folder.  A plain folder
     * walk -- safe in any process, before any engine is open.  Null when the
     * host refused. */
    external fun nativeScan(roots: String): String?

    /** What the last scan skipped, one "folder<TAB>why" per line. */
    external fun nativeSkipped(): String

    /** Open the engine this voice belongs to with the voice selected, or
     * switch voice inside the open engine when that is all it takes.  Scans
     * `roots` first, because the manifest comes from this process's own
     * catalogue.  0, or negative with the reason in [nativeError]. */
    external fun nativeUseVoice(roots: String, id: String): Int

    external fun nativeError(): String

    /** The driver's own 0-100 scales for rate, pitch, inflection and volume;
     * numbers 0 off, 1 words, 2 digits.  `ratePercent` is the requesting
     * app's speech rate, 100 being normal, applied on top of the slider. */
    external fun nativeSettings(rate: Int, pitch: Int, inflection: Int, volume: Int,
                                numbers: Int, ratePercent: Int)

    /** Begin an utterance.  UTF-8 in: the host folds it to MacRoman exactly
     * as the SAPI engine does.  0 when there is audio, 1 when the text had
     * nothing to say, negative on failure. */
    external fun nativeStart(utf8: ByteArray): Int

    /** Fill `out` with up to out.size 16-bit samples; -> the count, 0 when
     * the utterance is over, negative on failure.  Each call renders one
     * round of the engine on this thread. */
    external fun nativePull(out: ShortArray): Int

    /** Abandon the utterance being pulled.  Synthesis thread. */
    external fun nativeCancel()

    /** Make a render in progress return early.  Any thread. */
    external fun nativeStop()

    external fun nativeClose()

    /** The rate every engine renders at, in Hz. */
    external fun nativeSampleRate(): Int
}
