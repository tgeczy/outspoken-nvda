// The framework fires ACTION_CHECK_TTS_DATA to ask whether the engine's voice
// data is usable. Ours is usable exactly when the user has run "Check Engine"
// and something speaks -- the gate that keeps the engine from being
// selectable on an empty install.
package com.outspoken.tts

import android.app.Activity
import android.content.Intent
import android.os.Bundle
import android.speech.tts.TextToSpeech

class CheckVoiceDataActivity : Activity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        // CHECK_TTS_DATA uses ISO-3 locales, not the IDs returned by getVoices().
        val pass = OutspokenEngine.verified(this)
        val voices = ArrayList<String>()
        if (pass) {
            voices.add("eng-USA")
            if (OutspokenEngine.spanishVoices(this).isNotEmpty()) voices.add("spa-MEX")
        }
        val data = Intent().apply {
            putStringArrayListExtra(TextToSpeech.Engine.EXTRA_AVAILABLE_VOICES, voices)
            putStringArrayListExtra(
                TextToSpeech.Engine.EXTRA_UNAVAILABLE_VOICES,
                if (pass) arrayListOf() else arrayListOf("eng-USA"))
        }
        setResult(
            if (pass) TextToSpeech.Engine.CHECK_VOICE_DATA_PASS
            else TextToSpeech.Engine.CHECK_VOICE_DATA_FAIL, data)
        finish()
    }
}
