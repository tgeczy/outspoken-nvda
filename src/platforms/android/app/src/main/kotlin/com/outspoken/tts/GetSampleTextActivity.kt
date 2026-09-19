// ACTION_GET_SAMPLE_TEXT: the phrase the system speaks when the user previews
// the engine in Settings.  In the language asked for, when it is Spanish and
// the Spanish engine is here.
package com.outspoken.tts

import android.app.Activity
import android.content.Intent
import android.os.Bundle
import android.speech.tts.TextToSpeech

class GetSampleTextActivity : Activity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        val lang = intent?.getStringExtra("language")
        val spanish = (lang == "spa" || lang == "es") && OutspokenEngine.spanishVoices(this).isNotEmpty()
        val data = Intent().putExtra(
            TextToSpeech.Engine.EXTRA_SAMPLE_TEXT,
            if (spanish) "Hola. Esta es una voz de Macintosh, hablando en tu teléfono."
            else "Hello there. This is a Macintosh voice, speaking on your phone.")
        setResult(TextToSpeech.LANG_AVAILABLE, data)
        finish()
    }
}
