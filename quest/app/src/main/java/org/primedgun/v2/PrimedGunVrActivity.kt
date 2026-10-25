// SPDX-License-Identifier: GPL-3.0-or-later
package org.primedgun.v2

import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import dev.encounter.aurora.AuroraSurface
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.os.Process
import android.system.Os
import android.util.Log
import androidx.core.content.ContextCompat
import java.io.File
import java.io.FileOutputStream
import java.io.IOException
import org.libsdl.app.SDLActivity
import org.libsdl.app.SDLSurface

/**
 * The game: Metroid Prime in the headset. SDLActivity loads libmetroid_prime_port.so
 * and runs its SDL_main; the OpenXR loader finds this activity and the JavaVM
 * through SDL (platform/vr/openxr_android.cpp). Upstream's phone activity,
 * MetroidPrimeActivity, stays out of the Quest build: its touch overlay, storage
 * prompts and pickers are 2D UI nobody sees in VR.
 *
 * Before the library loads, this hands the game its folders and flags through the
 * environment, which the native side reads lazily:
 *  - MP_USER_PATH, the user folder ([QuestStorage.userFolder]);
 *  - MP_DISC, the disc the launcher copied in, only when it checks out, and
 *    MP_NO_DISC_DIALOG, so the game never opens a picker inside the headset;
 *  - MP_VR=1 whatever the settings say, and MP_LOG_FILE=1 for the log file.
 *
 * It runs in its own `:game` process, which ends with it: SDLActivity cannot be
 * created twice in one process, and the OpenXR device must not start twice. The
 * launcher (launcher.LauncherActivity) starts it with Play, and its Stop sends
 * [ACTION_STOP_GAME], which finishes it as the headset's Quit does.
 */
open class PrimedGunVrActivity : SDLActivity() {
    protected open val optionsOnly = false

    override fun getArguments(): Array<String> = if (optionsOnly) arrayOf("--options") else emptyArray()

    private val stopReceiver = object : BroadcastReceiver() {
        override fun onReceive(context: Context, intent: Intent) {
            Log.i(TAG, "The launcher asked the game to stop")
            if (!isFinishing) {
                finish()
            }
        }
    }
    private var stopReceiverRegistered = false

    override fun getLibraries(): Array<String> = arrayOf("metroid_prime_port")

    override fun createSDLSurface(context: Context): SDLSurface =
        if (optionsOnly) AuroraSurface(context) else QuestSurface(context)

    override fun onCreate(savedInstanceState: Bundle?) {
        val user = QuestStorage.userFolder(this)
        if (!optionsOnly) QuestStorage.clearLastError(this)
        Os.setenv("MP_USER_PATH", user.absolutePath, true)
        Os.setenv("MP_NO_DISC_DIALOG", "1", true)
        Os.setenv("MP_VR", if (optionsOnly) "0" else "1", true)
        Os.setenv("MP_LOG_FILE", if (optionsOnly) "0" else "1", true)
        if (optionsOnly) {
            // Share settings, not live GPU cache files, with the game process.
            val optionsCache = File(cacheDir, "port_options").apply { mkdirs() }
            Os.setenv("MP_CACHE_PATH", optionsCache.absolutePath, true)
        }
        // Test runs over adb, e.g. straight into the Landing Site:
        //   am start -n org.primedgun.v2/.PrimedGunVrActivity --es MP_BOOT_WORLD 39F2DE28:B2701146
        for (name in TEST_ENVIRONMENT) {
            intent?.getStringExtra(name)?.let { Os.setenv(name, it, true) }
        }
        if (!optionsOnly) {
            val disc = QuestStorage.discImage(this)
            val discProblem = QuestStorage.checkDisc(disc)
            if (discProblem == null) {
                Os.setenv("MP_DISC", disc.absolutePath, true)
            } else {
                // The game then finds no disc, does not ask (MP_NO_DISC_DIALOG) and ends,
                // which finishes this activity.
                Log.w(TAG, discProblem)
                QuestStorage.writeLastError(this, discProblem)
                Os.unsetenv("MP_DISC")
            }
        }
        unpackPortResources()

        super.onCreate(savedInstanceState)

        // Only this app's launcher can send it (an explicit, package-scoped broadcast).
        if (!optionsOnly) {
            ContextCompat.registerReceiver(this, stopReceiver, IntentFilter(ACTION_STOP_GAME), ContextCompat.RECEIVER_NOT_EXPORTED)
            stopReceiverRegistered = true
        }

        if (mBrokenLibraries) {
            // SDLActivity reports this in a dialog, which the headset never shows.
            QuestStorage.writeLastError(this, "The game library could not be loaded on this headset.")
            Log.e(TAG, "SDL could not load the game library; leaving")
            finish()
        }
    }

    /**
     * Called from native code (platform/vr) when the headset session is over for
     * good: the runtime asked the app to exit (Quit in the Quest menu), or VR
     * failed and there is no desktop to fall back to. [error] is empty for a plain
     * exit. Finishing makes SDL post its quit event and wakes a game thread SDL
     * paused; onDestroy then ends the process.
     */
    @Suppress("unused")
    fun requestQuit(error: String) {
        if (error.isNotEmpty()) {
            Log.e(TAG, "Leaving: $error")
            QuestStorage.writeLastError(this, error)
        } else {
            Log.i(TAG, "The headset session ended; leaving")
        }
        runOnUiThread {
            if (!isFinishing) {
                finish()
            }
        }
    }

    override fun onDestroy() {
        if (stopReceiverRegistered) {
            unregisterReceiver(stopReceiver)
            stopReceiverRegistered = false
        }
        // SDLActivity sends the quit event and waits a second for SDL_main before it
        // tears SDL's Java side down. The game's own teardown (the OpenXR session,
        // Aurora's device, the settings file) can take longer, so give it a head start.
        if (!mBrokenLibraries) {
            SDLActivity.mSDLThread?.let { thread ->
                if (thread.isAlive) {
                    SDLActivity.nativeSendQuit()
                    try {
                        thread.join(QUIT_GRACE_MS)
                    } catch (_: InterruptedException) {
                    }
                }
            }
        }
        super.onDestroy()
        // Ended from the main looper, so the activity manager has recorded this
        // destruction first.
        Log.i(TAG, "Game activity destroyed; ending the game process")
        Handler(Looper.getMainLooper()).post { Process.killProcess(Process.myPid()) }
    }

    /**
     * The built-in textures and the pipeline cache seeds the game reads from its
     * private folder (platform/main.cpp), as upstream's MetroidPrimeActivity copies
     * them, but only once per installed build: the stamp holds the install time,
     * because a rebuilt APK sideloaded over the last one keeps its version.
     */
    private fun unpackPortResources() {
        val stamp = File(filesDir, "port_resources.version")
        @Suppress("DEPRECATION")
        val installedAt = packageManager.getPackageInfo(packageName, 0).lastUpdateTime
        val expected = "${BuildConfig.VERSION_CODE}:${BuildConfig.VERSION_NAME}:$installedAt"
        if (stamp.isFile && stamp.readText() == expected) {
            return
        }
        try {
            val textures = File(filesDir, "textures")
            textures.deleteRecursively()
            copyAssetTree("textures", textures)
            // The APK carries PrimedGun's seed and not upstream's (see syncPortAssets
            // in build.gradle.kts). A seed the APK doesn't carry goes, since aurora
            // would keep merging an earlier build's rows at every start.
            val packed = assets.list("")?.toSet() ?: emptySet()
            for (name in listOf("initial_pipeline_cache.db", "primedgun_pipeline_cache.db")) {
                val seed = File(filesDir, name)
                if (name in packed) {
                    copyAsset(name, seed)
                } else {
                    seed.delete()
                }
            }
            stamp.writeText(expected)
            Log.i(TAG, "Unpacked the game's resources into ${filesDir.absolutePath}")
        } catch (e: IOException) {
            Log.e(TAG, "Failed to prepare the game's resources", e)
        }
    }

    private fun copyAssetTree(assetPath: String, destination: File) {
        val entries = assets.list(assetPath) ?: emptyArray()
        if (entries.isEmpty()) {
            copyAsset(assetPath, destination)
            return
        }
        destination.mkdirs()
        for (entry in entries) {
            copyAssetTree("$assetPath/$entry", File(destination, entry))
        }
    }

    private fun copyAsset(assetPath: String, destination: File) {
        destination.parentFile?.mkdirs()
        assets.open(assetPath).use { input ->
            FileOutputStream(destination).use { output -> input.copyTo(output) }
        }
    }

    companion object {
        /** The launcher's Stop. */
        const val ACTION_STOP_GAME = "org.primedgun.v2.action.STOP_GAME"
        private const val TAG = "PrimedGunVr"
        private const val QUIT_GRACE_MS = 3000L
        // The game's per-run switches an adb start may pass as string extras.
        private val TEST_ENVIRONMENT =
            listOf("MP_BOOT_WORLD", "MP_FAST_BOOT", "MP_VR_LOG", "MP_LOAD_STATE", "MP_FRAME_STATS", "MP_SORT_OPAQUE", "MP_MINIMAP_BATCH", "MP_MAP_SCREEN_BATCH", "MP_SURFACE_CULL", "MP_MONO_SHADOW", "MP_GEOMETRY_CACHE", "MP_FOVEATION", "MP_FDM_DEVICE", "MP_FOVEATION_LAYERS", "MP_NATIVE_VERTICES",
                // Dawn toggles for driver debugging (aurora gpu.cpp), e.g. MP_DAWN_DISABLE=skip_validation.
                "MP_DAWN_ENABLE", "MP_DAWN_DISABLE")
    }
}
