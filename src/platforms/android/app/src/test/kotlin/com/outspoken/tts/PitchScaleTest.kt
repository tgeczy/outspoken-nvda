package com.outspoken.tts

import org.junit.Assert.assertEquals
import org.junit.Test

/** Android's pitch ratio onto the slider, and the semitones it is worth.
 *
 * The numbers here are arithmetic, not measurements: what makes them worth
 * pinning is that the two scales are a logarithm apart, so a plausible-looking
 * linear conversion would be wrong by octaves at exactly the moment a screen
 * reader raises a capital letter.
 */
class PitchScaleTest {

    /** The case the whole thing exists for. A screen reader asking for half
     * again the pitch means seven semitones up, not fifty slider units. */
    @Test fun aRaisedCapitalIsSevenSemitones() {
        val offset = PitchScale.sliderOffset(150)
        assertEquals(29, offset)
        assertEquals(70, PitchScale.tenths(50, offset))     // 7.0 semitones
    }

    /** And back down again, which is the next utterance in that same sequence. */
    @Test fun lowerThanNormalGoesDown() {
        val offset = PitchScale.sliderOffset(75)
        assertEquals(-21, offset)
        assertEquals(-50, PitchScale.tenths(50, offset))    // -5.0 semitones
    }

    /** Normal is exactly nothing, so every caller that never touches pitch --
     * which is all of them before this existed -- is unaffected. */
    @Test fun normalPitchChangesNothing() {
        assertEquals(0, PitchScale.sliderOffset(100))
        assertEquals(0, PitchScale.tenths(50, 0))
        assertEquals(0, PitchScale.tenths(90, 0).let { PitchScale.sliderOffset(100) })
    }

    /** Doubling and halving are an octave, which is the span the driver keeps:
     * twelve semitones either way and no further. */
    @Test fun doublingIsAnOctave() {
        assertEquals(120, PitchScale.tenths(50, PitchScale.sliderOffset(200)))
        assertEquals(-120, PitchScale.tenths(50, PitchScale.sliderOffset(50)))
    }

    /** The scale is logarithmic: the same ratio is the same number of
     * semitones wherever it starts, which is what makes one setting mean one
     * thing across voices that sit an octave apart. */
    @Test fun equalRatiosAreEqualIntervals() {
        val oneOctave = PitchScale.sliderOffset(200)
        val twoOctaves = PitchScale.sliderOffset(400)
        assertEquals(oneOctave * 2, twoOctaves)
    }

    /** Nonsense answers nothing rather than throwing: this runs on every
     * utterance, and a bad number is not worth failing an announcement over. */
    @Test fun nonsenseIsIgnored() {
        assertEquals(0, PitchScale.sliderOffset(0))
        assertEquals(0, PitchScale.sliderOffset(-100))
        assertEquals(0, PitchScale.sliderOffset(Int.MIN_VALUE))
    }

    /** An extreme ratio saturates instead of running away, and the host clamps
     * the slider and the offset together -- so a user already near the top who
     * is asked for more gets the top, not an out-of-range request. */
    @Test fun extremesSaturate() {
        assertEquals(100, PitchScale.sliderOffset(Int.MAX_VALUE))
        assertEquals(120, PitchScale.tenths(90, PitchScale.sliderOffset(150)))
        assertEquals(-120, PitchScale.tenths(10, PitchScale.sliderOffset(75)))
    }

    /** One slider unit is 2.4 tenths of a semitone, which is the constant the
     * NVDA driver derives from twelve semitones over half the scale. Stated
     * here because every number above depends on it. */
    @Test fun oneUnitIsTwoPointFourTenths() {
        assertEquals(2.4, PitchScale.TENTHS_PER_UNIT, 1e-9)
        assertEquals(12.0 * 10 / 50, PitchScale.TENTHS_PER_UNIT, 1e-9)
    }
}
