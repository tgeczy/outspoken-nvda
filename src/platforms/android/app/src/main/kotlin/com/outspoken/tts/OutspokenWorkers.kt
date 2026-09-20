package com.outspoken.tts

import android.content.ComponentName
import android.content.Context
import android.content.Intent
import android.content.ServiceConnection
import android.os.IBinder
import android.os.Looper
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit

/** The one binding to the engine's process.  A retired connection stays
 * until its Binder dies, so a replacement cannot bind the exiting process.
 *
 * Panthera keeps one worker per engine generation and retires a worker to
 * cancel an utterance, because its engine cannot be interrupted.  This host
 * can be -- stop() lands inside a render and cancel() abandons the rest --
 * and it opens whichever engine a voice belongs to in a few tens of
 * milliseconds, so one process serves every voice and is retired only when
 * its Binder is found dead. */
internal object OutspokenWorkers {
    private val lock = Any()
    private class Connection(val context: Context) : ServiceConnection {
        val ready = CountDownLatch(1)
        val closed = CountDownLatch(1)
        var api: IOutspokenWorker? = null
        var bound = false
        var retired = false

        override fun onServiceConnected(name: ComponentName, binder: IBinder) = synchronized(lock) {
            if (retired) return@synchronized
            api = IOutspokenWorker.Stub.asInterface(binder)
            try {
                binder.linkToDeath({ disconnected() }, 0)
            } catch (_: android.os.DeadObjectException) {
                disconnected()
            }
            ready.countDown()
        }
        private fun unbind() {
            if (bound) {
                context.unbindService(this)
                bound = false
            }
        }
        private fun disconnected() = synchronized(lock) {
            retired = true
            unbind()
            ready.countDown()
            closed.countDown()
        }
        override fun onServiceDisconnected(name: ComponentName) { disconnected() }
        override fun onBindingDied(name: ComponentName) = synchronized(lock) { retire() }
        override fun onNullBinding(name: ComponentName) { disconnected() }

        // Called under lock. Never await Binder death while holding it: the
        // death recipient needs this lock, and cancellation must return promptly.
        fun retire() {
            if (retired) return
            retired = true
            unbind() // Drop BIND_AUTO_CREATE BEFORE intentionally killing it.
            ready.countDown()
            val target = api
            if (target == null || !target.asBinder().isBinderAlive) {
                closed.countDown()
                return
            }
            android.util.Log.i("OutspokenEngine", "Retiring the engine worker")
            try { target.shutdown() } catch (_: android.os.DeadObjectException) {
                closed.countDown()
            }
        }
    }
    private var connection: Connection? = null

    /** Identity matters: a late retirement must never retire a replacement. */
    fun retire(api: IOutspokenWorker) = synchronized(lock) {
        connection?.takeIf { it.api?.asBinder() === api.asBinder() }?.retire()
        Unit
    }

    /** Retire the worker and wait for its process to go, so the next get()
     * starts a fresh one -- the way out of a wedged engine. */
    fun restart() {
        val current = synchronized(lock) { connection?.also { it.retire() } } ?: return
        check(current.closed.await(10, TimeUnit.SECONDS)) { "Engine worker did not stop" }
        synchronized(lock) { if (connection === current) connection = null }
    }

    // Synthesis callers are serialized. Lifecycle callbacks and stop are not.
    fun get(context: Context): IOutspokenWorker {
        check(Looper.myLooper() != Looper.getMainLooper())
        val ctx = context.applicationContext
        while (true) {
            val current = synchronized(lock) {
                connection ?: Connection(ctx).also { made ->
                    made.bound = ctx.bindService(Intent(ctx, OutspokenWorkerService::class.java),
                                                 made, Context.BIND_AUTO_CREATE)
                    check(made.bound) { "Engine binding failed" }
                    connection = made
                }
            }
            if (!current.ready.await(30, TimeUnit.SECONDS)) {
                synchronized(lock) { current.retire() }
                error("Engine connection timed out")
            }
            synchronized(lock) {
                if (!current.retired) {
                    current.api?.let { if (it.asBinder().isBinderAlive) return it }
                    current.retire()
                }
            }
            check(current.closed.await(10, TimeUnit.SECONDS)) { "Engine worker did not stop" }
            synchronized(lock) { if (connection === current) connection = null }
            // A failed initial binding is an error, not an unbounded respawn loop.
            check(current.api != null) { "Engine connection failed" }
        }
    }
}
