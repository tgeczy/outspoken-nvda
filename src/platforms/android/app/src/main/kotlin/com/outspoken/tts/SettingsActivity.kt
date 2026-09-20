package com.outspoken.tts

import android.app.Activity
import android.content.ActivityNotFoundException
import android.content.Intent
import android.net.Uri
import android.os.StatFs
import android.media.AudioAttributes
import android.media.AudioFormat
import android.media.AudioTrack
import android.os.Build
import android.os.Bundle
import android.provider.Settings
import android.text.InputType
import android.util.Log
import java.io.File
import java.io.FileOutputStream
import android.text.method.ScrollingMovementMethod
import android.view.Gravity
import android.view.View
import android.view.ViewGroup
import android.view.WindowInsets
import android.widget.Button
import android.widget.EditText
import android.widget.LinearLayout
import android.widget.RadioButton
import android.widget.RadioGroup
import android.widget.ScrollView
import android.widget.SeekBar
import android.widget.TextView
import android.widget.Toast

class SettingsActivity : Activity() {

    private lateinit var status: TextView
    private lateinit var importStatus: TextView
    private var engineHolder: LinearLayout? = null
    private var importDialog: android.app.AlertDialog? = null
    private var importBar: android.widget.ProgressBar? = null
    private var importMessage: TextView? = null
    private var importAnnounced = -1
    private lateinit var voicesView: TextView
    private lateinit var testButton: Button
    private lateinit var sampleText: EditText
    private lateinit var setupTab: Button
    private lateinit var engineTab: Button
    private var voiceHolder: LinearLayout? = null
    private var pageHolders: List<View> = emptyList()
    private var pad = 0
    private var rateSlider: ValueSlider? = null
    private var pitchSlider: ValueSlider? = null
    private var volumeSlider: ValueSlider? = null
    private var inflectionSlider: ValueSlider? = null
    private var volumeLevels = OutspokenEngine.volumeLevels(OutspokenEngine.VOLUME_SYSTEM_DEFAULT)
    private var numberChoice: Choice? = null
    private var overrideVoice: android.widget.CheckBox? = null
    private var voiceLabel: TextView? = null
    private var voiceSignature = ""
    private var voiceButton: Button? = null
    private var filterButton: Button? = null
    private var voiceFilter = FILTER_ALL
    private var listedVoices: List<OutspokenEngine.VoiceInfo> = emptyList()
    private var currentPage = 0
    private var moveStatus: TextView? = null
    private var moveAnnounced = -1
    private var updateStatus: TextView? = null
    private val preferenceListener = android.content.SharedPreferences.OnSharedPreferenceChangeListener { _, key ->
        if (key != "sample_text" && key != "settings_page" && key != PREF_VOICE_FILTER)
            refreshSettings(key == OutspokenEngine.PREF_FAMILY || key?.startsWith("default_voice") == true)
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        pad = (16 * resources.displayMetrics.density).toInt()

        val setupPage = column().also { buildSetup(it) }
        val enginePage = column().also { buildEngine(it) }

        // The tab strip is two ordinary buttons rather than a TabHost.
        //
        // A TabHost is more idiomatic, but this app also runs on a round watch
        // screen where a tab bar is nearly untappable -- and TalkBack reads a
        // selected/unselected button pair perfectly well, which is the audience
        // that matters most here.
        setupTab = Button(this).apply { text = "Setup"; setOnClickListener { show(0) } }
        engineTab = Button(this).apply {
            text = "Engine settings"; setOnClickListener { show(1) }
        }
        val wide = resources.configuration.screenWidthDp >= 340
        val tabs = LinearLayout(this).apply {
            orientation = if (wide) LinearLayout.HORIZONTAL else LinearLayout.VERTICAL
            addView(setupTab, tabParams(wide))
            addView(engineTab, tabParams(wide))
        }

        val pages = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            addView(ScrollView(this@SettingsActivity).apply { addView(setupPage) })
            addView(ScrollView(this@SettingsActivity).apply { addView(enginePage) })
        }
        pageHolders = listOf(pages.getChildAt(0), pages.getChildAt(1))

        val root = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            addView(tabs)
            addView(pages)
        }

        // Android 15 draws every activity edge to edge whether it asked to or
        // not.  Without this the tab strip is laid out at y=0 -- underneath
        // the status bar, invisible and untappable -- and the last button on
        // the setup page disappears under the navigation bar.  Found by eye
        // in the sibling app.
        root.setOnApplyWindowInsetsListener { view, insets ->
            val bars = systemBarInsets(insets)
            view.setPadding(bars[0], bars[1], bars[2], bars[3])
            insets
        }

        setContentView(root)
        show(savedInstanceState?.getInt("page") ?: OutspokenEngine.prefs(this).getInt("settings_page", 0))
        OutspokenEngine.prefs(this).registerOnSharedPreferenceChangeListener(preferenceListener)
        refresh()

        // Test hook: `am start ... --ez autospeak true` checks the engine and
        // speaks a sample, so a render can be triggered without navigating to
        // the button. Harmless in normal use (the extra is never set).
        if (intent?.getBooleanExtra("autospeak", false) == true) {
            Thread {
                OutspokenEngine.checkEngine(this)
                runOnUiThread { refresh(); testSpeak() }
            }.start()
        }

        // An import already running -- this screen was rebuilt underneath it
        // -- shows its progress again. Otherwise, the adb route: `am start
        // ... --es import /path/to/outspoken.zip` (or a content: URI) imports
        // without the confirm dialog; typing the command is the consent.
        importJob?.let { attachImport(it) } ?: intent?.getStringExtra("import")?.let { arg ->
            val source = if (arg.startsWith("content:") || arg.startsWith("file:"))
                ZipImport.source(this, Uri.parse(arg)) else ZipImport.source(File(arg))
            importZip(source, confirm = false)
        }
    }

    override fun onResume() { super.onResume(); refresh(); refreshSettings(true); moveDataIn() }
    override fun onDestroy() {
        OutspokenEngine.prefs(this).unregisterOnSharedPreferenceChangeListener(preferenceListener)
        importJob?.listener = null
        importDialog?.dismiss(); importDialog = null
        super.onDestroy()
    }

    override fun onActivityResult(requestCode: Int, resultCode: Int, data: Intent?) {
        super.onActivityResult(requestCode, resultCode, data)
        if (requestCode != REQUEST_ZIP) return
        val uri = data?.data
        if (resultCode != RESULT_OK || uri == null) { importStatus.text = "No zip chosen."; return }
        importZip(ZipImport.source(this, uri), confirm = true)
    }

    // ---- importing a zip ----------------------------------------------------

    /** Open the system's file picker on a zip. A watch answers the intent
     * with a stub that does nothing but say so; better to say so here. */
    private fun pickZip() {
        if (importJob != null) { toast("An import is already running."); return }
        val intent = Intent(Intent.ACTION_OPEN_DOCUMENT).apply {
            addCategory(Intent.CATEGORY_OPENABLE)
            type = "*/*"
            putExtra(Intent.EXTRA_MIME_TYPES, arrayOf("application/zip",
                "application/x-zip-compressed", "application/octet-stream"))
        }
        // The manifest's <queries> lets this app see who answers; without it,
        // Android 11's package visibility hides the picker from the question
        // and a phone with a perfectly good one is told it has none. A null
        // here is therefore "could not tell", and the picker is tried anyway;
        // only the Wear stub is refused up front.
        val handler = packageManager.resolveActivity(intent, 0)
        if (handler?.activityInfo?.packageName == WEAR_STUB) { noPicker(); return }
        try { startActivityForResult(intent, REQUEST_ZIP) }
        catch (e: ActivityNotFoundException) { noPicker() }
    }

    private fun noPicker() {
        android.app.AlertDialog.Builder(this)
            .setTitle("No file picker on this device")
            .setMessage("This device has no file picker, so there is no way to choose a " +
                "zip on it. A phone or tablet has one; a watch does not.")
            .setPositiveButton("Close", null).show()
    }

    /** Read what the zip holds, say what will happen, and -- with `confirm`,
     * once OK is pressed -- do it. */
    private fun importZip(source: ZipImport.Source, confirm: Boolean) {
        if (importJob != null) { toast("An import is already running."); return }
        importStatus.text = "Checking ${source.name}…"
        val checking = android.app.AlertDialog.Builder(this)
            .setTitle("Checking zip")
            .setMessage("Reading what is inside ${source.name}…")
            .setCancelable(false).create().also { it.show() }
        Thread {
            val plan = try { ZipImport.inspect(source) }
                catch (e: Exception) { ZipImport.Plan(emptyList(), "The zip could not be read: ${e.message}") }
            runOnUiThread {
                checking.dismiss()
                if (isFinishing || isDestroyed) return@runOnUiThread
                val root = OutspokenEngine.dataRoot(this)
                val refusal = plan.refusal ?: run {
                    root.mkdirs()
                    val free = try { StatFs(root.absolutePath).availableBytes } catch (e: Exception) { -1L }
                    if (free in 0 until plan.bytes + (64L shl 20))
                        "Not enough room: the engine data unpacks to ${ZipImport.sizeText(plan.bytes)} " +
                        "and this device has ${ZipImport.sizeText(free)} free."
                    else null
                }
                if (refusal != null) {
                    importStatus.text = "Not imported."
                    android.app.AlertDialog.Builder(this)
                        .setTitle("Cannot import ${source.name}").setMessage(refusal)
                        .setPositiveButton("Close", null).show()
                    return@runOnUiThread
                }
                val lines = plan.found.map { f ->
                    "Will import ${f.label}: ${f.files} files, ${ZipImport.sizeText(f.bytes)}." +
                    when {
                        f.unit == ZipImport.VOICES && File(root, f.unit).exists() ->
                            " A voice folder already here with the same name will be replaced; the others stay."
                        File(root, f.unit).exists() -> " The ${f.label} folder already here will be replaced."
                        else -> ""
                    }
                }
                val message = lines.joinToString("\n\n") +
                    "\n\nInto this app's protected storage, where the voices can speak " +
                    "before the phone is unlocked."
                if (!confirm) { startImport(source, plan, root); return@runOnUiThread }
                android.app.AlertDialog.Builder(this)
                    .setTitle("Import engine data?").setMessage(message)
                    .setPositiveButton("OK") { _, _ -> startImport(source, plan, root) }
                    .setNegativeButton("Cancel") { _, _ -> importStatus.text = "Not imported." }
                    .show()
            }
        }.start()
    }

    private fun startImport(source: ZipImport.Source, plan: ZipImport.Plan, root: File) {
        val job = ImportJob(source, plan, root)
        importJob = job
        attachImport(job)
        job.start()
    }

    private fun importNames(job: ImportJob) = job.plan.found.joinToString(" and ") { it.label }

    /** Show a running import's progress on this screen, whichever screen
     * instance this is. */
    private fun attachImport(job: ImportJob) {
        importDialog?.dismiss()
        val names = importNames(job)
        val box = column()
        importMessage = body("Importing $names…").also { box.addView(it) }
        importBar = android.widget.ProgressBar(this, null, android.R.attr.progressBarStyleHorizontal).apply {
            max = 1000
            isIndeterminate = job.total <= 0 && job.source.size <= 0
            contentDescription = "Import progress"
            box.addView(this)
        }
        box.addView(body("Keep the app open until it finishes."))
        importDialog = android.app.AlertDialog.Builder(this)
            .setTitle("Importing engine data").setView(box).setCancelable(false)
            .setNegativeButton("Cancel") { _, _ -> job.cancelled = true; importStatus.text = "Cancelling…" }
            .create().also { it.show() }
        importAnnounced = -1
        window.addFlags(android.view.WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        job.listener = { j -> runOnUiThread { showImportProgress(j) } }
        showImportProgress(job)
    }

    private fun showImportProgress(job: ImportJob) {
        if (isFinishing || isDestroyed || importJob !== job) return
        if (job.result != null) { finishImport(job); return }
        val names = importNames(job)
        val total = if (job.total > 0) job.total else job.source.size
        if (total > 0) {
            val permille = (job.done * 1000 / total).toInt().coerceIn(0, 1000)
            importBar?.isIndeterminate = false
            importBar?.progress = permille
            importMessage?.text = "Importing $names… ${permille / 10}%"
            // A screen reader hears the dialog once; the percentage moving is
            // silent unless said. Every fifth of the way is enough.
            val fifth = permille / 200
            if (fifth != importAnnounced && fifth in 1..4) {
                importAnnounced = fifth
                try { importMessage?.announceForAccessibility("${fifth * 20} percent") } catch (e: Throwable) {}
            }
        } else importMessage?.text = "Importing $names… ${ZipImport.sizeText(job.done)} so far"
    }

    private fun finishImport(job: ImportJob) {
        job.listener = null
        importJob = null
        importDialog?.dismiss(); importDialog = null
        window.clearFlags(android.view.WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        val result = job.result ?: return
        result.onSuccess { units ->
            OutspokenEngine.imported(this)
            refresh(); rebuildVoices(); refreshVoiceButton(); rebuildEngineSwitches()
            val said = units.joinToString(" and ") { ZipImport.unitLabel(it) }
            val voices = OutspokenEngine.allVoices(this).size
            importStatus.text = "Imported $said; $voices voice(s) speak now. " +
                if (OutspokenEngine.verified(this)) "The engine is verified and ready."
                else "Now press Check Engine."
            try { importStatus.announceForAccessibility(importStatus.text) } catch (e: Throwable) {}
        }.onFailure { e ->
            if (e is ZipImport.Cancelled) {
                importStatus.text = "Import cancelled. Nothing was changed."
            } else {
                Log.e("Outspoken", "import failed", e)
                importStatus.text = "Import failed. Nothing was changed."
                android.app.AlertDialog.Builder(this)
                    .setTitle("Import failed")
                    .setMessage(e.message ?: e.toString())
                    .setPositiveButton("Close", null).show()
            }
        }
    }

    /** An import in flight, held outside the activity so a screen rebuilt
     * underneath it -- a rotation, a watch going dark and back -- finds it
     * again and shows its progress. One at a time. */
    private class ImportJob(val source: ZipImport.Source, val plan: ZipImport.Plan, val root: File) {
        @Volatile var done = 0L
        @Volatile var total = -1L
        @Volatile var cancelled = false
        @Volatile var result: Result<List<String>>? = null
        @Volatile var listener: ((ImportJob) -> Unit)? = null
        private var lastStep = -1L
        fun start() = Thread {
            result = runCatching {
                ZipImport.extract(source, plan, root, { d, t ->
                    done = d; total = t
                    // Every 64 KB is too often to redraw; every 0.2 percent,
                    // or every 4 MB when the size is unknown, is not.
                    val step = if (t > 0) d * 500 / t else d shr 22
                    if (step != lastStep) { lastStep = step; listener?.invoke(this) }
                }) { cancelled }
            }
            listener?.invoke(this)
        }.start()
    }
    override fun onSaveInstanceState(out: Bundle) {
        out.putInt("page", currentPage)
        super.onSaveInstanceState(out)
    }
    private fun refreshSettings(voiceSelection: Boolean = false) {
        overrideVoice?.isChecked = OutspokenEngine.prefs(this).getBoolean("override_voice", true)
        val settings = OutspokenEngine.settings(this)
        rateSlider?.progress = settings.rate
        pitchSlider?.progress = settings.pitch
        volumeLevels = OutspokenEngine.volumeLevels(settings.volume)
        volumeSlider?.max = volumeLevels.lastIndex
        volumeSlider?.progress = volumeLevels.indexOf(settings.volume)
        volumeSlider?.refreshValue()
        inflectionSlider?.progress = settings.inflection
        numberChoice?.select(OutspokenEngine.NUMBERS_VALUES.indexOf(settings.numbers))
        if (voiceSelection) refreshVoiceButton()
    }

    /** Left, top, right and bottom taken by the status and navigation bars.
     *
     * `WindowInsets.Type` arrived in API 30 and this app supports 26, so the
     * older accessors stay for the watch and anything else on an early
     * release.  Both report the same rectangle.
     */
    @Suppress("DEPRECATION")
    private fun systemBarInsets(insets: WindowInsets): IntArray =
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            val bars = insets.getInsets(WindowInsets.Type.systemBars())
            intArrayOf(bars.left, bars.top, bars.right, bars.bottom)
        } else {
            intArrayOf(insets.systemWindowInsetLeft, insets.systemWindowInsetTop,
                       insets.systemWindowInsetRight, insets.systemWindowInsetBottom)
        }

    private fun show(page: Int) {
        currentPage = page.coerceIn(0, 1)
        OutspokenEngine.prefs(this).edit().putInt("settings_page", currentPage).apply()
        pageHolders.forEachIndexed { i, v ->
            v.visibility = if (i == page) View.VISIBLE else View.GONE
        }
        setupTab.isSelected = page == 0
        engineTab.isSelected = page == 1
        // Say which page this is, and say it out loud. A sighted user sees the
        // content swap; a screen-reader user whose focus is still on the tab
        // button hears nothing at all unless it is announced.
        setupTab.contentDescription = "Setup, tab 1 of 2"
        engineTab.contentDescription = "Engine settings, tab 2 of 2"
        val name = if (page == 0) "Setup" else "Engine settings"
        try { window.decorView.announceForAccessibility("$name page") }
        catch (e: Throwable) { /* announcing is a courtesy, never a failure */ }
    }

    // ---- page 1: setup -----------------------------------------------------

    private fun buildSetup(root: LinearLayout) {
        root.addView(TextView(this).apply {
            text = "outSPOKEN"; textSize = 26f; gravity = Gravity.CENTER
        })
        root.addView(body("The classic Macintosh voices -- MacinTalk 1984, MacinTalk 2, " +
            "MacinTalk 3 and MacinTalk Pro in English and Spanish -- running under emulation."))

        // The positioning, said once and plainly.
        //
        // Extraction lives on the desktop, where the extractor and the disk
        // images are. Without saying so, the first thing anybody asks is how to
        // point this at a disk image on their phone -- which it will never do.
        root.addView(body(
            "\nThis app is the companion to outSPOKEN on your desktop. Extract " +
            "the engine data there, from your own copy of the software, then copy " +
            "the finished folder here. Nothing of Apple's or Berkeley's ships with " +
            "this app, and extraction does not happen on a phone or a watch."))

        root.addView(heading("1.  Put engine data here"))
        // The zip is the route that works everywhere a file can be chosen:
        // one file through the system picker, from Downloads or a cloud
        // drive, and the app reads what is inside rather than trusting the
        // zip's name.
        root.addView(body(
            "Zip the outspoken folder the desktop add-on extracted -- it holds " +
            "macintalk1, macintalk2, macintalk3, macintalkpro, macintalkespanol and " +
            "voices, or whichever of them you have -- get the zip onto this device, " +
            "and choose it here. Those folders can be at the top of the zip, inside " +
            "outspoken or outspoken-data, or inside macintalk/outspoken as they sit " +
            "on the desktop, in any casing. The app checks what is inside before " +
            "unpacking it into place, and a zip with only new voices adds them."))
        root.addView(Button(this).apply {
            text = "Extract engine from zip file"
            setOnClickListener { pickZip() }
        })
        importStatus = body("")
        root.addView(importStatus)
        root.addView(body("\nOr copy the extracted folder by hand into:"))
        root.addView(TextView(this).apply {
            textSize = 13f
            setTextIsSelectable(true)
            text = OutspokenEngine.inboxRoot(this@SettingsActivity)?.absolutePath
                ?: "Android/data/com.outspoken.tts/files/${OutspokenEngine.DATA_DIR}"
        })
        root.addView(body(
            "the outspoken folder as it is, or its contents. On a PC, plug the " +
            "phone in and use the file window; the folder above is under Android, " +
            "then data, then this app."))
        // Where the data really lives, said once: the folder above is an
        // inbox.  Somebody who copied it over and then finds the folder
        // empty deserves to have been told why before it happened.
        root.addView(body(
            "\nWhatever you put there is moved into this app's protected storage " +
            "the next time the app runs, so the voices can speak on the lock " +
            "screen after a restart, before the phone is unlocked. The folder " +
            "above is empty again afterwards; that is the move, not a loss."))
        moveStatus = body("").also { root.addView(it) }

        root.addView(heading("2.  Check the engine"))
        root.addView(Button(this).apply {
            text = "Check Engine"
            setOnClickListener { runCheck() }
        })
        status = body("Not checked yet.")
        status.movementMethod = ScrollingMovementMethod()
        root.addView(status)

        root.addView(heading("Voices found"))
        voicesView = body("—")
        root.addView(voicesView)

        root.addView(heading("3.  Try it"))
        // An edit field rather than a fixed sentence: the point of a preview is
        // to hear the voice say the kind of thing you are about to make it say
        // for hours, and that sentence is never the one that shipped.
        val sampleLabel = body("Text to speak").also { root.addView(it) }
        sampleText = EditText(this).apply {
            id = View.generateViewId()
            inputType = InputType.TYPE_CLASS_TEXT or InputType.TYPE_TEXT_FLAG_MULTI_LINE
            setText(OutspokenEngine.prefs(this@SettingsActivity).getString("sample_text",
                "Hello there. This is outSPOKEN. You owe 1,234 dollars."))
            addTextChangedListener(object : android.text.TextWatcher {
                override fun beforeTextChanged(s: CharSequence?, start: Int, count: Int, after: Int) {}
                override fun onTextChanged(s: CharSequence?, start: Int, before: Int, count: Int) {
                    OutspokenEngine.prefs(this@SettingsActivity).edit().putString("sample_text", s.toString()).apply()
                }
                override fun afterTextChanged(s: android.text.Editable?) {}
            })
            textSize = 15f
        }
        sampleLabel.labelFor = sampleText.id
        root.addView(sampleText)
        testButton = Button(this).apply {
            text = "Speak"
            isEnabled = false
            setOnClickListener { testSpeak() }
        }
        root.addView(testButton)

        root.addView(body(
            "\nThen pick outSPOKEN as your engine in the system's " +
            "Text-to-speech settings:"))
        root.addView(Button(this).apply {
            text = "Open Text-to-speech settings"
            setOnClickListener { openTtsSettings() }
        })

        root.addView(heading("Updates"))
        // Only when pressed; the app never asks the network on its own.  And
        // the answer is a download in the browser rather than an install,
        // because installing would need the "install unknown apps" switch
        // for this app, which is exactly the permission Google leans on
        // third-party apps for asking.
        root.addView(body(
            "Looks for a newer version on the project's release page. The " +
            "download opens in your browser; once it has finished, opening it " +
            "from the download notification installs it."))
        root.addView(Button(this).apply {
            text = "Check for updates"
            setOnClickListener { checkForUpdates() }
        })
        updateStatus = body("").also { root.addView(it) }

        root.addView(Button(this).apply {
            text = "Licenses and source"
            setOnClickListener {
                val notice = assets.open("DISTRIBUTION.txt").bufferedReader().use { it.readText() }
                android.app.AlertDialog.Builder(this@SettingsActivity)
                    .setTitle("Licenses and source").setMessage(notice)
                    .setPositiveButton("Close", null)
                    .setNeutralButton("Full licenses") { _, _ ->
                        val files = assets.list("").orEmpty()
                            .filter { it.endsWith(".txt") && it != "DISTRIBUTION.txt" }.sorted()
                        android.app.AlertDialog.Builder(this@SettingsActivity)
                            .setTitle("Full licenses")
                            .setItems(files.map { it.removeSuffix(".txt").replace('-', ' ') }.toTypedArray()) { _, which ->
                                val license = assets.open(files[which]).bufferedReader().use { it.readText() }
                                android.app.AlertDialog.Builder(this@SettingsActivity)
                                    .setTitle(files[which].removeSuffix(".txt").replace('-', ' '))
                                    .setMessage(license).setPositiveButton("Close", null).show()
                            }.setNegativeButton("Close", null).show()
                    }.show()
            }
        })
    }

    // ---- page 2: engine settings -------------------------------------------

    private fun buildEngine(root: LinearLayout) {
        val p = OutspokenEngine.prefs(this)

        voiceLabel = heading("Voice").also { root.addView(it) }
        root.addView(body("Choosing a voice also chooses its engine. The settings below " +
            "belong to that engine; each engine remembers its own."))
        voiceFilter = p.getString(PREF_VOICE_FILTER, null) ?: OutspokenEngine.activeFamily(this)
        voiceHolder = LinearLayout(this).apply { orientation = LinearLayout.VERTICAL }
        root.addView(voiceHolder!!)
        rebuildVoices()
        // On by default. A screen reader never names a voice of its own: it
        // asks this engine for a default once, when it connects, and sends
        // that name with every request from then on. With this off, choosing
        // a voice here changed nothing until the engine was restarted.
        root.addView(android.widget.CheckBox(this).apply {
            text = "Use selected voice in all apps"
            isChecked = p.getBoolean("override_voice", true)
            overrideVoice = this
            setOnCheckedChangeListener { _, checked ->
                if (checked != p.getBoolean("override_voice", true))
                    p.edit().putBoolean("override_voice", checked).apply()
            }
        })
        root.addView(body(
            "Every app hears the voice chosen above, straight away. Turn this " +
            "off only for an app that picks a voice of its own. A request in " +
            "Spanish gets Carlos or Catalina either way, when they are here."))

        // Which of the installed engines to offer at all. Data can be
        // present and unwanted, and an unwanted engine must not turn up in a
        // voice list a screen reader reads out forty entries at a time. Off
        // hides it everywhere at once and leaves its folder alone.
        root.addView(heading("Engines"))
        root.addView(body("Switch off an engine you do not want. Its voices leave " +
            "every list at once; its data stays where it is. An engine marked " +
            "not installed has no data yet."))
        engineHolder = LinearLayout(this).apply { orientation = LinearLayout.VERTICAL }
        root.addView(engineHolder!!)
        rebuildEngineSwitches()

        root.addView(heading("Rate"))
        root.addView(body(
            "The same slider as the desktop add-on: 50 is the voice's own " +
            "speed. The rate an app asks for -- the system's text-to-speech " +
            "speed, or a screen reader's own -- is applied on top of it."))
        rateSlider = addSlider(root, "Speech rate", 100, OutspokenEngine.settings(this).rate,
            { if (it == 50) "50, the voice's own speed" else "$it" }) {
            p.edit().putInt(OutspokenEngine.settingKey(OutspokenEngine.PREF_RATE, OutspokenEngine.activeFamily(this)), it).apply()
        }

        root.addView(heading("Pitch"))
        root.addView(body("50 is the voice as it was recorded; the ends are an octave " +
            "either way. Applies from the next request."))
        pitchSlider = addSlider(root, "Pitch", 100, OutspokenEngine.settings(this).pitch,
            { if (it == 50) "50, as recorded" else "$it" }) {
            p.edit().putInt(OutspokenEngine.settingKey(OutspokenEngine.PREF_PITCH, OutspokenEngine.activeFamily(this)), it).apply()
        }

        root.addView(heading("Inflection"))
        root.addView(body("How much the pitch moves within a sentence. 50 is the voice's " +
            "own; the 1984 driver has no inflection to change. Applies from the " +
            "next request."))
        inflectionSlider = addSlider(root, "Inflection", 100, OutspokenEngine.settings(this).inflection,
            { if (it == 50) "50 percent, the voice's own" else "$it percent" }) {
            p.edit().putInt(OutspokenEngine.settingKey(OutspokenEngine.PREF_INFLECTION,
                OutspokenEngine.activeFamily(this)), it).apply()
        }

        root.addView(heading("Volume"))
        root.addView(body("System default plays the engine at full level while Android " +
            "controls app and device volume. Choose 0 to mute, or adjust in 5 percent steps."))
        val savedVolume = OutspokenEngine.settings(this).volume
        volumeLevels = OutspokenEngine.volumeLevels(savedVolume)
        volumeSlider = addSlider(root, "Engine volume", volumeLevels.lastIndex,
            volumeLevels.indexOf(savedVolume), { volumeText(volumeLevels[it]) }) {
            p.edit().putInt(OutspokenEngine.settingKey(OutspokenEngine.PREF_VOLUME,
                OutspokenEngine.activeFamily(this)), volumeLevels[it]).apply()
        }

        val numberLabel = heading("Numbers").also { root.addView(it) }
        root.addView(body(
            "How a number is read, in English or in Spanish as the voice speaks. " +
            "“In words” says 1,234 as one thousand two hundred thirty-four; " +
            "“digit by digit” says the digits; “as the engine reads it” leaves " +
            "the number to the engine, which stops at six digits."))
        val numberNames = listOf("In words (recommended)", "Digit by digit", "As the engine reads it")
        val current = OutspokenEngine.settings(this).numbers
        numberChoice = choice("How to read numbers", numberNames,
            OutspokenEngine.NUMBERS_VALUES.indexOf(current).coerceAtLeast(0)) { i ->
            val value = OutspokenEngine.NUMBERS_VALUES[i]
            if (OutspokenEngine.settings(this).numbers != value)
                p.edit().putString(OutspokenEngine.settingKey(OutspokenEngine.PREF_NUMBERS,
                OutspokenEngine.activeFamily(this)), value).apply()
        }.also {
            numberLabel.labelFor = it.id
            root.addView(it)
        }
    }

    /** One switch per engine this app knows; rebuilt after an import,
     * because "not installed" stops being true the moment one lands. */
    private fun rebuildEngineSwitches() {
        val holder = engineHolder ?: return
        holder.removeAllViews()
        val installed = OutspokenEngine.installedFamilies(this)
        for (fam in OutspokenEngine.FAMILIES) {
            holder.addView(android.widget.CheckBox(this).apply {
                if (fam !in installed) {
                    text = "${OutspokenEngine.famLabel(fam)}: not installed"
                    isChecked = false
                    isEnabled = false
                    return@apply
                }
                text = OutspokenEngine.famLabel(fam)
                isChecked = OutspokenEngine.famOffered(this@SettingsActivity, fam)
                setOnCheckedChangeListener { box, checked ->
                    if (!OutspokenEngine.setFamOffered(this@SettingsActivity, fam, checked)) {
                        box.isChecked = true
                        android.widget.Toast.makeText(this@SettingsActivity,
                            "Keep at least one engine on.", android.widget.Toast.LENGTH_SHORT).show()
                        return@setOnCheckedChangeListener
                    }
                    rebuildVoices()
                    refreshVoiceButton()
                }
            })
        }
    }

    /** The listed voice that is in use, or -1 when none of them is.
     *
     * -1 matters: with the list filtered to an engine other than the one
     * speaking, nothing is the voice in use, and a radio button that says
     * "checked" of a voice nobody is hearing is a lie the ear cannot catch. */
    private fun preferredVoiceIndex(voices: List<OutspokenEngine.VoiceInfo>): Int {
        val chosen = OutspokenEngine.defaultVoice(this) ?: return -1
        return voices.indexOfFirst { it.hostId == chosen.hostId }
    }

    /** The button says which voice is in use, so it has to be re-read whenever
     * the choice changes -- from the dialog, or from another screen. */
    private fun refreshVoiceButton() {
        val button = voiceButton ?: return
        val voices = listedVoices
        val labelled = voiceFilter == FILTER_ALL && OutspokenEngine.availableFamilies(this).size > 1
        val at = preferredVoiceIndex(voices)
        val name = voices.getOrNull(at)?.let { if (labelled) it.label else it.name }
        button.text = "Voice: " + (name ?: "none")
    }

    private fun rebuildVoices() {
        val holder = voiceHolder ?: return
        holder.removeAllViews()
        val all = OutspokenEngine.allVoices(this)
        if (all.isEmpty()) {
            listedVoices = all
            filterButton = null
            voiceButton = null
            holder.addView(body("No voices in the engine data folder."))
            return
        }
        val fams = OutspokenEngine.availableFamilies(this)
        if (voiceFilter != FILTER_ALL && voiceFilter !in fams) voiceFilter = FILTER_ALL

        // Which engine's voices are listed. One button that opens a list,
        // not a column of radios: the engine is a filter on the choice below
        // it. With one engine present there is nothing to filter and it is
        // not shown.
        if (fams.size > 1) {
            val options = listOf(FILTER_ALL) + fams
            val names = listOf("All engines") + fams.map { OutspokenEngine.famLabel(it) }
            filterButton = Button(this).apply {
                id = View.generateViewId()
                text = "Engine: ${names[options.indexOf(voiceFilter).coerceAtLeast(0)]}"
                setOnClickListener {
                    val at = options.indexOf(voiceFilter).coerceAtLeast(0)
                    android.app.AlertDialog.Builder(this@SettingsActivity)
                        .setTitle("Engine")
                        .setSingleChoiceItems(names.toTypedArray(), at) { dialog, which ->
                            dialog.dismiss()
                            if (options[which] != voiceFilter) {
                                voiceFilter = options[which]
                                OutspokenEngine.prefs(this@SettingsActivity).edit()
                                    .putString(PREF_VOICE_FILTER, voiceFilter).apply()
                                rebuildVoices()
                            }
                        }
                        .setNegativeButton("Cancel", null)
                        .show()
                }
                holder.addView(this)
            }
        } else filterButton = null

        val voices = if (voiceFilter == FILTER_ALL) all else all.filter { it.family == voiceFilter }
        listedVoices = voices
        // Engines are named only when more than one is in the list.
        val labelled = voiceFilter == FILTER_ALL && fams.size > 1
        val names = voices.map { if (labelled) it.label else it.name }

        // A button that opens the list, not the list itself: thirty-six
        // voices inline were thirty-six stops to swipe past to reach the
        // rate slider below. In a dialog the whole choice is one stop, Back
        // leaves it, and the list scrolls on its own.
        voiceButton = Button(this).apply {
            id = View.generateViewId()
            val at = preferredVoiceIndex(voices)
            text = "Voice: " + (names.getOrNull(at) ?: "none")
            setOnClickListener {
                android.app.AlertDialog.Builder(this@SettingsActivity)
                    .setTitle("Voice")
                    .setSingleChoiceItems(names.toTypedArray(), preferredVoiceIndex(voices)) { dialog, which ->
                        dialog.dismiss()
                        val v = voices[which]
                        if (OutspokenEngine.defaultVoice(this@SettingsActivity)?.hostId != v.hostId)
                            OutspokenEngine.chooseVoice(this@SettingsActivity, v)
                        refreshVoiceButton()
                        refreshSettings()
                    }
                    .setNegativeButton("Cancel", null)
                    .show()
            }
            voiceLabel?.labelFor = id
            holder.addView(this)
        }
        holder.addView(body("The sample on the Setup page speaks with this one."))
    }

    // ---- small builders ----------------------------------------------------

    private fun column() = LinearLayout(this).apply {
        orientation = LinearLayout.VERTICAL
        setPadding(pad, pad, pad, pad)
    }
    private fun heading(t: String) = TextView(this).apply {
        text = t; textSize = 20f; setPadding(0, pad, 0, pad / 2)
        if (android.os.Build.VERSION.SDK_INT >= 28) isAccessibilityHeading = true
    }
    private fun body(t: String) = TextView(this).apply { text = t; textSize = 15f }
    private fun tabParams(wide: Boolean) =
        if (wide) LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f)
        else LinearLayout.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT)

    /** A set of radio buttons: one focus stop per option, and each option
     * says its own name and whether it is the one in use -- "Fred, radio
     * button, checked".  These were Spinners once; the stock RadioButton
     * owns its label, its focus and its checked state, and says each once. */
    private class Choice(context: android.content.Context) : RadioGroup(context) {
        private var quiet = false
        var onPick: ((Int) -> Unit)? = null

        /** The chosen option's index, or -1 when none is. */
        fun index(): Int = (0 until childCount).indexOfFirst { (getChildAt(it) as RadioButton).isChecked }

        /** Show an option as chosen without treating that as the user's doing:
         * a preference changes because somebody chose it, not because a screen
         * was built or refreshed. -1 clears the choice. */
        fun select(i: Int) {
            quiet = true
            try { if (i in 0 until childCount) check(getChildAt(i).id) else clearCheck() }
            finally { quiet = false }
        }

        /** RadioGroup.isEnabled does not reach its buttons; this does. */
        fun enableAll(enabled: Boolean) {
            isEnabled = enabled
            for (i in 0 until childCount) getChildAt(i).isEnabled = enabled
        }

        init {
            orientation = VERTICAL
            setOnCheckedChangeListener { _, checkedId ->
                if (quiet || checkedId == View.NO_ID) return@setOnCheckedChangeListener
                val i = (0 until childCount).indexOfFirst { getChildAt(it).id == checkedId }
                if (i >= 0) onPick?.invoke(i)
            }
        }
    }

    private fun choice(name: String, items: List<String>, selected: Int,
                       onPick: (Int) -> Unit) = Choice(this).apply {
        id = View.generateViewId()
        tag = name
        for (item in items) addView(RadioButton(this@SettingsActivity).apply {
            id = View.generateViewId()
            text = item
            textSize = 16f
        })
        select(selected)
        this.onPick = onPick
    }

    // View-based equivalent of TG Speechbox's AccessibleSlider: named value,
    // one-step arrows, Home/End, and the stock accessibility range actions.
    private class ValueSlider(context: android.content.Context, private val label: TextView,
                              private val name: String, private val describe: (Int) -> String) : SeekBar(context) {
        fun refreshValue() {
            label.text = describe(progress)
            if (android.os.Build.VERSION.SDK_INT >= 30) {
                contentDescription = name
                stateDescription = describe(progress)
            } else contentDescription = "$name, ${describe(progress)}"
        }
    }

    private fun volumeText(value: Int) = if (value < 0) "System default" else "$value percent"

    private fun addSlider(root: LinearLayout, name: String, limit: Int, value: Int,
            describe: (Int) -> String, save: (Int) -> Unit): ValueSlider {
        val label = body(describe(value)).apply { importantForAccessibility = View.IMPORTANT_FOR_ACCESSIBILITY_NO }
        root.addView(label)
        val slider = ValueSlider(this, label, name, describe).apply {
            id = View.generateViewId()
            max = limit
            progress = value
            keyProgressIncrement = 1
            refreshValue()
            setOnSeekBarChangeListener(object : SeekBar.OnSeekBarChangeListener {
                override fun onProgressChanged(s: SeekBar, v: Int, user: Boolean) {
                    refreshValue()
                    if (user) save(v)
                }
                override fun onStartTrackingTouch(s: SeekBar) {}
                override fun onStopTrackingTouch(s: SeekBar) {}
            })
            setOnKeyListener { _, key, event ->
                val target = when (key) {
                    android.view.KeyEvent.KEYCODE_DPAD_LEFT -> (progress - 1).coerceAtLeast(0)
                    android.view.KeyEvent.KEYCODE_DPAD_RIGHT -> (progress + 1).coerceAtMost(max)
                    android.view.KeyEvent.KEYCODE_MOVE_HOME -> 0
                    android.view.KeyEvent.KEYCODE_MOVE_END -> max
                    else -> return@setOnKeyListener false
                }
                if (event.action == android.view.KeyEvent.ACTION_DOWN) {
                    progress = target
                    save(target)
                }
                true
            }
        }
        label.labelFor = slider.id
        root.addView(slider)
        return slider
    }

    // ---- behaviour ---------------------------------------------------------

    private fun refresh() {
        // So the inbox exists for the person about to copy into it over a PC.
        try { OutspokenEngine.inboxRoot(this)?.mkdirs() } catch (e: Exception) { /* shared storage absent */ }
        val signature = OutspokenEngine.allVoices(this).joinToString { it.id }
        if (voiceSignature != signature) { voiceSignature = signature; rebuildVoices() }
        val verified = OutspokenEngine.verified(this)
        testButton.isEnabled = verified
        val voices = OutspokenEngine.allVoices(this)
        voicesView.text = if (voices.isEmpty()) "—"
            else voices.joinToString("\n") { "•  ${it.label}" }
        if (verified) status.text = "Engine verified: ${voices.size} voice(s) ready."
    }

    private fun runCheck() {
        status.text = "Checking…"
        Thread {
            val ok = OutspokenEngine.checkEngine(this)
            val skipped = OutspokenEngine.skippedFolders(this)
            runOnUiThread {
                status.text = if (ok)
                    "Engine found and verified. You can now select outSPOKEN " +
                    "in Text-to-speech settings." +
                    if (skipped.isEmpty()) "" else "\n\nNot offered: " +
                        skipped.joinToString("; ") { "${it.first} (${it.second})" }
                else
                    "No engine data found.\nUse Extract engine from zip file above, " +
                    "or copy the outspoken folder the desktop add-on extracted -- " +
                    "macintalk1, macintalk2, macintalk3, macintalkpro, macintalkespanol " +
                    "and voices -- into:\n" +
                    (OutspokenEngine.inboxRoot(this)?.absolutePath
                        ?: "Android/data/com.outspoken.tts/files/${OutspokenEngine.DATA_DIR}") +
                    if (skipped.isEmpty()) "" else "\n\nFound but not usable: " +
                        skipped.joinToString("; ") { "${it.first} (${it.second})" }
                refresh()
            }
        }.start()
    }

    private fun testSpeak() {
        // The voice the settings chose, so the preview previews the settings.
        val voice = OutspokenEngine.defaultVoice(this)
        if (voice == null) { toast("No voice to speak."); return }
        val text = sampleText.text.toString().ifBlank { "Hello there." }
        val systemRate = Settings.Secure.getInt(contentResolver, "tts_default_rate", 100)
        val ratePercent = OutspokenEngine.settings(this, voice.family).ratePercent(systemRate)
        testButton.isEnabled = false
        status.text = "Rendering ${voice.label}…"
        Thread {
            val pcm = OutspokenEngine.render(this, voice, text, ratePercent)
            val n = pcm?.size ?: 0
            val peak = if (n > 0) pcm!!.maxOf { kotlin.math.abs(it.toInt()) } else 0
            Log.i("Outspoken", "render ${voice.hostId}: $n samples, peak $peak")
            if (n > 0) try {
                writeWav(File(filesDir, "last-render.wav"), pcm!!, OutspokenEngine.sampleRate())
            } catch (e: Exception) { Log.w("Outspoken", "wav dump failed", e) }
            runOnUiThread {
                testButton.isEnabled = true
                status.text = when {
                    n == 0 -> "The sample could not be spoken."
                    peak == 0 -> "The sample is silent. Check the volume setting."
                    else -> "Playing the sample with ${voice.label}."
                }
            }
            if (n > 0 && peak > 0) playPcm(pcm!!, OutspokenEngine.sampleRate())
        }.start()
    }

    /** Jump to the system's Text-to-speech settings; fall back if unavailable. */
    private fun openTtsSettings() {
        val tries = listOf(
            Intent("com.android.settings.TTS_SETTINGS"),
            Intent(Settings.ACTION_ACCESSIBILITY_SETTINGS),
            Intent(Settings.ACTION_SETTINGS),
        )
        for (i in tries) {
            try { i.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK); startActivity(i); return }
            catch (e: Exception) { /* try the next */ }
        }
        toast("Couldn't open settings on this device.")
    }

    /** A 16-bit mono WAV, for pulling a rendered utterance off the device. */
    private fun writeWav(file: File, pcm: ShortArray, rate: Int) {
        val dataBytes = pcm.size * 2
        FileOutputStream(file).use { o ->
            fun i32(v: Int) = o.write(byteArrayOf(
                v.toByte(), (v shr 8).toByte(), (v shr 16).toByte(), (v shr 24).toByte()))
            fun i16(v: Int) = o.write(byteArrayOf(v.toByte(), (v shr 8).toByte()))
            o.write("RIFF".toByteArray()); i32(36 + dataBytes); o.write("WAVE".toByteArray())
            o.write("fmt ".toByteArray()); i32(16); i16(1); i16(1)
            i32(rate); i32(rate * 2); i16(2); i16(16)
            o.write("data".toByteArray()); i32(dataBytes)
            val b = ByteArray(dataBytes); var j = 0
            for (s in pcm) { b[j++] = s.toByte(); b[j++] = (s.toInt() shr 8).toByte() }
            o.write(b)
        }
    }

    private fun playPcm(pcm: ShortArray, rate: Int) {
        // MODE_STREAM, not MODE_STATIC: the streaming path (write after play,
        // blocking) is what a watch actually plays.  Accessibility usage so
        // it goes to the built-in speaker (media does not, on a watch).
        val minBuf = AudioTrack.getMinBufferSize(
            rate, AudioFormat.CHANNEL_OUT_MONO, AudioFormat.ENCODING_PCM_16BIT)
        val track = AudioTrack.Builder()
            .setAudioAttributes(
                AudioAttributes.Builder()
                    .setUsage(AudioAttributes.USAGE_ASSISTANCE_ACCESSIBILITY)
                    .setContentType(AudioAttributes.CONTENT_TYPE_SPEECH).build())
            .setAudioFormat(
                AudioFormat.Builder()
                    .setEncoding(AudioFormat.ENCODING_PCM_16BIT)
                    .setSampleRate(rate)
                    .setChannelMask(AudioFormat.CHANNEL_OUT_MONO).build())
            .setBufferSizeInBytes(maxOf(minBuf, rate))   // ~0.5 s of headroom
            .setTransferMode(AudioTrack.MODE_STREAM)
            .build()
        Log.i("Outspoken", "AudioTrack state=${track.state} min=$minBuf")
        track.play()
        var off = 0
        while (off < pcm.size) {
            val w = track.write(pcm, off, pcm.size - off)   // blocks, pacing playback
            if (w <= 0) { Log.e("Outspoken", "AudioTrack.write -> $w"); break }
            off += w
        }
        Log.i("Outspoken", "wrote $off/${pcm.size} samples, playState=${track.playState}")
        // A successful write only queues audio. Wait for the playback head,
        // with a deadline, before releasing the remaining buffered speech.
        val deadline = android.os.SystemClock.elapsedRealtime() + off * 1000L / rate + 2000
        while (track.playbackHeadPosition.toLong() < off &&
            android.os.SystemClock.elapsedRealtime() < deadline) {
            try { Thread.sleep(20) } catch (e: InterruptedException) { break }
        }
        Log.i("Outspoken", "playback head=${track.playbackHeadPosition}/$off routed=${track.routedDevice?.type}")
        try { track.stop() } finally { track.release() }
    }

    /** Anything in the inbox or the old internal folder goes into protected
     * storage now, with the progress on the Setup page.  The service does
     * the same on its own when it starts, so this is for the person who has
     * just copied a folder over and is looking at the screen. */
    private fun moveDataIn() {
        if (OutspokenEngine.migrating) return
        moveStatus?.text = OutspokenEngine.migrationNote ?: ""
        val pending = try { OutspokenEngine.pendingMoves(this) } catch (e: Exception) { emptyList() }
        if (pending.isEmpty()) return
        val names = pending.joinToString(", ") { it.second }
        moveStatus?.text = "Moving $names into protected storage…"
        moveAnnounced = -1
        Thread({
            OutspokenEngine.migrate(this) { name, done, total ->
                val permille = if (total > 0) (done * 1000 / total).toInt().coerceIn(0, 1000) else 0
                val fifth = permille / 200
                if (fifth != moveAnnounced) {
                    moveAnnounced = fifth
                    runOnUiThread {
                        if (isFinishing || isDestroyed) return@runOnUiThread
                        val line = "Moving $name into protected storage… ${permille / 10}%"
                        moveStatus?.text = line
                        // A screen reader hears the line change only if told.
                        if (fifth in 1..4) try { moveStatus?.announceForAccessibility("${fifth * 20} percent") } catch (e: Throwable) {}
                    }
                }
            }
            runOnUiThread {
                if (isFinishing || isDestroyed) return@runOnUiThread
                moveStatus?.text = OutspokenEngine.migrationNote ?: ""
                try { moveStatus?.announceForAccessibility(moveStatus?.text) } catch (e: Throwable) {}
                refresh(); rebuildVoices(); refreshVoiceButton(); rebuildEngineSwitches()
            }
        }, "outspoken-move").start()
    }

    private fun installedVersion(): String =
        try { packageManager.getPackageInfo(packageName, 0).versionName ?: "?" } catch (e: Exception) { "?" }

    private fun checkForUpdates() {
        val installed = installedVersion()
        updateStatus?.text = "Checking…"
        Thread {
            val answer = Updates.check(installed)
            runOnUiThread {
                if (isFinishing || isDestroyed) return@runOnUiThread
                when (answer) {
                    is Updates.Answer.UpToDate ->
                        updateStatus?.text = "You have the newest version, $installed."
                    is Updates.Answer.Failed ->
                        updateStatus?.text = "Could not check: ${answer.reason}."
                    is Updates.Answer.Available -> offerUpdate(answer.release, installed)
                }
                try { updateStatus?.announceForAccessibility(updateStatus?.text) } catch (e: Throwable) {}
            }
        }.start()
    }

    private fun offerUpdate(release: Updates.Release, installed: String) {
        val watch = packageManager.hasSystemFeature(android.content.pm.PackageManager.FEATURE_WATCH)
        updateStatus?.text = "Version ${release.number} is available; you have $installed."
        if (watch) {
            // A watch has no browser to hand the download to, and no file
            // manager to install from.  Say where it is instead.
            android.app.AlertDialog.Builder(this)
                .setTitle("Version ${release.number} is available")
                .setMessage("You have $installed. A watch cannot download and install it " +
                    "on its own; get the APK from ${Updates.RELEASES_PAGE} on a phone or " +
                    "computer and install it from there.")
                .setPositiveButton("Close", null).show()
            return
        }
        android.app.AlertDialog.Builder(this)
            .setTitle("Version ${release.number} is available")
            .setMessage("You have $installed.\n\nDownload opens the new version in your " +
                "browser. When the download finishes, open it from the notification and " +
                "Android installs it; the first time, it asks you to allow the browser to " +
                "install apps.")
            .setPositiveButton("Download") { _, _ -> openUrl(release.apk) }
            .setNeutralButton("Release notes") { _, _ -> openUrl(release.page) }
            .setNegativeButton("Later", null)
            .show()
    }

    private fun openUrl(url: String) {
        try {
            startActivity(Intent(Intent.ACTION_VIEW, Uri.parse(url)).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK))
        } catch (e: Exception) {
            updateStatus?.text = "No browser could open $url."
        }
    }

    private fun toast(t: String) = Toast.makeText(this, t, Toast.LENGTH_SHORT).show()

    private companion object {
        // Which engine the voice list shows: a family name, or all.
        const val PREF_VOICE_FILTER = "voice_filter"
        const val FILTER_ALL = "all"
        const val REQUEST_ZIP = 41
        // Wear OS answers a document request with this package, whose whole
        // job is to say the feature is not on a watch.
        const val WEAR_STUB = "com.google.android.wearable.frameworkpackagestubs"
        /** The import in flight, if any: one per process, whichever screen
         * instance is showing it. */
        @Volatile private var importJob: ImportJob? = null
    }
}
