// What the engine is handed, on this platform.  The desktop driver gets words
// from NVDA; TalkBack hands over the code points themselves, so emoji are
// described here, before the text crosses to the host.  Everything after that
// -- MacRoman, numbers in English and Spanish, the 1984 rules, punctuation --
// is the host's, the same as on every other platform.
package com.outspoken.tts

object OutspokenText {

    /** The utterance as the engine should hear it.  One piece: the host
     * streams the audio as the engine renders it and a cancel lands inside
     * the render, so nothing here needs cutting to be interruptible, and an
     * utterance kept whole keeps its sentence prosody. */
    fun pieces(text: String): List<String> = listOf(Emoji.describe(text, true))

    /** One piece, as the host takes it: UTF-8, folded to MacRoman there. */
    fun bytes(piece: String): ByteArray = piece.toByteArray(Charsets.UTF_8)
}
