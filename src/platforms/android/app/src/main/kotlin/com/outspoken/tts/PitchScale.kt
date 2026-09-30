package com.outspoken.tts

/**
 * Android's pitch ratio onto the driver's own 0-100 pitch scale.
 *
 * A screen reader marks a capital letter by raising the pitch for one
 * utterance and dropping it again for the next. On Android that arrives as
 * `SynthesisRequest.getPitch()`, a **ratio times 100** where 100 is the voice
 * as recorded. Everything below this speaks in slider units instead, because
 * `osp_apply_settings` adds the offset to the slider and converts afterwards --
 * the same `padj` the NVDA driver sends as a `PitchCommand`, so one quantity
 * crosses every front end.
 *
 * **The two scales are a logarithm apart, not proportional.** One slider unit
 * is `12 * 10 / 50 = 2.4` tenths of a semitone, and Apple's pitch is musical:
 * twelve units to the octave. So a ratio becomes `50 * log2(r / 100)` slider
 * units. Treating 150 as "fifty units up" would put a capital letter octaves
 * above the sentence it sits in, which is the mistake this file exists to make
 * impossible to repeat.
 *
 * Note this is **not** the number the Mac OS X sibling passes across the same
 * boundary; there it is tenths of a semitone, because that host applies the
 * offset itself. They meet at the engine: 150 lands on 70 tenths in both.
 *
 * Nothing here knows about Android, so the desktop JVM can test it.
 */
internal object PitchScale {
    /** One slider unit, in tenths of a semitone -- `_PITCH_SEMITONES * 10 / 50`
     * from the NVDA driver, where 50 is the middle of the 0-100 scale. */
    const val TENTHS_PER_UNIT = 2.4

    /**
     * -> the offset to add to the pitch slider, in slider units.
     *
     * 100 answers 0 exactly, so a caller that leaves the pitch alone -- most of
     * them, and all of them before this existed -- is untouched. Zero, negative
     * and nonsensical values answer 0 rather than throwing: this runs on every
     * utterance, and a bad number is not worth failing an announcement over.
     *
     * The result is not clamped to what the slider can hold, only to the scale:
     * `set_pitch_tenths` clamps the slider and the offset together, so a user
     * already at 90 asked for another 29 saturates at the top instead of
     * overshooting -- the same thing the NVDA driver does with the same numbers.
     */
    fun sliderOffset(requestedRatio: Int): Int {
        if (requestedRatio <= 0 || requestedRatio == 100) return 0
        val units = 50.0 * (Math.log(requestedRatio / 100.0) / Math.log(2.0))
        if (!units.isFinite()) return 0
        return Math.round(units).toInt().coerceIn(-100, 100)
    }

    /** What `sliderOffset` will be worth once the host has converted it, in
     * tenths of a semitone from the voice's own pitch, for a given slider
     * position. Mirrors `set_pitch_tenths` in osp_settings.c, and exists so a
     * test can state the semitones a capital letter is raised by rather than
     * only the slider units. */
    fun tenths(slider: Int, offset: Int): Int {
        val p = (slider + offset).coerceIn(0, 100)
        return Math.round((p - 50) * TENTHS_PER_UNIT).toInt()
    }
}
