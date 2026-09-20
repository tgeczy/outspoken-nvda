package com.outspoken.tts

import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.content.SharedPreferences
import android.os.Build
import android.os.StatFs
import android.os.UserManager
import android.util.Log
import java.io.File
import java.util.Locale

/** The app-process side of the engine: where the data is, which voices it
 * provides, the settings for each engine family, the gate, the move into
 * protected storage, and ownership of one utterance at a time on the worker.
 *
 * The voice list and everything about the data come from the host's own
 * catalogue (src/osp_voices.c, the same scan the NVDA driver and the SAPI
 * engine use), called in this process as a plain folder walk: it finds the
 * engine folders wherever they sit under a root, so a folder copied over as
 * `outspoken`, as `outspoken-data`, or as the bare engine folders all work. */
object OutspokenEngine {
    const val PREFS = "outspoken"
    const val PREF_VERIFIED = "engine_verified"      // set by "Check Engine"
    const val PREF_DEFAULT_VOICE = "default_voice"   // per family: the host's voice id
    const val PREF_FAMILY = "engine_family"          // the family of the chosen voice
    /** Families the person has switched off.  Their data may well be
     * present; they are simply not wanted, and a family that is not wanted
     * is not offered anywhere: not in the voice list a screen reader sees,
     * not in the settings picker, not as a fallback. */
    const val PREF_DISABLED_FAMILIES = "disabled_families"
    const val PREF_RATE = "rate"                     // 0-100, the NVDA slider
    const val PREF_PITCH = "pitch"                   // 0-100, 50 as recorded
    const val PREF_INFLECTION = "inflection"         // 0-100, 50 as recorded
    const val PREF_VOLUME = "volume"                 // -1 = 100, else 0-100
    const val PREF_NUMBERS = "numbers"               // words | digits | off
    const val NUMBERS_DEFAULT = "words"
    val NUMBERS_VALUES = listOf("words", "digits", "off")

    const val DATA_DIR = "outspoken-data"

    // The engine families, in the order the desktop lists them.  A voice's
    // `creator` field in the catalogue names its family; the two MacinTalk
    // Pro engines are one engine body (`gala`) with the Spanish one told
    // apart by its creator, exactly as the SAPI settings window has them.
    const val FAM_SP = "sp"
    const val FAM_MTK2 = "mtk2"
    const val FAM_MTK3 = "mtk3"
    const val FAM_GALA = "gala"
    const val FAM_CAMI = "cami"
    val FAMILIES = listOf(FAM_SP, FAM_MTK2, FAM_MTK3, FAM_GALA, FAM_CAMI)

    /** How a family is named to a person. */
    fun famLabel(fam: String): String = when (fam) {
        FAM_SP -> "MacinTalk 1 (1984)"
        FAM_MTK2 -> "MacinTalk 2"
        FAM_MTK3 -> "MacinTalk 3"
        FAM_GALA -> "MacinTalk Pro"
        FAM_CAMI -> "MacinTalk Pro (Spanish)"
        else -> fam
    }

    data class VoiceInfo(
        val name: String,      // "Fred"
        val hostId: String,    // the catalogue's id, "mtk3:Fred", what opens it
        val family: String,    // FAM_*
        val language: String,  // "en" or "es"
        val gender: Int,       // 1 male, 2 female, 0 unknown
    ) {
        /** Android's voice id: what a screen reader stores. */
        val id: String get() = "outspoken-$family-${name.lowercase(Locale.ROOT).replace(' ', '-')}"
        /** How the voice is named in a list a person reads, family-qualified
         * because the SAPI tokens read the same way and two platforms should
         * describe one voice identically. */
        val label: String get() = "$name (${famLabel(family)})"
        val locale: Locale get() = if (language == "es") Locale("es", "MX") else Locale.US
    }

    // ---- data layout -------------------------------------------------------
    //
    // Data lives in device-protected storage: the half of the app's files
    // that exists before the phone has been unlocked, which is what lets a
    // screen reader read the lock screen after a reboot with these voices --
    // see ProtectedStorage for the whole of it.  Two other places are looked
    // in, and only once unlocked, because before that they do not exist: the
    // external files dir a PC's file window shows, which is the inbox a
    // person copies into, and the internal files dir a debug push writes to.
    // Whatever is found in either is moved into protected storage by
    // `migrate`, so a lookup that finds data there is finding it on its way in.

    @Volatile private var knownUnlocked = false

    fun unlocked(ctx: Context): Boolean {
        if (knownUnlocked) return true
        val now = try { ctx.getSystemService(UserManager::class.java)?.isUserUnlocked ?: true }
                  catch (e: Exception) { true }
        if (now) knownUnlocked = true
        return now
    }

    private fun protectedContext(ctx: Context): Context =
        if (ctx.isDeviceProtectedStorage) ctx else ctx.createDeviceProtectedStorageContext()

    /** Where the engine data lives, and where a zip import lands. */
    fun dataRoot(ctx: Context): File = File(protectedContext(ctx).filesDir, DATA_DIR)

    /** The folder a PC's file window shows -- the inbox -- or null before unlock. */
    fun inboxRoot(ctx: Context): File? =
        if (!unlocked(ctx)) null
        else try { ctx.getExternalFilesDir(null)?.let { File(it, DATA_DIR) } }
             catch (e: Exception) { null }

    /** The internal folder a debug push writes to.  Null before unlock. */
    private fun legacyRoot(ctx: Context): File? =
        if (!unlocked(ctx)) null else File(ctx.filesDir, DATA_DIR)

    /** The folder names the desktop extractor makes, which is how a folder
     * is told from a wrapper somebody zipped or copied around them. */
    private val UNIT_NAMES = ZipImport.UNITS

    /** Whether a folder holds the engine data directly: a unit folder, or
     * a marker file, at its top. */
    private fun holdsData(dir: File): Boolean =
        dir.listFiles()?.any { it.name.lowercase() in UNIT_NAMES } == true

    /** The roots as the host wants them.  Engine files are found anywhere
     * under a root, but `voices` has to sit directly in one, so a folder
     * copied over as `outspoken` or `outspoken-data`, or as the desktop's
     * `macintalk/outspoken`, is a root of its own: each base root, then
     * every wrapper one or two levels down that holds the data. */
    private fun candidateRoots(ctx: Context): List<File> {
        val out = ArrayList<File>()
        for (base in listOfNotNull(dataRoot(ctx), inboxRoot(ctx), legacyRoot(ctx))) {
            if (!base.isDirectory) continue
            out.add(base)
            for (child in base.listFiles()?.filter { it.isDirectory } ?: emptyList()) {
                if (holdsData(child)) out.add(child)
                for (grandchild in child.listFiles()?.filter { it.isDirectory } ?: emptyList())
                    if (holdsData(grandchild)) out.add(grandchild)
            }
        }
        return out
    }

    /** The roots as the host takes them, one per line, in the order that
     * decides which copy of a file wins. */
    fun rootsArgument(ctx: Context): String = candidateRoots(ctx).joinToString("\n") { it.absolutePath }

    // ---- the catalogue -----------------------------------------------------

    private class Catalogue(val key: String, val voices: List<VoiceInfo>, val skipped: List<Pair<String, String>>)
    private val catalogueLock = Any()
    @Volatile private var catalogue: Catalogue? = null

    /** Folder contents change on import and on a move, not on every spoken
     * digit: the catalogue is rebuilt when the roots or their top-level
     * folders have changed, or when someone who changed them says so. */
    fun refreshVoiceCatalogue() = synchronized(catalogueLock) { catalogue = null }

    private fun catalogueKey(ctx: Context): String = candidateRoots(ctx).joinToString("\n") { root ->
        val children = root.listFiles()?.sortedBy { it.name }?.joinToString(",") { "${it.name}:${it.lastModified()}" } ?: ""
        "${root.absolutePath}@${root.lastModified()}[$children]"
    }

    private fun scanned(ctx: Context): Catalogue = synchronized(catalogueLock) {
        val key = catalogueKey(ctx)
        catalogue?.takeIf { it.key == key }?.let { return@synchronized it }
        val roots = rootsArgument(ctx)
        val voices = ArrayList<VoiceInfo>()
        val skipped = ArrayList<Pair<String, String>>()
        if (roots.isNotEmpty()) {
            val text = try { OutspokenNative.nativeScan(roots) } catch (e: Throwable) {
                Log.w("OutspokenEngine", "the catalogue scan failed", e); null
            }
            for (line in (text ?: "").split('\n')) {
                if (line.isEmpty()) continue
                // id, label, kind, creator, voice id, name, language, gender, folder
                val f = line.split('\t')
                if (f.size < 8) continue
                voices.add(VoiceInfo(name = f[5], hostId = f[0], family = f[3],
                                     language = f[6], gender = f[7].toIntOrNull() ?: 0))
            }
            try {
                for (line in OutspokenNative.nativeSkipped().split('\n')) {
                    if (line.isEmpty()) continue
                    val at = line.indexOf('\t')
                    if (at > 0) skipped.add(line.substring(0, at) to line.substring(at + 1))
                }
            } catch (e: Throwable) { /* a diagnostic, never a failure */ }
        }
        Catalogue(key, voices, skipped).also { catalogue = it }
    }

    /** Every voice the data provides, whatever the switches say. */
    fun installedVoices(ctx: Context): List<VoiceInfo> = scanned(ctx).voices

    /** What the last scan passed over and why, for the Setup page. */
    fun skippedFolders(ctx: Context): List<Pair<String, String>> = scanned(ctx).skipped

    /** The families whose data is present, whether offered or not. */
    fun installedFamilies(ctx: Context): List<String> {
        val present = installedVoices(ctx).map { it.family }.toSet()
        return FAMILIES.filter { it in present }
    }

    /** The families the person has switched off in Engine settings. */
    fun disabledFamilies(ctx: Context): Set<String> =
        prefs(ctx).getStringSet(PREF_DISABLED_FAMILIES, null) ?: emptySet()

    fun famOffered(ctx: Context, fam: String): Boolean = fam !in disabledFamilies(ctx)

    /** Switch a family on or off.  Off means its voices vanish from every
     * list at once, its data untouched; on brings them back.  The last
     * family standing cannot be switched off, because an engine with no
     * voices is a screen reader with no speech. */
    fun setFamOffered(ctx: Context, fam: String, offered: Boolean): Boolean {
        val disabled = disabledFamilies(ctx).toMutableSet()
        if (offered) disabled.remove(fam) else {
            val remaining = installedFamilies(ctx).filter { it != fam && it !in disabled }
            if (remaining.isEmpty()) return false
            disabled.add(fam)
        }
        prefs(ctx).edit().putStringSet(PREF_DISABLED_FAMILIES, disabled).apply()
        return true
    }

    /** Present AND not switched off: what the picker and the voice list use. */
    fun availableFamilies(ctx: Context): List<String> =
        installedFamilies(ctx).filter { famOffered(ctx, it) }

    /** Every voice of every family that is present and wanted, in the
     * desktop's order: family by family, and within a family as the
     * catalogue lists them -- Male before Female for 1984, the rest by name. */
    fun allVoices(ctx: Context): List<VoiceInfo> {
        val offered = availableFamilies(ctx).toSet()
        return installedVoices(ctx).filter { it.family in offered }
            .sortedBy { FAMILIES.indexOf(it.family) }          // stable: catalogue order within
    }

    fun voicesOf(ctx: Context, fam: String): List<VoiceInfo> = allVoices(ctx).filter { it.family == fam }

    fun voiceById(ctx: Context, id: String?): VoiceInfo? =
        allVoices(ctx).firstOrNull { it.id == id }

    /** Voices that speak Spanish -- Carlos and Catalina, when the Mexican
     * Spanish MacinTalk Pro is present. */
    fun spanishVoices(ctx: Context): List<VoiceInfo> = allVoices(ctx).filter { it.language == "es" }

    /** A family just imported is wanted, whatever its switch said before;
     * then the catalogue is rebuilt and the gate re-run, so the new voices
     * are offered without a trip to Check Engine. */
    fun imported(ctx: Context) {
        refreshVoiceCatalogue()
        val disabled = disabledFamilies(ctx).toMutableSet()
        if (disabled.removeAll(installedFamilies(ctx).toSet()))
            prefs(ctx).edit().putStringSet(PREF_DISABLED_FAMILIES, disabled).apply()
        checkEngine(ctx)
    }

    // ---- the chosen voice --------------------------------------------------

    /** The family the engine is (or would be) speaking from: the family of
     * the chosen voice if its data is actually there, otherwise the first
     * family that is.  Presence wins over preference, so a stored choice
     * whose data has since been removed never strands the app. */
    fun activeFamily(ctx: Context): String {
        val chosen = prefs(ctx).getString(PREF_FAMILY, null)
        val available = availableFamilies(ctx)
        if (chosen != null && chosen in available) return chosen
        return available.firstOrNull() ?: FAM_MTK3
    }

    fun voicePrefKey(fam: String) = "${PREF_DEFAULT_VOICE}_$fam"

    /** The chosen voice of a family, as the host's id, or null. */
    fun defaultVoiceId(ctx: Context, fam: String): String? = prefs(ctx).getString(voicePrefKey(fam), null)

    /** The voice a family speaks with: the chosen one, else Fred for
     * MacinTalk 3 (the voice everyone remembers), else the first. */
    fun defaultVoice(ctx: Context, fam: String): VoiceInfo? {
        val voices = voicesOf(ctx, fam)
        val saved = defaultVoiceId(ctx, fam)
        return voices.firstOrNull { it.hostId == saved }
            ?: voices.firstOrNull { it.name.equals("Fred", true) }
            ?: voices.firstOrNull()
    }

    /** The voice the engine speaks with when nobody asks for another: the
     * chosen one, or -- before anything has been chosen -- MacinTalk 3's
     * Fred if it is here, the voice everyone remembers, else the first. */
    fun defaultVoice(ctx: Context): VoiceInfo? {
        if (prefs(ctx).getString(PREF_FAMILY, null) == null)
            allVoices(ctx).firstOrNull { it.family == FAM_MTK3 && it.name.equals("Fred", true) }?.let { return it }
        return defaultVoice(ctx, activeFamily(ctx)) ?: allVoices(ctx).firstOrNull()
    }

    /** Record a voice choice, and with it the family that voice belongs to.
     * These are one action: choosing "Bruce (MacinTalk Pro)" *is* choosing
     * MacinTalk Pro. */
    fun chooseVoice(ctx: Context, voice: VoiceInfo) {
        prefs(ctx).edit()
            .putString(voicePrefKey(voice.family), voice.hostId)
            .putString(PREF_FAMILY, voice.family)
            .apply()
    }

    // ---- the gate ----------------------------------------------------------

    @Volatile private var prefsMoved = false

    /** The settings, in device-protected storage so the service can read
     * them before unlock.  The first call in a process after unlock carries
     * them over from where they used to live -- a no-op once nothing is
     * left there.  The other half cannot even be asked before unlock. */
    fun prefs(ctx: Context): SharedPreferences {
        val protected = protectedContext(ctx)
        if (!prefsMoved && !ctx.isDeviceProtectedStorage && unlocked(ctx)) {
            try { protected.moveSharedPreferencesFrom(ctx, PREFS) }
            catch (e: Exception) { Log.w("OutspokenEngine", "settings could not be moved", e) }
            prefsMoved = true
        }
        return protected.getSharedPreferences(PREFS, Context.MODE_PRIVATE)
    }

    /** The engine is usable only after the user has run "Check Engine" AND
     * the data is actually there.  This is the gate the service honours: no
     * verify, no voices. */
    fun verified(ctx: Context): Boolean =
        prefs(ctx).getBoolean(PREF_VERIFIED, false) && allVoices(ctx).isNotEmpty()

    /** Run the check: does anything speak?  Records the result as the gate. */
    fun checkEngine(ctx: Context): Boolean {
        refreshVoiceCatalogue()
        val ok = allVoices(ctx).isNotEmpty()
        prefs(ctx).edit().putBoolean(PREF_VERIFIED, ok).apply()
        return ok
    }

    // ---- moving data into protected storage --------------------------------
    //
    // Whatever is found in the inbox or the old internal folder is copied
    // into protected storage and then removed from where it was.  The engine
    // keeps working throughout.  One move at a time, and never on the main
    // thread.  Folder by folder rather than the whole tree at once, and a
    // folder that already exists in protected storage is merged into rather
    // than replaced: `voices` is shared by three engines, and a person who
    // copies one new voice folder must not lose the others.

    private val migrateLock = Any()
    @Volatile private var unlockWatcher: BroadcastReceiver? = null

    @Volatile var migrationNote: String? = null
        private set

    @Volatile var migrating: Boolean = false
        private set

    private val STAGING = setOf(ZipImport.IMPORTING, ProtectedStorage.MOVING)

    /** (folder, name) pairs sitting outside protected storage, `folder/name`
     * being what moves and landing as `name` under the data root.  Empty
     * before unlock, because those folders cannot be looked at.  A wrapper
     * folder -- `outspoken`, `outspoken-data`, `macintalk/outspoken` -- is
     * stepped into rather than moved, so the data lands flat, the way the
     * desktop lays it out.  The internal folder first and the inbox last,
     * so the inbox wins. */
    fun pendingMoves(ctx: Context): List<Pair<File, String>> {
        val out = ArrayList<Pair<File, String>>()
        fun walk(dir: File, depth: Int) {
            for (f in dir.listFiles()?.filter { it.isDirectory }?.sortedBy { it.name } ?: emptyList()) {
                if (STAGING.any { f.name.endsWith(it) }) continue
                if (f.name.lowercase() !in UNIT_NAMES && depth < 2 && (holdsData(f) || f.listFiles()?.any { it.isDirectory && holdsData(it) } == true))
                    walk(f, depth + 1)
                else out.add(dir to f.name)
            }
        }
        for (root in listOfNotNull(legacyRoot(ctx), inboxRoot(ctx))) if (root.isDirectory) walk(root, 0)
        return out
    }

    /** The folders under `from/<name>` that do not yet exist under `into`,
     * which are what gets moved whole; a folder that does exist is descended
     * into.  Loose files are copied one by one. */
    private fun moveMerged(from: File, name: String, into: File,
                           progress: (Long, Long) -> Unit): Long {
        val source = File(from, name)
        val target = File(into, name)
        if (!target.exists()) return ProtectedStorage.move(from, name, into, progress)
        var moved = 0L
        for (child in source.listFiles()?.sortedBy { it.name } ?: emptyList()) {
            if (child.isDirectory) moved += moveMerged(source, child.name, target, progress)
            else {
                val twin = File(target, child.name)
                child.copyTo(twin, overwrite = true)
                if (twin.length() != child.length()) throw java.io.IOException("${child.name} did not copy whole")
                moved += child.length()
                child.delete()
            }
        }
        if (source.listFiles()?.isEmpty() != false) source.delete()
        return moved
    }

    /** Move everything found outside protected storage into it.
     * `progress(folder, bytesDone, bytesTotal)` follows the copy.
     * -> the folders moved. */
    fun migrate(ctx: Context, progress: ((String, Long, Long) -> Unit)? = null): List<String> =
        synchronized(migrateLock) {
            val moved = ArrayList<String>()
            migrating = true
            try {
                for ((root, name) in pendingMoves(ctx)) {
                    val into = dataRoot(ctx)
                    into.mkdirs()
                    val bytes = ProtectedStorage.size(File(root, name))
                    val free = try { StatFs(into.absolutePath).availableBytes } catch (e: Exception) { -1L }
                    if (free in 0 until bytes + (64L shl 20)) {
                        migrationNote = "$name could not be moved into protected storage: " +
                            "it needs ${ZipImport.sizeText(bytes)} and this device has " +
                            "${ZipImport.sizeText(free)} free. It will speak, but not before the " +
                            "phone is unlocked."
                        Log.w("OutspokenEngine", migrationNote!!)
                        continue
                    }
                    try {
                        moveMerged(root, name, into) { d, t -> progress?.invoke(name, d, t) }
                        moved.add(name)
                        Log.i("OutspokenEngine", "moved $name (${ZipImport.sizeText(bytes)}) from $root " +
                            "into protected storage")
                    } catch (e: Exception) {
                        migrationNote = "$name could not be moved into protected storage: ${e.message}"
                        Log.w("OutspokenEngine", migrationNote!!, e)
                    }
                }
                if (moved.isNotEmpty()) {
                    refreshVoiceCatalogue()
                    migrationNote = "Moved ${moved.joinToString(", ")} into protected storage, " +
                        "so the voices can speak before the phone is unlocked."
                }
            } finally {
                migrating = false
            }
            moved
        }

    /** Move data in as soon as the phone is unlocked: now, or when the
     * unlock arrives.  The service calls this when it starts. */
    fun migrateWhenUnlocked(ctx: Context) {
        val app = ctx.applicationContext
        fun now() = Thread({
            try { migrate(app) } catch (e: Throwable) { Log.w("OutspokenEngine", "the move failed", e) }
        }, "outspoken-move").apply { priority = Thread.MIN_PRIORITY }.start()
        if (unlocked(app)) { now(); return }
        synchronized(migrateLock) {
            if (unlockWatcher != null) return
            val watcher = object : BroadcastReceiver() {
                override fun onReceive(c: Context, intent: Intent) {
                    try { app.unregisterReceiver(this) } catch (e: Exception) { /* already gone */ }
                    synchronized(migrateLock) { if (unlockWatcher === this) unlockWatcher = null }
                    now()
                }
            }
            unlockWatcher = watcher
            val filter = IntentFilter(Intent.ACTION_USER_UNLOCKED)
            if (Build.VERSION.SDK_INT >= 33) app.registerReceiver(watcher, filter, Context.RECEIVER_NOT_EXPORTED)
            else app.registerReceiver(watcher, filter)
        }
    }

    // ---- per-family settings -----------------------------------------------
    //
    // Read together once per utterance, after resolving its voice.  A key
    // without a family suffix is the fallback for every family, which is
    // what the sliders write when "for every engine" is chosen.

    fun settingKey(key: String, fam: String) = "${key}_$fam"
    const val VOLUME_SYSTEM_DEFAULT = -1
    fun volumeLevels(saved: Int): List<Int> =
        (listOf(VOLUME_SYSTEM_DEFAULT) + (0..100 step 5) + listOf(saved.coerceIn(-1, 100))).distinct().sorted()

    data class Settings(val rate: Int = 50, val pitch: Int = 50, val inflection: Int = 50,
                        val volume: Int = VOLUME_SYSTEM_DEFAULT, val numbers: String = NUMBERS_DEFAULT) {
        fun engineVolume() = if (volume < 0) 100 else volume
        val numbersMode: Int get() = when (numbers) { "off" -> 0; "digits" -> 2; else -> 1 }
        /** The requesting app's speech rate, 100 being normal, as the host
         * takes it: a percentage on top of the slider. */
        fun ratePercent(requestRate: Int): Int = if (requestRate <= 0) 100 else requestRate.coerceIn(10, 400)
    }

    fun settings(ctx: Context, fam: String = activeFamily(ctx)): Settings {
        val values = prefs(ctx).all
        fun value(key: String): Any? = values[settingKey(key, fam)] ?: values[key]
        return Settings(
            ((value(PREF_RATE) as? Int) ?: 50).coerceIn(0, 100),
            ((value(PREF_PITCH) as? Int) ?: 50).coerceIn(0, 100),
            ((value(PREF_INFLECTION) as? Int) ?: 50).coerceIn(0, 100),
            ((value(PREF_VOLUME) as? Int) ?: VOLUME_SYSTEM_DEFAULT).coerceIn(-1, 100),
            (value(PREF_NUMBERS) as? String)?.takeIf { it in NUMBERS_VALUES } ?: NUMBERS_DEFAULT)
    }

    // ---- synthesis ---------------------------------------------------------

    private val lock = Any()
    private val cancellations = java.util.concurrent.Executors.newSingleThreadExecutor { work ->
        Thread(work, "outspoken-cancel")
    }
    @Volatile private var request: OutspokenRequest<IOutspokenWorker>? = null
    @Volatile private var rate = 22254
    fun isOpen(): Boolean = worker != null
    @Volatile private var worker: IOutspokenWorker? = null

    /** Own the engine for one utterance (or one preview).  A cancel that
     * lands while this runs stops the render in place and abandons the
     * rest; the worker is kept, because it can be. */
    fun <T> withSynthesis(block: () -> T): T = synchronized(lock) {
        val owned = OutspokenRequest<IOutspokenWorker>(
            retire = OutspokenWorkers::retire,
            canReuse = { api, _ ->
                try { api.cancel(); true } catch (e: Exception) {
                    Log.w("OutspokenEngine", "cancel did not reach the worker; retiring it", e); false
                }
            },
            dispatchCancel = { cancellations.execute(it) })
        check(request == null) { "Nested synthesis request" }
        request = owned
        try { block() } finally {
            try { owned.current()?.cancel() } catch (e: Exception) {
                Log.w("OutspokenEngine", "Engine cancel at the end failed", e)
            } finally { owned.complete(); request = null }
        }
    }

    private fun open(ctx: Context, voice: VoiceInfo): IOutspokenWorker {
        val next = OutspokenWorkers.get(ctx)
        val rc = next.useVoice(rootsArgument(ctx), voice.hostId)
        check(rc == 0) { "Engine could not open ${voice.hostId}: $rc" }
        worker = next
        rate = 22254
        return next
    }

    /** Open the engine of the default voice ahead of the first request. */
    fun warmUp(ctx: Context) {
        val voice = defaultVoice(ctx) ?: return
        synchronized(lock) { open(ctx, voice) }
    }

    /** Preview uses the same streaming path and settings as platform speech. */
    fun render(ctx: Context, voice: VoiceInfo, text: String, ratePercent: Int = 100): ShortArray? = withSynthesis {
        try {
            val snapshot = settings(ctx, voice.family)
            val chunks = ArrayList<ShortArray>()
            var total = 0
            val buffer = ShortArray(4096)
            for (piece in OutspokenText.pieces(text)) {
                val started = speakStart(ctx, voice, OutspokenText.bytes(piece), ratePercent, snapshot)
                check(started >= 0)
                if (started != 0) continue
                while (true) {
                    val count = pull(buffer)
                    check(count >= 0)
                    if (count == 0) break
                    chunks.add(buffer.copyOf(count)); total += count
                }
            }
            ShortArray(total).also { result ->
                var offset = 0
                for (chunk in chunks) { chunk.copyInto(result, offset); offset += chunk.size }
            }
        } catch (e: Exception) {
            Log.e("OutspokenEngine", "Preview failed", e); null
        }
    }

    /** Begin an utterance on the worker: 0 when there is audio to pull, 1
     * when the text had nothing to say, negative on failure. */
    fun speakStart(ctx: Context, voice: VoiceInfo, utf8: ByteArray, ratePercent: Int,
                   snapshot: Settings = settings(ctx, voice.family)): Int = synchronized(lock) {
        try {
            val owned = checkNotNull(request) { "Speech requires withSynthesis" }
            val next = open(ctx, voice)
            // A stop during open stays attached to this request. It cannot be
            // lost in the interval before the worker marks itself active.
            if (!owned.attach(next)) return@synchronized -1
            next.settings(snapshot.rate, snapshot.pitch, snapshot.inflection, snapshot.engineVolume(),
                          snapshot.numbersMode, ratePercent)
            next.start(utf8).also { if (it == 0) owned.started() }
        } catch (e: Exception) {
            Log.e("OutspokenEngine", "Speech failed", e); -1
        }
    }

    fun pull(out: ShortArray): Int { return try {
        val bytes = request?.current()?.pull(out.size) ?: return -1
        for (i in 0 until bytes.size / 2)
            out[i] = ((bytes[i*2].toInt() and 255) or (bytes[i*2+1].toInt() shl 8)).toShort()
        bytes.size / 2
    } catch (e: Exception) { -1 } }

    /** Interrupt.  The stop flag first, straight at the worker from this
     * thread, so a render in progress returns early; then the cancel, which
     * abandons what is left and keeps the worker. */
    fun stop() {
        val current = request ?: return
        try { current.current()?.stop() } catch (e: Exception) { /* dead or gone; cancel handles it */ }
        current.cancel()
    }
    fun sampleRate(): Int = rate
}
