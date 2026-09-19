package com.outspoken.tts

import android.content.Context
import android.net.Uri
import android.os.ParcelFileDescriptor
import android.provider.OpenableColumns
import java.io.BufferedInputStream
import java.io.EOFException
import java.io.File
import java.io.FileInputStream
import java.io.FileOutputStream
import java.io.IOException
import java.io.InputStream
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.nio.channels.FileChannel
import java.util.zip.Inflater
import java.util.zip.InflaterInputStream
import java.util.zip.ZipInputStream

/**
 * Engine data from a zip file.
 *
 * The desktop add-on extracts one folder, `outspoken`, holding an engine
 * folder per family -- `macintalk1`, `macintalk2`, `macintalk3`,
 * `macintalkpro`, `macintalkespanol` -- and `voices`, which the three
 * Speech Manager engines share.  A person zips that folder to get it onto a
 * phone in one piece.  The zip may hold the folder itself (`outspoken/...`),
 * its contents at the top, or the folder inside `outspoken-data` or
 * `macintalk/outspoken`, the way it sits on the desktop; folder names are
 * matched without regard to case, and a folder is known by what is in it
 * rather than by its name: `Cecy_1.bin` is MacinTalk 2 wherever it sits.
 * A zip of Mac OS X engine data -- a `MacinTalk.SpeechSynthesizer` bundle --
 * belongs to Panthera, and is said so.
 *
 * Reading the zip is `java.util.zip`, which is the platform's own: nothing
 * here needs a library, and nothing needs a licence.
 *
 * Two passes.  The check reads the central directory at the end of the file
 * when the file can be seeked -- a few kilobytes -- so the question "what is
 * in here?" is answered before the confirm dialog.  A source that cannot
 * seek is scanned front to back instead.  The import itself is one streamed
 * pass, written into `<folder>.importing` beside the real folder and
 * swapped in only at the end, so a failed or cancelled import leaves what
 * was there alone; `voices` is merged rather than replaced, because it is
 * shared and a zip may carry one new voice.
 */
object ZipImport {
    const val IMPORTING = ".importing"

    /** The engine folders as the desktop extractor names them, and what is
     * in each that says so. */
    val ENGINE_FOLDERS = mapOf(
        "macintalk1" to listOf("DRVR_1030.bin", "TALK_1001.bin", "RULZ_1129.bin"),
        "macintalk2" to listOf("Cecy_1.bin", "Cecy_3.bin"),
        "macintalk3" to listOf("ttvi_10.bin", "ttvi_8.bin", "ttvi_9.bin"),
        "macintalkpro" to listOf("gtse_1.bin", "datafork.bin", "rsrcfork.bin"),
        "macintalkespanol" to listOf("gtse_99.bin", "rsrcfork.bin"),
    )
    const val VOICES = "voices"
    val UNITS = ENGINE_FOLDERS.keys + VOICES

    /** What each unit is called to a person. */
    fun unitLabel(unit: String): String = when (unit) {
        "macintalk1" -> "MacinTalk 1 (1984)"
        "macintalk2" -> "MacinTalk 2"
        "macintalk3" -> "MacinTalk 3"
        "macintalkpro" -> "MacinTalk Pro"
        "macintalkespanol" -> "MacinTalk Pro (Spanish)"
        VOICES -> "voices"
        else -> unit
    }

    /** Folders a person might zip around the data, and which are stepped
     * into rather than imported as folders of their own. */
    private val WRAPPERS = setOf("outspoken", "outspoken-data", "macintalk")

    /** The bytes, twice over: a fresh stream from the start for the import,
     * and the same file as a channel when it can be seeked, for the check. */
    class Source(val name: String, val size: Long,
                 private val stream: () -> InputStream,
                 private val seekable: (() -> FileChannel?)? = null) {
        fun open(): InputStream = stream()
        fun channel(): FileChannel? = try { seekable?.invoke() } catch (e: Exception) { null }
    }

    /** A zip on disk, by path: the adb route, and the tests. */
    fun source(file: File) = Source(file.name, file.length(),
        { FileInputStream(file) }, { FileInputStream(file).channel })

    /** A zip the system picker handed over. Its display name and size come
     * from the provider; a provider that will not say a size says -1. */
    fun source(ctx: Context, uri: Uri): Source {
        val resolver = ctx.contentResolver
        var name = uri.lastPathSegment?.substringAfterLast('/') ?: "zip"
        var size = -1L
        try {
            resolver.query(uri, arrayOf(OpenableColumns.DISPLAY_NAME, OpenableColumns.SIZE),
                null, null, null)?.use { c ->
                if (c.moveToFirst()) {
                    c.getColumnIndex(OpenableColumns.DISPLAY_NAME).takeIf { it >= 0 }
                        ?.let { i -> if (!c.isNull(i)) name = c.getString(i) }
                    c.getColumnIndex(OpenableColumns.SIZE).takeIf { it >= 0 }
                        ?.let { i -> if (!c.isNull(i)) size = c.getLong(i) }
                }
            }
        } catch (e: Exception) { /* the name is a courtesy */ }
        if (size < 0) try {
            resolver.openFileDescriptor(uri, "r")?.use { size = it.statSize }
        } catch (e: Exception) { /* then the progress counts files instead */ }
        return Source(name, size,
            { resolver.openInputStream(uri) ?: throw IOException("Cannot open $name") },
            seekable@{
                val pfd = resolver.openFileDescriptor(uri, "r") ?: return@seekable null
                val stream = ParcelFileDescriptor.AutoCloseInputStream(pfd)
                // A pipe answers a size of zero, or throws; a file answers.
                val seekable = try { stream.channel.size() > 0 } catch (e: IOException) { false }
                if (seekable) stream.channel else { stream.close(); null }
            })
    }

    // ---- what is in the zip ----------------------------------------------

    class Entry(val name: String, val size: Long, val compressed: Long,
                val method: Int, val offset: Long)

    /** One folder inside the zip that will land under the data root. */
    class Found(
        val prefix: String,       // what precedes the folder in the zip: "outspoken/", or ""
        val source: String,       // the folder's name in the zip, as written there
        val unit: String,         // the canonical name it lands as: "macintalk2", "voices"
        val files: Int,
        val bytes: Long,          // unpacked, everything under it
        val voices: Int,          // for `voices`: the voice folders in it
    ) {
        val label: String get() = if (unit == VOICES) "$voices voice folder(s)" else unitLabel(unit)
    }

    /** The answer to "can this be imported, and as what?". Either `refusal`
     * says why not, in words meant for the person holding the phone, or
     * `found` lists what will be imported. */
    class Plan(val found: List<Found>, val refusal: String?) {
        val units: List<String> get() = found.map { it.unit }
        val bytes: Long get() = found.sumOf { it.bytes }
    }

    fun inspect(source: Source): Plan {
        val channel = source.channel()
        return if (channel != null) channel.use { inspectSeekable(it) }
        else inspectStreamed(source)
    }

    /** Slashes one way, and nothing that leaves the folder. Null means the
     * entry is not a file worth looking at: a directory, Finder's resource
     * forks, a desktop services file. */
    fun cleanName(raw: String): String? {
        val name = raw.replace('\\', '/')
        if (name.endsWith("/")) return null
        val parts = name.split('/')
        if (parts.any { it == "__MACOSX" } || parts.last() == ".DS_Store") return null
        return name
    }

    fun unsafe(name: String): Boolean =
        name.startsWith("/") || name.split('/').any { it == ".." || it.isEmpty() }

    /** Which unit a file's folder is, by name or by what it carries. */
    private fun unitOf(folderName: String, fileName: String): String? {
        val lower = folderName.lowercase()
        if (lower in UNITS) return lower
        for ((unit, markers) in ENGINE_FOLDERS) if (fileName in markers) return unit
        return null
    }

    private const val LAYOUT_HELP =
        "Zip the outspoken folder the desktop add-on extracted: it holds macintalk1, " +
        "macintalk2, macintalk3, macintalkpro, macintalkespanol and voices, or whichever " +
        "of them you have. Those folders can be at the top of the zip, inside outspoken, " +
        "inside outspoken-data, or inside macintalk/outspoken as they sit on the desktop."

    /** Decide from the entry list what the zip holds. */
    fun plan(entries: List<Entry>): Plan {
        val byName = LinkedHashMap<String, Entry>()
        for (e in entries) {
            val name = cleanName(e.name) ?: continue
            if (unsafe(name)) return Plan(emptyList(),
                "The zip holds a path that leaves its own folder (${e.name}), so it is not one this app will unpack.")
            byName[name] = e
        }
        if (byName.keys.any { n ->
                n.contains("MacinTalk.SpeechSynthesizer") || n.contains("SpeechDictionary.framework") ||
                n.substringBefore('/').lowercase() in setOf("tiger", "leopard", "snowleopard", "lion", "panthera", "panthera-data")
            }) return Plan(emptyList(),
                "Whoops, did you mean Panthera Speech? This zip has Mac OS X engine data. " +
                "Choose a zip with outSPOKEN engine data instead: the outspoken folder the " +
                "desktop add-on extracted.")
        // Each file's unit: the first folder on its path that is a unit by
        // name, else the folder holding a marker file; wrapper folders above
        // it are the prefix.
        class Key(val prefix: String, val source: String, val unit: String)
        val found = LinkedHashMap<String, Key>()          // "prefix|source" -> key
        val filesOf = HashMap<String, MutableList<String>>()
        val bytesOf = HashMap<String, Long>()
        for ((name, e) in byName) {
            val parts = name.split('/')
            if (parts.size < 2) continue                  // a loose file at the top
            var key: Key? = null
            for (i in 0 until parts.size - 1) {
                val unit = unitOf(parts[i], if (i == parts.size - 2) parts.last() else "")
                    ?: continue
                key = Key(parts.subList(0, i).joinToString("") { "$it/" }, parts[i], unit)
                break
            }
            if (key == null) {
                // No unit by name anywhere on the path: the folder holding a
                // marker file is the engine folder, whatever it is called.
                val folder = parts[parts.size - 2]
                val unit = unitOf("", parts.last()) ?: continue
                key = Key(parts.subList(0, parts.size - 2).joinToString("") { "$it/" }, folder, unit)
            }
            val id = key.prefix + key.source
            found.getOrPut(id) { key }
            filesOf.getOrPut(id) { ArrayList() }.add(name)
            bytesOf[id] = (bytesOf[id] ?: 0L) + e.size.coerceAtLeast(0)
        }
        if (found.isEmpty()) return Plan(emptyList(), "No engine data in this zip. " + LAYOUT_HELP)
        val list = found.map { (id, key) ->
            val files = filesOf[id]!!
            val voices = if (key.unit == VOICES)
                files.map { it.removePrefix(key.prefix + key.source + "/") }
                    .filter { it.contains('/') }.map { it.substringBefore('/') }.toSet().size
            else 0
            Found(key.prefix, key.source, key.unit, files.size, bytesOf[id] ?: 0L, voices)
        }
        val twice = list.groupBy { it.unit }.filter { it.value.size > 1 }.keys.firstOrNull()
        if (twice != null) return Plan(list,
            "The zip holds ${unitLabel(twice)} twice, in different folders, and this app " +
            "cannot choose between them.")
        return Plan(list, null)
    }

    // ---- the central directory, for a file that can be seeked --------------

    private fun ByteBuffer.u16(at: Int) = getShort(at).toInt() and 0xffff
    private fun ByteBuffer.u32(at: Int) = getInt(at).toLong() and 0xffffffffL

    private fun FileChannel.readFully(position: Long, length: Int): ByteBuffer {
        val buffer = ByteBuffer.allocate(length).order(ByteOrder.LITTLE_ENDIAN)
        var at = position
        while (buffer.hasRemaining()) {
            val n = read(buffer, at)
            if (n < 0) throw EOFException("zip ends early")
            at += n
        }
        buffer.flip()
        return buffer
    }

    /** The entries, from the central directory at the end of the file.
     * Zip64 is honoured where a zipper used it; a comment up to the format's
     * limit is searched past. Throws when this is not a zip. */
    fun centralDirectory(channel: FileChannel): List<Entry> {
        val size = channel.size()
        val tailLength = minOf(size, 22L + 0xffff).toInt()
        val tail = channel.readFully(size - tailLength, tailLength)
        var eocd = -1
        var i = tailLength - 22
        while (i >= 0) { if (tail.u32(i) == 0x06054b50L) { eocd = i; break }; i-- }
        if (eocd < 0) throw IOException("not a zip file")
        var count = tail.u16(eocd + 10).toLong()
        var cdSize = tail.u32(eocd + 12)
        var cdOffset = tail.u32(eocd + 16)
        if (count == 0xffffL || cdSize == 0xffffffffL || cdOffset == 0xffffffffL) {
            val locatorAt = size - tailLength + eocd - 20
            if (locatorAt >= 0) {
                val locator = channel.readFully(locatorAt, 20)
                if (locator.u32(0) == 0x07064b50L) {
                    val record = channel.readFully(locator.getLong(8), 56)
                    if (record.u32(0) == 0x06064b50L) {
                        count = record.getLong(32)
                        cdSize = record.getLong(40)
                        cdOffset = record.getLong(48)
                    }
                }
            }
        }
        if (cdSize > 256L shl 20 || cdOffset + cdSize > size) throw IOException("zip directory out of range")
        val cd = channel.readFully(cdOffset, cdSize.toInt())
        val entries = ArrayList<Entry>(count.coerceAtMost(1 shl 20).toInt())
        var at = 0
        while (at + 46 <= cd.limit() && entries.size < count) {
            if (cd.u32(at) != 0x02014b50L) throw IOException("zip directory damaged")
            val flags = cd.u16(at + 8)
            val method = cd.u16(at + 10)
            var compressed = cd.u32(at + 20)
            var uncompressed = cd.u32(at + 24)
            val nameLength = cd.u16(at + 28)
            val extraLength = cd.u16(at + 30)
            val commentLength = cd.u16(at + 32)
            var offset = cd.u32(at + 42)
            val nameBytes = ByteArray(nameLength).also { cd.position(at + 46); cd.get(it) }
            val name = String(nameBytes, if (flags and 0x800 != 0) Charsets.UTF_8 else Charsets.ISO_8859_1)
            var extraAt = at + 46 + nameLength
            val extraEnd = extraAt + extraLength
            while (extraAt + 4 <= extraEnd) {
                val id = cd.u16(extraAt); val length = cd.u16(extraAt + 2)
                if (id == 1) {
                    var f = extraAt + 4
                    if (uncompressed == 0xffffffffL && f + 8 <= extraAt + 4 + length) { uncompressed = cd.getLong(f); f += 8 }
                    if (compressed == 0xffffffffL && f + 8 <= extraAt + 4 + length) { compressed = cd.getLong(f); f += 8 }
                    if (offset == 0xffffffffL && f + 8 <= extraAt + 4 + length) { offset = cd.getLong(f) }
                }
                extraAt += 4 + length
            }
            entries.add(Entry(name, uncompressed, compressed, method, offset))
            at += 46 + nameLength + extraLength + commentLength
        }
        return entries
    }

    /** One entry's bytes, read in place: stored or deflated, nothing else. */
    fun readEntry(channel: FileChannel, entry: Entry): ByteArray? {
        val header = channel.readFully(entry.offset, 30)
        if (header.u32(0) != 0x04034b50L) return null
        val dataAt = entry.offset + 30 + header.u16(26) + header.u16(28)
        val stream: InputStream = when (entry.method) {
            0 -> ChannelInputStream(channel, dataAt, entry.compressed, pad = false)
            8 -> InflaterInputStream(ChannelInputStream(channel, dataAt, entry.compressed, pad = true),
                Inflater(true), 1 shl 16)
            else -> return null
        }
        return stream.use { it.readBytes() }
    }

    private class ChannelInputStream(private val channel: FileChannel, start: Long,
                                     private val length: Long, pad: Boolean) : InputStream() {
        private var at = start
        private var left = length
        private var padded = !pad
        override fun read(): Int {
            val one = ByteArray(1)
            return if (read(one, 0, 1) == 1) one[0].toInt() and 0xff else -1
        }
        override fun read(b: ByteArray, off: Int, len: Int): Int {
            if (len == 0) return 0
            if (left <= 0) {
                if (padded) return -1
                padded = true; b[off] = 0
                return 1
            }
            val n = channel.read(ByteBuffer.wrap(b, off, minOf(len.toLong(), left).toInt()), at)
            if (n < 0) return -1
            at += n; left -= n
            return n
        }
    }

    private fun inspectSeekable(channel: FileChannel): Plan = try {
        plan(centralDirectory(channel))
    } catch (e: IOException) {
        Plan(emptyList(), "This is not a zip file this app can read (${e.message}).")
    }

    // ---- front to back, for a source that cannot be seeked -----------------

    private fun inspectStreamed(source: Source): Plan = try {
        val entries = ArrayList<Entry>()
        ZipInputStream(BufferedInputStream(source.open(), 1 shl 16)).use { zip ->
            val buffer = ByteArray(1 shl 16)
            while (true) {
                val e = zip.nextEntry ?: break
                var size = 0L
                while (true) { val n = zip.read(buffer); if (n < 0) break; size += n }
                entries.add(Entry(e.name, size, e.compressedSize, e.method, -1))
                zip.closeEntry()
            }
        }
        if (entries.isEmpty()) throw IOException("no entries")
        plan(entries)
    } catch (e: IOException) {
        // Android's own ZipInputStream refuses a `..` path before this code
        // sees it; the refusal reads the same either way.
        val path = Regex("Invalid zip entry path: (.*)").find(e.message ?: "")?.groupValues?.get(1)
        if (path != null) Plan(emptyList(),
            "The zip holds a path that leaves its own folder ($path), so it is not one this app will unpack.")
        else Plan(emptyList(), "This is not a zip file this app can read (${e.message}).")
    }

    // ---- the import itself --------------------------------------------------

    class Cancelled : IOException("cancelled")

    /** Unpack every folder in `plan` under `root`, each as its canonical
     * name, replacing an engine folder that is there and merging into
     * `voices`, only once the whole zip has been written.  `progress(done,
     * total)` is called as bytes of the zip go by; `total` is -1 when the
     * source would not say its size.  `cancelled()` is polled between
     * writes.  Returns the units now in place. */
    fun extract(source: Source, plan: Plan, root: File,
                progress: (Long, Long) -> Unit, cancelled: () -> Boolean): List<String> {
        check(plan.refusal == null && plan.found.isNotEmpty()) { "nothing to import" }
        // The longest prefix wins, so a folder inside another folder's
        // prefix sends its files where they belong.
        val targets = plan.found.sortedByDescending { (it.prefix + it.source).length }
            .map { it to File(root, it.unit + IMPORTING) }
        root.mkdirs()
        for ((_, dir) in targets) if (dir.exists()) dir.deleteRecursively()
        val counting = CountingInputStream(source.open())
        try {
            ZipInputStream(BufferedInputStream(counting, 1 shl 16)).use { zip ->
                val buffer = ByteArray(1 shl 16)
                while (true) {
                    val e = zip.nextEntry ?: break
                    val name = cleanName(e.name)
                    val target = name?.let { n ->
                        targets.firstOrNull { n.startsWith(it.first.prefix + it.first.source + "/") }
                    }
                    if (name == null || target == null || unsafe(name)) { zip.closeEntry(); continue }
                    val out = File(target.second, name.removePrefix(target.first.prefix + target.first.source + "/"))
                    out.parentFile?.mkdirs()
                    FileOutputStream(out).use { o ->
                        while (true) {
                            if (cancelled()) throw Cancelled()
                            val n = zip.read(buffer)
                            if (n < 0) break
                            o.write(buffer, 0, n)
                            progress(counting.count, source.size)
                        }
                    }
                    zip.closeEntry()
                }
            }
            // Everything is on disk. Now, and only now, the swap, in the
            // order the plan named them.
            val done = ArrayList<String>()
            for (found in plan.found) {
                val dir = targets.first { it.first === found }.second
                if (!dir.isDirectory) throw IOException("the import of ${found.unit} is incomplete")
                val live = File(root, found.unit)
                if (found.unit == VOICES && live.isDirectory) {
                    // Merged: each voice folder replaces its namesake and
                    // the others stay.
                    for (voice in dir.listFiles()?.sortedBy { it.name } ?: emptyList()) {
                        val twin = File(live, voice.name)
                        if (twin.exists()) twin.deleteRecursively()
                        if (!voice.renameTo(twin)) throw IOException("could not move ${voice.name} into place")
                    }
                    dir.deleteRecursively()
                } else {
                    if (live.exists()) live.deleteRecursively()
                    if (!dir.renameTo(live)) throw IOException("could not move ${found.unit} into place")
                }
                done.add(found.unit)
            }
            progress(source.size, source.size)
            return done
        } catch (e: Throwable) {
            for ((_, dir) in targets) dir.deleteRecursively()
            throw e
        } finally {
            try { counting.close() } catch (e: IOException) { /* already failing, or done */ }
        }
    }

    private class CountingInputStream(private val inner: InputStream) : InputStream() {
        @Volatile var count = 0L
        override fun read(): Int = inner.read().also { if (it >= 0) count++ }
        override fun read(b: ByteArray, off: Int, len: Int): Int =
            inner.read(b, off, len).also { if (it > 0) count += it }
        override fun close() = inner.close()
    }

    /** How a size reads to a person: "1.2 GB", "37 MB". */
    fun sizeText(bytes: Long): String = when {
        bytes >= 1L shl 30 -> String.format("%.1f GB", bytes / (1024.0 * 1024 * 1024))
        bytes >= 1L shl 20 -> "${bytes shr 20} MB"
        else -> "${(bytes shr 10).coerceAtLeast(1)} KB"
    }
}
