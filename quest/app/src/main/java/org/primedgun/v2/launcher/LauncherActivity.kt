// SPDX-License-Identifier: GPL-3.0-or-later
// Derived from PrimedGun's Quest launcher (ui/main/MainActivity.kt, Copyright 2026
// PrimedGun Project, GPL-2.0-or-later) and Wiicompiled VR's LauncherActivity.
package org.primedgun.v2.launcher

import android.app.ActivityManager
import android.content.ActivityNotFoundException
import android.content.Context
import android.content.Intent
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.os.Process
import android.util.Log
import android.view.View
import android.widget.Toast
import androidx.appcompat.app.AppCompatActivity
import androidx.appcompat.app.AppCompatDelegate
import androidx.core.view.ViewCompat
import androidx.core.view.WindowCompat
import androidx.core.view.WindowInsetsCompat
import androidx.fragment.app.Fragment
import com.google.android.material.dialog.MaterialAlertDialogBuilder
import com.google.android.material.tabs.TabLayout
import java.io.File
import java.io.FileOutputStream
import java.io.IOException
import java.io.RandomAccessFile
import org.primedgun.v2.BuildConfig
import org.primedgun.v2.PrimedGunVrActivity
import org.primedgun.v2.PortOptionsActivity
import org.primedgun.v2.QuestStorage
import org.primedgun.v2.R
import org.primedgun.v2.databinding.ActivityLauncherBinding

/**
 * The PrimedGun launcher panel: the Quest counterpart of the PC launcher
 * (launcher/ui/launcher_window.cpp), with PrimedGun's Quest launcher look.
 *
 * It edits the game's port_settings.ini through the PC launcher's own core and starts
 * the game, [PrimedGunVrActivity], an immersive activity in its own `:game` process.
 * The game rewrites the whole settings file as it exits, so the settings tabs are
 * locked while that process lives, and the file is read again once it is gone. Play
 * saves pending edits first. Stop asks the game to close as its own menu would, and
 * offers to end it after ten seconds.
 */
class LauncherActivity : AppCompatActivity() {

    companion object {
        private const val TAG = "PrimedGunLauncher"
        /** Optional intent extra: the [PrimedGunTabs.Tab] name to open instead of Setup. */
        const val EXTRA_TAB = "tab"
        // Must match PrimedGunVrActivity's android:process in AndroidManifest.xml.
        private const val GAME_PROCESS_SUFFIX = ":game"
        private const val POLL_MS = 1000L
        // The game process ends a moment after its activity; keep looking that long.
        private const val EXIT_GRACE_POLLS = 3
        private const val FORCE_STOP_DELAY_MS = 10_000L
        private const val LOG_TAIL_BYTES = 48 * 1024

        private var initialized = false

        /** The panel is in front; when it is not, edits that arrive are saved at once. */
        @Volatile
        var panelResumed = false
            private set

        /**
         * Unpacks the APK's cannon slots (stamped by install, like the game's own
         * resources) and loads the settings, once per launcher process: a recreated
         * window keeps the unsaved edits.
         */
        fun ensureInitialized(context: Context): Boolean {
            if (initialized) return true
            unpackCannonLibrary(context)
            val ok = LauncherNative.init(
                QuestStorage.userFolder(context).path,
                QuestStorage.shippedCannonLibrary(context).path
            )
            initialized = true
            return ok
        }

        private fun unpackCannonLibrary(context: Context) {
            val target = QuestStorage.shippedCannonLibrary(context)
            val stamp = File(context.filesDir, "cannon_textures.version")
            @Suppress("DEPRECATION")
            val installedAt = context.packageManager.getPackageInfo(context.packageName, 0).lastUpdateTime
            val expected = "${BuildConfig.VERSION_CODE}:${BuildConfig.VERSION_NAME}:$installedAt"
            if (stamp.isFile && stamp.readText() == expected) return
            try {
                target.deleteRecursively()
                copyAssetTree(context, "cannon_textures", target)
                stamp.writeText(expected)
            } catch (e: IOException) {
                Log.e(TAG, "Failed to unpack the cannon textures", e)
            }
        }

        private fun copyAssetTree(context: Context, assetPath: String, destination: File) {
            val entries = context.assets.list(assetPath) ?: emptyArray()
            if (entries.isEmpty()) {
                destination.parentFile?.mkdirs()
                context.assets.open(assetPath).use { input ->
                    FileOutputStream(destination).use { output -> input.copyTo(output) }
                }
                return
            }
            destination.mkdirs()
            for (entry in entries) {
                copyAssetTree(context, "$assetPath/$entry", File(destination, entry))
            }
        }
    }

    private lateinit var binding: ActivityLauncherBinding
    private val tabs = PrimedGunTabs.Tab.entries
    private val handler = Handler(Looper.getMainLooper())

    /** The game's process is alive: settings are locked. */
    var gameRunning = false
        private set

    /** What the Setup tab says while the game runs. */
    var runStatus: String? = null
        private set

    // Play was pressed and the game's process has not shown up yet.
    private var launching = false

    // A transfer step took its settings: the footer and the visible tab show them,
    // whichever tab is open (the Setup tab shows the report).
    private val transferListener: (PrimedGunTransfer.Outcome) -> Unit = {
        refreshVisibleTab()
        updateFooter()
    }
    private var exitGrace = 0
    private var forceStopOffered = false

    private val poll = object : Runnable {
        override fun run() {
            updateRunning()
            if (gameRunning || launching || exitGrace > 0) {
                handler.postDelayed(this, POLL_MS)
            }
        }
    }

    private val offerForceStop = Runnable {
        if (gameRunning) {
            forceStopOffered = true
            runStatus = getString(R.string.primedgun_force_stop_offered)
            refreshVisibleTab()
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        // The launcher keeps the Qt window's dark palette whatever the system theme.
        delegate.localNightMode = AppCompatDelegate.MODE_NIGHT_YES
        super.onCreate(savedInstanceState)

        val settingsReadable = ensureInitialized(this)
        DiscCopy.discardStalePartial(this)

        binding = ActivityLauncherBinding.inflate(layoutInflater)
        setContentView(binding.root)
        setInsets()

        val version = "v" + BuildConfig.VERSION_NAME
        binding.mainToolbar.title = getString(R.string.primedgun_window_title, version)
        binding.mainCredit.text = getString(R.string.primedgun_credit, version)

        setupTabs(savedInstanceState)
        binding.mainResetAll.setOnClickListener { confirmResetAll() }
        binding.mainSaveSettings.setOnClickListener { saveSettings(showResult = true) }

        if (!settingsReadable) {
            showMessage(
                R.string.primedgun_error,
                getString(R.string.primedgun_settings_unreadable, QuestStorage.settingsFile(this).path)
            )
        }
    }

    override fun onStart() {
        super.onStart()
        PrimedGunTransfer.addListener(transferListener)
    }

    override fun onStop() {
        super.onStop()
        PrimedGunTransfer.removeListener(transferListener)
    }

    override fun onResume() {
        super.onResume()
        panelResumed = true
        // The full-options panel shares this settings file, even while :game lives.
        if (!LauncherSettings.dirty && LauncherNative.changedOnDisk()) {
            LauncherNative.reload()
            refreshVisibleTab()
        }
        // Back in front, so the game's activity is not: a launch either ended or never
        // began, and only the process (still exiting, perhaps) says whether it runs.
        launching = false
        val wasRunning = gameRunning
        updateRunning()
        if (!gameRunning && !wasRunning) {
            onGameGone()
        }
        handler.removeCallbacks(poll)
        // A game that is just ending still has its process for a moment.
        exitGrace = EXIT_GRACE_POLLS
        handler.postDelayed(poll, POLL_MS)
    }

    override fun onPause() {
        super.onPause()
        panelResumed = false
        handler.removeCallbacks(poll)
        // Edits are kept when the panel is closed without Save Settings, as PrimedGun's
        // Quest launcher does, except while the game owns the file.
        if (!gameRunning && !launching && LauncherSettings.dirty) {
            saveSettings(showResult = false)
        }
    }

    // --- the game ------------------------------------------------------------------

    private fun gamePid(): Int? {
        val manager = getSystemService(ActivityManager::class.java) ?: return null
        val name = packageName + GAME_PROCESS_SUFFIX
        return manager.runningAppProcesses.orEmpty().firstOrNull { it.processName == name }?.pid
    }

    private fun updateRunning() {
        val alive = gamePid() != null
        if (alive) {
            launching = false
        }
        val running = alive || launching
        if (exitGrace > 0 && !running) {
            exitGrace--
        }
        if (running == gameRunning) {
            if (running && runStatus == null) runStatus = getString(R.string.primedgun_running_locked)
            return
        }
        gameRunning = running
        if (running) {
            runStatus = getString(R.string.primedgun_running_locked)
        } else {
            Log.i(TAG, "The game process is gone")
            handler.removeCallbacks(offerForceStop)
            forceStopOffered = false
            runStatus = null
            onGameGone()
        }
        refreshVisibleTab()
        updateFooter()
    }

    /**
     * After the game, or at a start without it: the game rewrote the settings as it
     * exited, so read them again (unless there are edits, which Save Settings writes over
     * it), and say why it ended, and what became of a handed-over memory card.
     */
    private fun onGameGone() {
        if (!LauncherSettings.dirty) {
            LauncherNative.reload()
        }
        refreshVisibleTab()
        updateFooter()

        val messages = mutableListOf<String>()
        val error = QuestStorage.lastErrorFile(this)
        if (error.isFile) {
            runCatching { error.readText().trim() }.getOrNull()?.takeIf { it.isNotEmpty() }?.let {
                messages += getString(R.string.primedgun_game_error, it)
            }
            QuestStorage.clearLastError(this)
        }
        val report = QuestStorage.importReportFile(this)
        if (report.isFile) {
            runCatching { report.readText().trim() }.getOrNull()?.takeIf { it.isNotEmpty() }?.let {
                messages += getString(R.string.primedgun_import_report, it)
            }
            report.delete()
        }
        if (messages.isNotEmpty()) {
            showMessage(R.string.primedgun_title, messages.joinToString("\n\n"))
        }
    }

    fun openPortOptions() {
        if (PrimedGunTransfer.isStaging || DiscCopy.isRunning) return
        if (!gameRunning && !saveSettings(showResult = false)) return
        try {
            startActivity(Intent(this, PortOptionsActivity::class.java))
        } catch (e: ActivityNotFoundException) {
            Log.e(TAG, "Cannot open port options", e)
            Toast.makeText(this, R.string.primedgun_launch_failed, Toast.LENGTH_LONG).show()
        }
    }

    fun play() {
        if (gameRunning || launching || PrimedGunTransfer.isStaging || DiscCopy.isRunning) return
        val problem = QuestStorage.checkDisc(QuestStorage.discImage(this))
        if (problem != null) {
            showMessage(R.string.primedgun_play, problem)
            return
        }
        if (!saveSettings(showResult = false)) return
        reapplyCannonPack()
        QuestStorage.clearLastError(this)
        try {
            startActivity(Intent(this, PrimedGunVrActivity::class.java))
        } catch (e: ActivityNotFoundException) {
            Log.e(TAG, "Cannot start the game activity", e)
            Toast.makeText(this, R.string.primedgun_launch_failed, Toast.LENGTH_LONG).show()
            return
        }
        launching = true
        gameRunning = true
        runStatus = getString(R.string.primedgun_running_locked)
        refreshVisibleTab()
        updateFooter()
        handler.removeCallbacks(poll)
        handler.postDelayed(poll, POLL_MS)
    }

    // A slot whose files went missing (the user texture folder emptied over USB) is
    // copied in again, as the Qt launcher's slot is applied once and then trusted.
    private fun reapplyCannonPack() {
        val slot = LauncherSettings.int("vr_cannon_texture_slot")
        if (slot > 0 && LauncherNative.cannonPackFolders().any { !File(it).isDirectory }) {
            val error = LauncherNative.cannonApply(slot)
            if (error.isNotEmpty()) Log.w(TAG, "Re-applying cannon slot $slot failed: $error")
        }
    }

    fun stop() {
        if (!gameRunning) return
        val pid = gamePid()
        if (forceStopOffered && pid != null) {
            Log.w(TAG, "Force-stopping the game (pid $pid)")
            Process.killProcess(pid)
            return
        }
        // The game finishes as when the headset session ends: the settings and the
        // shader caches are saved on the way out.
        sendBroadcast(Intent(PrimedGunVrActivity.ACTION_STOP_GAME).setPackage(packageName))
        runStatus = getString(R.string.primedgun_stopping)
        refreshVisibleTab()
        handler.removeCallbacks(offerForceStop)
        handler.postDelayed(offerForceStop, FORCE_STOP_DELAY_MS)
    }

    // --- settings -------------------------------------------------------------------

    fun onSettingsEdited() = updateFooter()

    private fun updateFooter() {
        if (!::binding.isInitialized) return
        val status = when {
            gameRunning -> ""
            !LauncherSettings.dirty -> ""
            LauncherNative.changedOnDisk() -> getString(R.string.primedgun_unsaved_changed_on_disk)
            else -> getString(R.string.primedgun_unsaved_changes)
        }
        binding.mainStatus.text = status
        binding.mainStatus.visibility = if (status.isEmpty()) View.GONE else View.VISIBLE
        binding.mainResetAll.isEnabled = !gameRunning
        binding.mainSaveSettings.isEnabled = !gameRunning
    }

    fun prepareUpdate(): Boolean {
        val optionsOpen = (getSystemService(android.app.ActivityManager::class.java).runningAppProcesses ?: emptyList())
            .any { it.processName == "$packageName:options" }
        if (optionsOpen || gameRunning || launching || DiscCopy.isRunning || PrimedGunTransfer.isStaging) {
            Toast.makeText(this, "Close the game and Port Options, and finish any file transfers before installing an update.", Toast.LENGTH_LONG).show()
            return false
        }
        return saveSettings(showResult = false)
    }

    /** Writes the changed keys; false (after saying why) when the file could not be written. */
    private fun saveSettings(showResult: Boolean): Boolean {
        if (gameRunning) return false
        val error = LauncherSettings.save()
        updateFooter()
        if (error != null) {
            showMessage(R.string.primedgun_save_settings, getString(R.string.primedgun_settings_save_failed, error))
            return false
        }
        if (showResult) {
            Toast.makeText(this, R.string.primedgun_settings_saved, Toast.LENGTH_SHORT).show()
        }
        return true
    }

    private fun confirmResetAll() {
        // The desktop asks too; a headset panel is easier to mis-tap.
        MaterialAlertDialogBuilder(this)
            .setTitle(R.string.primedgun_reset_all)
            .setMessage(R.string.primedgun_reset_all_confirmation)
            .setPositiveButton(R.string.primedgun_yes) { _, _ ->
                LauncherNative.resetAll()
                refreshVisibleTab()
                updateFooter()
                Toast.makeText(this, R.string.primedgun_reset_all_done, Toast.LENGTH_SHORT).show()
            }
            .setNegativeButton(R.string.primedgun_no, null)
            .show()
    }

    // --- folders and logs -------------------------------------------------------------

    fun showUserFolder() {
        showMessage(
            R.string.primedgun_show_user_folder,
            getString(R.string.primedgun_user_folder_message, QuestStorage.userFolder(this).path)
        )
    }

    fun showTexturePackFolder() {
        val folder = File(QuestStorage.userFolder(this), "user_textures")
        if (!folder.isDirectory && !folder.mkdirs()) {
            showMessage(R.string.primedgun_error, "Could not create the texture pack folder:\n${folder.path}")
            return
        }
        showMessage(R.string.primedgun_texture_pack_folder,
            "Texture folder ready:\n${folder.path}\n\n" +
            "Connect your Quest to a computer and allow file access, then copy extracted PNG/DDS textures here. " +
            "A named pack subfolder is fine. Restart the game after copying. " +
            "See Texture Pack Help for the full steps.")
    }

    fun showTexturePackHelp() {
        val folder = File(QuestStorage.userFolder(this), "user_textures")
        showMessage(R.string.primedgun_texture_pack_help,
            "1. Extract your Metroid Prime texture pack ZIP/7z on your computer. Keep the PNG/DDS files' " +
            "original texture filenames (usually tex1_...). No ISO patch is needed.\n\n" +
            "2. Launch PrimedGun once. Use Show Texture Pack Folder to create user_textures if it is missing, " +
            "or create a folder named exactly user_textures inside Android/data/org.primedgun.v2/files.\n\n" +
            "Your texture folder:\n${folder.path}\n\n" +
            "3. Connect the Quest to your computer and allow file access. Copy the extracted textures into " +
            "that folder. A subfolder such as My HD Pack is fine; subfolders are scanned automatically. " +
            "Copy the extracted files, not the ZIP/7z archive.\n\n" +
            "4. Restart the game after copying. Use one pack/version at a time to avoid conflicting " +
            "replacements. Larger packs use more memory, especially on Quest; choose a lower-resolution version if needed.")
    }

    /** The end of the last run's log (MP_LOG_FILE), for a headset with no file viewer. */
    fun showLastLog() {
        val log = QuestStorage.gameLog(this)
        val text = if (!log.isFile) {
            getString(R.string.primedgun_no_log)
        } else {
            runCatching {
                RandomAccessFile(log, "r").use { file ->
                    val start = (file.length() - LOG_TAIL_BYTES).coerceAtLeast(0)
                    val bytes = ByteArray((file.length() - start).toInt())
                    file.seek(start)
                    file.readFully(bytes)
                    val tail = String(bytes, Charsets.UTF_8)
                    if (start > 0) tail.substringAfter('\n') else tail
                }
            }.getOrElse { getString(R.string.primedgun_no_log) }
        }
        MaterialAlertDialogBuilder(this)
            .setTitle(getString(R.string.primedgun_last_log_title, log.path))
            .setMessage(text)
            .setPositiveButton(R.string.primedgun_close, null)
            .show()
    }

    // --- tabs -------------------------------------------------------------------------

    /** Asks the visible tab to re-read the settings and the locked state. */
    fun refreshVisibleTab() {
        (supportFragmentManager.findFragmentById(R.id.main_content) as? PrimedGunRefreshable)?.refresh()
    }

    private fun setupTabs(savedInstanceState: Bundle?) {
        val tabLayout = binding.mainTabs
        tabs.forEach { tab -> tabLayout.addTab(tabLayout.newTab().setText(tab.titleId)) }

        // After a recreation the fragment manager already holds the open tab's fragment,
        // tagged with the tab name; only the tab strip has to catch up.
        val restored = if (savedInstanceState == null) null
        else tabs.firstOrNull { supportFragmentManager.findFragmentByTag(it.name) != null }
        if (restored != null) {
            tabLayout.selectTab(tabLayout.getTabAt(tabs.indexOf(restored)))
        } else {
            val requested = intent.getStringExtra(EXTRA_TAB)
            val initial = tabs.firstOrNull { it.name == requested } ?: tabs.first()
            tabLayout.selectTab(tabLayout.getTabAt(tabs.indexOf(initial)))
            showTab(initial)
        }

        tabLayout.addOnTabSelectedListener(object : TabLayout.OnTabSelectedListener {
            override fun onTabSelected(tab: TabLayout.Tab) = showTab(tabs[tab.position])
            override fun onTabUnselected(tab: TabLayout.Tab) = Unit
            override fun onTabReselected(tab: TabLayout.Tab) = Unit
        })
    }

    private fun showTab(tab: PrimedGunTabs.Tab) {
        supportFragmentManager.beginTransaction()
            .replace(R.id.main_content, createFragment(tab), tab.name)
            .commit()
    }

    private fun createFragment(tab: PrimedGunTabs.Tab): Fragment = when (tab) {
        PrimedGunTabs.Tab.SETUP -> PrimedGunSetupFragment()
        PrimedGunTabs.Tab.CANNON_TEXTURES -> PrimedGunCannonTexturesFragment()
        PrimedGunTabs.Tab.LAYOUT -> PrimedGunLayoutFragment()
        PrimedGunTabs.Tab.ABOUT -> PrimedGunAboutFragment()
        PrimedGunTabs.Tab.CONTROLLER,
        PrimedGunTabs.Tab.CALIBRATION,
        PrimedGunTabs.Tab.PORT_CONFIG -> PrimedGunSettingsFragment.newInstance(tab)
    }

    private fun showMessage(title: Int, message: String) {
        MaterialAlertDialogBuilder(this)
            .setTitle(title)
            .setMessage(message)
            .setPositiveButton(R.string.primedgun_ok, null)
            .show()
    }

    private fun setInsets() {
        WindowCompat.getInsetsController(window, window.decorView).apply {
            isAppearanceLightStatusBars = false
            isAppearanceLightNavigationBars = false
        }
        ViewCompat.setOnApplyWindowInsetsListener(binding.root) { view: View, windowInsets: WindowInsetsCompat ->
            val insets = windowInsets.getInsets(
                WindowInsetsCompat.Type.systemBars() or WindowInsetsCompat.Type.displayCutout()
            )
            view.setPadding(insets.left, insets.top, insets.right, insets.bottom)
            WindowInsetsCompat.CONSUMED
        }
    }
}
