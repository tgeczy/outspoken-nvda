package com.outspoken.tts

import android.app.Service
import android.content.Intent
import android.os.IBinder
import java.util.concurrent.Callable
import java.util.concurrent.Executors

/** The engine in its own private process, so a fault in guest code can never
 * take the public TTS service down with it.  One process for every engine:
 * the host is one emulated CPU that opens the engine a voice belongs to and
 * closes whatever was open before, in about fifty milliseconds on a phone,
 * so switching from MacinTalk 3 to Pro is a call, not a process.
 *
 * Native calls use one persistent thread, in order.  stop() bypasses it: it
 * is the one cross-thread call the host allows, and it is what makes a
 * cancel land inside a render rather than after it. */
class OutspokenWorkerService : Service() {
    private val synthesis = Executors.newSingleThreadExecutor { work ->
        Thread(work, "outspoken-engine").apply {
            priority = Thread.MAX_PRIORITY
        }
    }
    private fun <T> runNative(block: () -> T): T = synthesis.submit(Callable(block)).get()
    private val binder = object : IOutspokenWorker.Stub() {
        override fun scan(roots: String): String = runNative {
            OutspokenNative.nativeScan(roots) ?: ""
        }
        override fun useVoice(roots: String, id: String): Int = runNative {
            OutspokenNative.nativeUseVoice(roots, id)
        }
        override fun settings(rate: Int, pitch: Int, inflection: Int, volume: Int,
                              numbers: Int, ratePercent: Int) = runNative {
            OutspokenNative.nativeSettings(rate, pitch, inflection, volume, numbers, ratePercent)
        }
        override fun start(utf8: ByteArray): Int = runNative {
            OutspokenNative.nativeStart(utf8)
        }
        override fun pull(capacity: Int): ByteArray? = runNative {
            val samples = ShortArray(capacity.coerceIn(1, 8192))
            val count = OutspokenNative.nativePull(samples)
            if (count < 0) null else ByteArray(count * 2).also { bytes ->
                for (i in 0 until count) {
                    bytes[i * 2] = samples[i].toByte()
                    bytes[i * 2 + 1] = (samples[i].toInt() shr 8).toByte()
                }
            }
        }
        override fun cancel() = runNative { OutspokenNative.nativeCancel() }
        // Straight through, from the Binder thread: the host's stop flag.
        override fun stop() { OutspokenNative.nativeStop() }
        // Only the app can bind this service.  The owner waits for Binder
        // death before binding a new worker; the public TTS process lives on.
        override fun shutdown() { android.os.Process.killProcess(android.os.Process.myPid()) }
    }
    override fun onBind(intent: Intent): IBinder = binder
    override fun onDestroy() {
        synthesis.shutdownNow()
        super.onDestroy()
    }
}
