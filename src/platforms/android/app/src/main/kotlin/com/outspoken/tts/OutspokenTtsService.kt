// The public TTS service.  A voice selects an engine family; every request
// streams from the one worker process, which holds whichever engine the
// voice belongs to.  A stop reaches the render in progress.
package com.outspoken.tts

import android.media.AudioFormat
import android.speech.tts.SynthesisCallback
import android.speech.tts.SynthesisRequest
import android.speech.tts.TextToSpeech
import android.speech.tts.TextToSpeechService
import android.speech.tts.Voice
import android.util.Log

class OutspokenTtsService : TextToSpeechService() {

    @Volatile private var stopRequested = false

    override fun onCreate() {
        super.onCreate()
        // Warm the engine when the service binds, so the first utterance is
        // not gated on the worker process starting.
        Thread { try { OutspokenEngine.warmUp(applicationContext) } catch (e: Throwable) {} }.start()
        // Data copied in by hand goes into protected storage as soon as the
        // phone is unlocked.  On a phone that has just booted, that is later
        // than now.
        try { OutspokenEngine.migrateWhenUnlocked(applicationContext) } catch (e: Throwable) {}
    }

    private fun spanish(): Boolean = OutspokenEngine.spanishVoices(this).isNotEmpty()

    /** English for every voice; Mexican Spanish when Carlos and Catalina are
     * here.  Country granularity where it is true. */
    private fun langAvailability(lang: String?, country: String?): Int = when (lang) {
        "eng", "en" -> if (country == "USA" || country == "US") TextToSpeech.LANG_COUNTRY_AVAILABLE
                       else TextToSpeech.LANG_AVAILABLE
        "spa", "es" -> if (!spanish()) TextToSpeech.LANG_NOT_SUPPORTED
                       else if (country == "MEX" || country == "MX") TextToSpeech.LANG_COUNTRY_AVAILABLE
                       else TextToSpeech.LANG_AVAILABLE
        else -> TextToSpeech.LANG_NOT_SUPPORTED
    }

    override fun onIsLanguageAvailable(lang: String?, country: String?, variant: String?): Int {
        // The gate: until the user has run Check Engine and the data is present,
        // the engine has no usable voices, so it reports its language missing.
        if (!OutspokenEngine.verified(this)) return TextToSpeech.LANG_MISSING_DATA
        return langAvailability(lang, country)
    }

    override fun onGetLanguage(): Array<String> = arrayOf("eng", "USA", "")

    override fun onLoadLanguage(lang: String?, country: String?, variant: String?): Int =
        onIsLanguageAvailable(lang, country, variant)

    override fun onGetVoices(): MutableList<Voice> {
        if (!OutspokenEngine.verified(this)) return mutableListOf()
        return OutspokenEngine.allVoices(this).map { v ->
            Voice(v.id, v.locale, Voice.QUALITY_NORMAL, Voice.LATENCY_NORMAL, false, emptySet())
        }.toMutableList()
    }

    override fun onIsValidVoiceName(name: String?): Int =
        if (OutspokenEngine.verified(this) && OutspokenEngine.voiceById(this, name) != null) TextToSpeech.SUCCESS
        else TextToSpeech.ERROR

    override fun onLoadVoice(name: String?): Int = onIsValidVoiceName(name)

    private fun isSpanish(lang: String?) = lang == "spa" || lang == "es"

    override fun onGetDefaultVoiceNameFor(lang: String?, country: String?, variant: String?): String? {
        if (!OutspokenEngine.verified(this)) return null
        if (lang != null && langAvailability(lang, country) < 0) return null
        // Spanish asked for and present: the Spanish family's own choice.
        if (isSpanish(lang)) {
            OutspokenEngine.defaultVoice(this, OutspokenEngine.FAM_CAMI)?.let { return it.id }
        }
        // The default belongs to the chosen voice's family, not to the whole
        // list, so a saved "Fred" never quietly switches engines.
        return OutspokenEngine.defaultVoice(this)?.id
    }

    override fun onStop() {
        stopRequested = true
        OutspokenEngine.stop()
    }

    override fun onSynthesizeText(request: SynthesisRequest, callback: SynthesisCallback) {
        stopRequested = false
        OutspokenEngine.withSynthesis {
            if (!stopRequested) synthesize(request, callback)
        }
    }

    private fun synthesize(request: SynthesisRequest, callback: SynthesisCallback) {
        // Say that this thread carries speech, for the scheduler's sake on a
        // watch with unequal cores.
        android.os.Process.setThreadPriority(android.os.Process.THREAD_PRIORITY_AUDIO)

        val text = request.charSequenceText?.toString() ?: ""
        val latencyProbe = request.params.getBoolean("com.outspoken.tts.latency_probe", false)
        var probeMarked = false
        Log.i("OutspokenTts", "synth: voice=${request.voiceName} lang=${request.language} " +
            "rate=${request.speechRate} verified=${OutspokenEngine.verified(this)} textLen=${text.length}")

        if (!OutspokenEngine.verified(this)) {
            Log.w("OutspokenTts", "not verified -> error"); callback.error(TextToSpeech.ERROR_SERVICE); return
        }

        // The voice chosen in settings wins unless the user says otherwise --
        // except that a request in Spanish gets a Spanish voice when there
        // is one, whatever the setting, because "Home" in Carlos's accent is
        // what a Spanish-speaking screen reader user asked for.
        val requested = OutspokenEngine.voiceById(this, request.voiceName)
        val override = OutspokenEngine.prefs(this).getBoolean("override_voice", true)
        val voice = when {
            isSpanish(request.language) && spanish() ->
                requested?.takeIf { it.language == "es" }
                    ?: OutspokenEngine.defaultVoice(this, OutspokenEngine.FAM_CAMI)
            override -> OutspokenEngine.defaultVoice(this)
            else -> requested
        } ?: OutspokenEngine.defaultVoice(this)
            ?: run { Log.w("OutspokenTts", "no voice for '${request.voiceName}' -> error")
                     callback.error(TextToSpeech.ERROR_SERVICE); return }

        val snapshot = OutspokenEngine.settings(this, voice.family)
        val rate = OutspokenEngine.sampleRate()
        if (callback.start(rate, AudioFormat.ENCODING_PCM_16BIT, 1) != TextToSpeech.SUCCESS) {
            Log.w("OutspokenTts", "callback.start refused"); return
        }
        if (text.isBlank()) { callback.done(); return }

        val sampleCapacity = minOf(4096, callback.maxBufferSize / 2)
        if (sampleCapacity <= 0) {
            OutspokenEngine.stop()
            callback.error(TextToSpeech.ERROR_OUTPUT)
            return
        }
        val samples = ShortArray(sampleCapacity)
        val bytes = ByteArray(samples.size * 2)
        var total = 0
        val ratePercent = snapshot.ratePercent(request.speechRate)

        val pieces = OutspokenText.pieces(text)
        for (piece in pieces) {
            if (stopRequested) break
            val started = OutspokenEngine.speakStart(this, voice, OutspokenText.bytes(piece), ratePercent, snapshot)
            if (started == 1) continue                  // nothing to say in it
            if (started != 0) {
                if (stopRequested) return
                Log.w("OutspokenTts", "speakStart -> $started")
                // Anything already spoken is real audio the user heard; only a
                // failure on the very first piece is a failed utterance.
                if (total == 0) { callback.error(TextToSpeech.ERROR_SYNTHESIS); return }
                break
            }
            if (stopRequested) OutspokenEngine.stop()
            while (!stopRequested) {
                val n = OutspokenEngine.pull(samples)
                if (n < 0) {
                    if (stopRequested) return
                    Log.e("OutspokenTts", "pull failed: $n")
                    OutspokenEngine.stop()
                    callback.error(TextToSpeech.ERROR_SYNTHESIS)
                    return
                }
                if (n == 0) break
                if (latencyProbe && !probeMarked) {
                    val audible = (0 until n).firstOrNull { kotlin.math.abs(samples[it].toInt()) > 128 }
                    if (audible != null) {
                        callback.rangeStart(maxOf(1, total + audible), 0, minOf(1, text.length))
                        probeMarked = true
                    }
                }
                var bi = 0
                for (i in 0 until n) {
                    val s = samples[i].toInt()
                    bytes[bi++] = (s and 0xff).toByte()
                    bytes[bi++] = ((s shr 8) and 0xff).toByte()
                }
                if (callback.audioAvailable(bytes, 0, n * 2) != TextToSpeech.SUCCESS) {
                    Log.i("OutspokenTts", "audio callback stopped after $total samples")
                    OutspokenEngine.stop()
                    return
                }
                total += n
            }
        }
        Log.i("OutspokenTts", "synth ${voice.hostId} streamed $total samples, stop=$stopRequested")
        callback.done()
    }
}
