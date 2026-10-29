// SPDX-License-Identifier: GPL-3.0-or-later
package org.primedgun.v2.launcher

import android.content.Intent
import android.content.pm.PackageManager
import android.net.Uri
import android.provider.Settings
import android.widget.Button
import android.widget.CheckBox
import android.widget.LinearLayout
import android.widget.ProgressBar
import android.widget.ScrollView
import android.widget.TextView
import androidx.activity.result.contract.ActivityResultContracts
import androidx.appcompat.app.AlertDialog
import androidx.core.content.FileProvider
import androidx.fragment.app.Fragment
import androidx.lifecycle.lifecycleScope
import com.google.android.material.dialog.MaterialAlertDialogBuilder
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import org.primedgun.v2.BuildConfig
import java.io.File
import java.security.MessageDigest

internal class QuestUpdateController(private val fragment: Fragment) {
    private var dialog: AlertDialog? = null
    private var job: Job? = null
    private var pending: File? = null
    private val permission = fragment.registerForActivityResult(ActivityResultContracts.StartActivityForResult()) {
        if (fragment.isAdded && pending != null) {
            if (fragment.requireContext().packageManager.canRequestPackageInstalls()) install()
            else message("Installation permission was not granted. You can try again or sideload the official APK.")
        }
    }
    private fun message(text: String) {
        if (!fragment.isAdded) return
        MaterialAlertDialogBuilder(fragment.requireContext()).setTitle("PrimedGun updates")
            .setMessage(text).setPositiveButton("OK", null).show()
    }
    fun close() { job?.cancel(); job=null; dialog?.dismiss(); dialog=null }
    fun show() {
        if (dialog?.isShowing == true) return
        val context = fragment.requireContext()
        val prefs = context.getSharedPreferences("updates", 0)
        val layout = LinearLayout(context).apply { orientation=LinearLayout.VERTICAL; setPadding(24,12,24,12) }
        val installed = TextView(context).apply { text="Installed: ${BuildConfig.VERSION_NAME}" }
        val previews = CheckBox(context).apply {
            text="Include alpha, beta and release candidate versions"
            isChecked=prefs.getBoolean("include_previews", '-' in BuildConfig.VERSION_NAME)
        }
        val status = TextView(context)
        val progress = ProgressBar(context, null, android.R.attr.progressBarStyleHorizontal).apply { max=100 }
        val notes = TextView(context).apply { setTextIsSelectable(true) }
        val scroll = ScrollView(context).apply { addView(notes) }
        val check = Button(context).apply { text="Check for updates" }
        val action = Button(context).apply { text="Download update"; isEnabled=false }
        val page = Button(context).apply { text="Open GitHub releases" }
        listOf(installed,previews,check,status,progress).forEach { layout.addView(it) }
        layout.addView(scroll, LinearLayout.LayoutParams(-1, 230))
        layout.addView(action); layout.addView(page)
        dialog=MaterialAlertDialogBuilder(context).setTitle("PrimedGun updates").setView(layout)
            .setNegativeButton("Close",null).create().also { d ->
                d.setOnDismissListener { job?.cancel(); job=null; dialog=null }
                d.show()
            }
        var release: ReleaseUpdates.Release? = null
        fun setBusy(busy: Boolean) { check.isEnabled=!busy; previews.isEnabled=!busy; action.isEnabled=!busy && release!=null }
        fun checkNow() {
            pending=null; release=null; action.text="Download update"; action.isEnabled=false
            status.text="Checking GitHub releases..."; notes.text=""; progress.isIndeterminate=true
            prefs.edit().putBoolean("include_previews",previews.isChecked).apply()
            val include=previews.isChecked
            setBusy(true)
            job=fragment.lifecycleScope.launch {
                try {
                    release=withContext(Dispatchers.IO) { ReleaseUpdates.latest(BuildConfig.VERSION_NAME, include) }
                    val r=release
                    status.text=if (r==null) "You are up to date for the selected release channel."
                                else "PrimedGun ${r.version} is available (${r.size/1048576} MB)."
                    notes.text=r?.notes.orEmpty()
                } catch (e: CancellationException) { throw e }
                  catch (e: Exception) { status.text="Could not check for updates: ${e.message}" }
                finally { progress.isIndeterminate=false; progress.progress=0; setBusy(false) }
            }
        }
        check.setOnClickListener { checkNow() }
        page.setOnClickListener {
            try { fragment.startActivity(Intent(Intent.ACTION_VIEW,Uri.parse(release?.page ?: ReleaseUpdates.PAGE))) }
            catch (_: Exception) { message(ReleaseUpdates.PAGE) }
        }
        action.setOnClickListener {
            if (pending != null) { install(); return@setOnClickListener }
            val r=release ?: return@setOnClickListener
            val folder=File(context.cacheDir,"updates").apply { mkdirs() }
            val file=File(folder,"PrimedGun-update.apk")
            status.text="Downloading and verifying the Quest APK..."; progress.progress=0; setBusy(true)
            job=fragment.lifecycleScope.launch {
                try {
                    withContext(Dispatchers.IO) {
                        ReleaseUpdates.download(r,file) { percent ->
                            fragment.activity?.runOnUiThread { progress.progress=percent }
                        }
                        validate(file,r.version)
                    }
                    pending=file
                    status.text="Download verified. Close the game before installing. Saves and settings are preserved."
                    action.text="Install update"
                } catch (e: CancellationException) { throw e }
                  catch (e: Exception) { pending=null; status.text=e.message ?: "Update download failed." }
                finally { setBusy(false) }
            }
        }
        checkNow()
    }
    @Suppress("DEPRECATION")
    private fun validate(file: File, expectedVersion: String? = null) {
        val context=fragment.requireContext()
        val pm=context.packageManager
        val apk=pm.getPackageArchiveInfo(file.path,PackageManager.GET_SIGNING_CERTIFICATES)
            ?: error("The downloaded file is not a valid APK.")
        require(apk.packageName==context.packageName) { "The APK belongs to a different application." }
        require(apk.longVersionCode>BuildConfig.VERSION_CODE.toLong()) { "The APK does not have a newer Android version code. Please report this release packaging error." }
        if (expectedVersion!=null) require(apk.versionName==expectedVersion) { "APK version does not match the selected release." }
        val signers=apk.signingInfo?.apkContentsSigners
        require(!signers.isNullOrEmpty() && signers.all {
            pm.hasSigningCertificate(context.packageName, MessageDigest.getInstance("SHA-256").digest(it.toByteArray()), PackageManager.CERT_INPUT_SHA256)
        }) { "This APK uses a different signing key. It cannot update this installation while preserving app data. No uninstall was performed; use a build signed by the same publisher." }
    }
    private fun install() {
        val file=pending ?: return
        if ((fragment.activity as? LauncherActivity)?.prepareUpdate()!=true) return
        try {
            validate(file)
            val context=fragment.requireContext()
            if (!context.packageManager.canRequestPackageInstalls()) {
                MaterialAlertDialogBuilder(context).setTitle("Allow PrimedGun updates")
                    .setMessage("Quest may ask you to allow this launcher to install apps. Enable that permission, then return here to approve the update.")
                    .setPositiveButton("Open permission settings") { _,_ ->
                        try { permission.launch(Intent(Settings.ACTION_MANAGE_UNKNOWN_APP_SOURCES, Uri.parse("package:${context.packageName}"))) }
                        catch (_: Exception) { message("Quest did not provide the installation permission screen. You can still sideload the APK from the GitHub release.") }
                    }.setNegativeButton("Cancel",null).show()
                return
            }
            val uri=FileProvider.getUriForFile(context,"${context.packageName}.updates",file)
            fragment.startActivity(Intent(Intent.ACTION_VIEW).setDataAndType(uri,"application/vnd.android.package-archive")
                .addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION))
        } catch (e: Exception) { message("Could not open the Quest installer: ${e.message}\nYou can still sideload the official APK from GitHub. Do not uninstall to update.") }
    }
}
