package com.outspoken.tts

import android.app.Activity
import android.app.Instrumentation
import android.os.Bundle
import android.speech.tts.TextToSpeech
import android.speech.tts.UtteranceProgressListener
import android.util.Log
import java.io.File
import java.security.MessageDigest
import java.util.Locale
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit

/**
 * Device check: the pitch a caller asks for reaches the engine and changes the
 * audio. Requires engine data on the device.
 *
 *   adb shell am instrument -w -e report true \
 *       com.outspoken.tts.test/com.outspoken.tts.PitchCheck
 *   adb shell am instrument -w -e pitch true [-e voice Fred] \
 *       com.outspoken.tts.test/com.outspoken.tts.PitchCheck
 *
 * **Through the platform client and `setPitch`, deliberately.** The bug this
 * was written for was not that the engines cannot change pitch -- all four can,
 * and the NVDA driver has raised capitals with the same offset since f0d6208 --
 * but that the Android layer passed a hardcoded zero, so every request rendered
 * at the voice's own pitch however loudly the caller asked otherwise. A check
 * that called the native setter directly would have passed against that build.
 *
 * A screen reader marking a capital letter is exactly this: one request raised,
 * the next back at 1.0. So the last leg checks that coming back down returns the
 * *original bytes* -- if an offset leaked into the engine and stayed there,
 * pitch would climb for the rest of the session and every render after the
 * capital would be wrong.
 */
class PitchCheck : Instrumentation() {
    private var pitch = false
    private var report = false
    private var voiceName: String? = null
    private var removeUnit: String? = null
    private var restoreAfter: (() -> Unit)? = null

    override fun onCreate(arguments: Bundle?) {
        pitch = arguments?.getString("pitch") == "true"
        report = arguments?.getString("report") == "true"
        voiceName = arguments?.getString("voice")
        removeUnit = arguments?.getString("removeUnit")
        super.onCreate(arguments); start()
    }

    override fun onStart() {
        if (report) { reportState(); return }
        if (pitch) { checkPitch(); return }
        removeUnit?.let { checkRemoval(it); return }
        val results = Bundle()
        results.putString("usage", "pass -e pitch true, or -e report true")
        finish(Activity.RESULT_CANCELED, results)
    }

    /** Read-only: what is on this device, which the pitch check needs to exist. */
    private fun reportState() {
        val results = Bundle()
        try {
            val voices = OutspokenEngine.allVoices(targetContext)
            results.putString("verified", OutspokenEngine.verified(targetContext).toString())
            results.putString("family", OutspokenEngine.activeFamily(targetContext))
            results.putString("voices", voices.size.toString())
            results.putString("families", voices.map { it.family }.distinct().toString())
            // One voice per family, so a pitch run can be pointed at each of them:
            // 1984 moves its pitch in hertz and the Speech Manager engines in
            // 'pbas', and only a run per family says both paths work.
            for ((family, group) in voices.groupBy { it.family })
                results.putString("family_$family", group.joinToString(" ") { it.name })
            results.putString("default", OutspokenEngine.defaultVoice(targetContext)?.id ?: "none")
            val units = OutspokenEngine.installedUnits(targetContext)
            results.putString("units", units.toString())
            results.putString("dataRoot", OutspokenEngine.dataRoot(targetContext).path)
            val inbox = OutspokenEngine.inboxRoot(targetContext)
            results.putString("inbox", inbox?.path ?: "unavailable")
            results.putString("inboxHolds",
                inbox?.listFiles()?.joinToString { it.name } ?: "unreadable")
            results.putString("roots", OutspokenEngine.rootsArgument(targetContext).replace("\n", " | "))
            for (unit in units)
                results.putString("unit_$unit",
                    OutspokenEngine.removableSize(targetContext, listOf(unit)).toString() + " bytes")
            // The arithmetic the fix turns on, reported so a device run states it
            // rather than leaving it to be recomputed by hand.
            for (ratio in listOf(150, 100, 75)) {
                val offset = PitchScale.sliderOffset(ratio)
                results.putString("ratio$ratio", "offset=$offset tenths=${PitchScale.tenths(50, offset)}")
            }
            finish(Activity.RESULT_OK, results)
        } catch (e: Throwable) {
            results.putString("failure", Log.getStackTraceString(e))
            finish(Activity.RESULT_CANCELED, results)
        }
    }

    /** Remove one data unit and report what the app believes afterwards.
     *
     * **Destructive, and named explicitly for that reason**: `-e removeUnit
     * macintalk2` deletes that engine's files on the device it runs on. It takes
     * a unit rather than defaulting to one, and refuses one that is not there
     * instead of reporting a cheerful zero.
     *
     * `foldersLeft` is the point of the report: a copy surviving in the inbox is
     * one migrate away from undoing the removal, and this is where that shows.
     */
    private fun checkRemoval(unit: String) {
        val results = Bundle()
        try {
            check(unit in ZipImport.UNITS) { "not a data unit: $unit" }
            val before = OutspokenEngine.installedUnits(targetContext)
            check(unit in before) { "$unit is not installed; installed: $before" }
            results.putString("unitsBefore", before.toString())
            results.putString("sizeBefore",
                OutspokenEngine.removableSize(targetContext, listOf(unit)).toString())
            // Speak first, so the removal has a live worker holding the engine
            // mapped to retire rather than the easy case of one never used.
            results.putString("warmed", warmWorker())
            val freed = OutspokenEngine.removeUnits(targetContext, listOf(unit))
            results.putString("freed", freed.toString())
            val after = OutspokenEngine.installedUnits(targetContext)
            results.putString("unitsAfter", after.toString())
            results.putString("verifiedAfter", OutspokenEngine.verified(targetContext).toString())
            results.putString("familiesAfter", OutspokenEngine.installedFamilies(targetContext).toString())
            results.putString("voicesAfter", OutspokenEngine.allVoices(targetContext).size.toString())
            val left = OutspokenEngine.unitFolders(
                listOfNotNull(OutspokenEngine.dataRoot(targetContext),
                              OutspokenEngine.inboxRoot(targetContext)), unit)
            results.putString("foldersLeft", left.joinToString().ifEmpty { "none" })
            check(unit !in after) { "$unit is still installed" }
            check(left.isEmpty()) { "copies of $unit survive: $left" }
            results.putString("verdict", "$unit removed, no copy left behind")
            finish(Activity.RESULT_OK, results)
        } catch (e: Throwable) {
            results.putString("failure", Log.getStackTraceString(e))
            finish(Activity.RESULT_CANCELED, results)
        }
    }

    /** Render one utterance so the worker process is up and holding its engine
     * mapped. -> what happened, for the report. */
    private fun warmWorker(): String {
        var tts: TextToSpeech? = null
        return try {
            val ready = CountDownLatch(1)
            val client = TextToSpeech(targetContext, { if (it == TextToSpeech.SUCCESS) ready.countDown() },
                "com.outspoken.tts")
            tts = client
            check(ready.await(60, TimeUnit.SECONDS)) { "the engine never initialized" }
            val done = CountDownLatch(1)
            client.setOnUtteranceProgressListener(object : UtteranceProgressListener() {
                override fun onStart(utteranceId: String) {}
                override fun onDone(utteranceId: String) { done.countDown() }
                override fun onError(utteranceId: String) { done.countDown() }
                override fun onError(utteranceId: String, code: Int) { done.countDown() }
            })
            val file = File(targetContext.filesDir, "warm.wav")
            check(client.synthesizeToFile("Hello there.", Bundle(), file, "warm") == TextToSpeech.SUCCESS)
            check(done.await(60, TimeUnit.SECONDS)) { "warm-up render timed out" }
            "spoke (" + file.length() + " bytes), the worker is live"
        } catch (e: Throwable) {
            "warm-up failed: " + e.message
        } finally {
            tts?.shutdown()
        }
    }

    private fun checkPitch() {
        val results = Bundle()
        var tts: TextToSpeech? = null
        try {
            check(OutspokenEngine.verified(targetContext)) { "no engine data on this device" }
            val ready = CountDownLatch(1)
            val client = TextToSpeech(targetContext, { if (it == TextToSpeech.SUCCESS) ready.countDown() },
                "com.outspoken.tts")
            tts = client
            check(ready.await(60, TimeUnit.SECONDS)) { "the engine never initialized" }
            check(client.setLanguage(Locale.US) >= TextToSpeech.LANG_AVAILABLE)
            check(client.setSpeechRate(1.0f) == TextToSpeech.SUCCESS)

            // Leave the settings as they were found: this runs on somebody's own
            // phone against their own data.
            val saved = OutspokenEngine.prefs(targetContext).all.toMap()
            restoreAfter = { restorePrefs(saved) }

            val wanted = voiceName
            val voice = OutspokenEngine.allVoices(targetContext).let { all ->
                (if (wanted != null) all.firstOrNull { it.name.equals(wanted, true) } else null)
                    ?: OutspokenEngine.defaultVoice(targetContext)
                    ?: all.firstOrNull()
            } ?: error("no voices")
            OutspokenEngine.prefs(targetContext).edit().putBoolean("override_voice", false).commit()
            val match = client.voices?.firstOrNull { it.name == voice.id }
                ?: error("voice ${voice.id} is not listed")
            check(client.setVoice(match) == TextToSpeech.SUCCESS)
            results.putString("voice", voice.id)
            results.putString("family", voice.family)

            fun render(tag: String, ratio: Float): ByteArray {
                check(client.setPitch(ratio) == TextToSpeech.SUCCESS) { "setPitch($ratio) refused" }
                val file = File(targetContext.filesDir, "pitch-$tag.wav")
                val done = CountDownLatch(1)
                var failure: String? = null
                client.setOnUtteranceProgressListener(object : UtteranceProgressListener() {
                    override fun onStart(utteranceId: String) {}
                    override fun onDone(utteranceId: String) { if (utteranceId == tag) done.countDown() }
                    override fun onError(utteranceId: String) {
                        if (utteranceId == tag) { failure = "synthesis failed"; done.countDown() }
                    }
                    override fun onError(utteranceId: String, code: Int) {
                        if (utteranceId == tag) { failure = "synthesis failed: $code"; done.countDown() }
                    }
                })
                check(client.synthesizeToFile("Capital A", Bundle(), file, tag) == TextToSpeech.SUCCESS)
                check(done.await(60, TimeUnit.SECONDS)) { "timed out at pitch $ratio" }
                check(failure == null) { "pitch $ratio: $failure" }
                val bytes = file.readBytes()
                check(bytes.size > 1000) { "pitch $ratio rendered ${bytes.size} bytes" }
                results.putString(tag, "${bytes.size} bytes " +
                    MessageDigest.getInstance("SHA-256").digest(bytes).joinToString("") { "%02x".format(it) })
                Log.i("OutspokenTest", "pitch $ratio -> ${bytes.size} bytes")
                return bytes
            }

            val normal = render("normal", 1.0f)
            val high = render("high", 1.5f)
            val low = render("low", 0.75f)
            val back = render("back", 1.0f)
            check(!high.contentEquals(normal)) {
                "pitch 1.5 rendered the same bytes as 1.0: the request is still being ignored"
            }
            check(!low.contentEquals(normal)) { "pitch 0.75 rendered the same bytes as 1.0" }
            check(!high.contentEquals(low)) { "raised and lowered pitch rendered alike" }
            check(back.contentEquals(normal)) {
                "returning to pitch 1.0 did not return the original audio: an offset outlived its request"
            }
            results.putString("verdict", "pitch changes the audio and returning to 1.0 restores it")
            finish(Activity.RESULT_OK, results)
        } catch (e: Throwable) {
            results.putString("failure", Log.getStackTraceString(e))
            finish(Activity.RESULT_CANCELED, results)
        } finally {
            tts?.shutdown()
            restoreAfter?.invoke(); restoreAfter = null
        }
    }

    private fun restorePrefs(saved: Map<String, Any?>) {
        val editor = OutspokenEngine.prefs(targetContext).edit().clear()
        for ((key, value) in saved) when (value) {
            is Boolean -> editor.putBoolean(key, value)
            is Int -> editor.putInt(key, value)
            is Long -> editor.putLong(key, value)
            is Float -> editor.putFloat(key, value)
            is String -> editor.putString(key, value)
            is Set<*> -> @Suppress("UNCHECKED_CAST") editor.putStringSet(key, value as Set<String>)
        }
        editor.commit()
    }
}
