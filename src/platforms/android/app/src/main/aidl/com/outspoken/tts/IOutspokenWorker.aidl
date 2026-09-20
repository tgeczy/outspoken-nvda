package com.outspoken.tts;

/** The engine, in its own process.  Every call but stop() runs on the
 * worker's one synthesis thread, in order; stop() is the one cross-thread
 * call the host allows, and it makes a render in progress return early. */
interface IOutspokenWorker {
    /** Scan the roots (one per line) in this process.  -> the catalogue,
     * one voice per line, tab-separated fields; empty when nothing speaks. */
    String scan(String roots);
    /** Open or switch to the voice with this catalogue id.  0, or negative. */
    int useVoice(String roots, String id);
    /** The driver's own 0-100 scales; numbers 0 off, 1 words, 2 digits;
     * ratePercent is the requesting app's speech rate, 100 being normal. */
    void settings(int rate, int pitch, int inflection, int volume, int numbers, int ratePercent);
    /** Begin an utterance from UTF-8 text.  0 when there is audio to pull,
     * 1 when there was nothing to say, negative on failure. */
    int start(in byte[] utf8);
    /** The next piece as 16-bit little-endian PCM, empty when the utterance
     * is over, null on failure. */
    byte[] pull(int capacity);
    /** Abandon the utterance being pulled; the next pull answers nothing. */
    void cancel();
    /** From any thread: make a render in progress return early. */
    void stop();
    void shutdown();
}
