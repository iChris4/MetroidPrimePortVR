// SPDX-License-Identifier: GPL-3.0-or-later
// Derived from PrimedGun's Quest launcher (features/primedgun/ui/PrimedGunSetupFragment.kt,
// Copyright 2026 PrimedGun Project, GPL-2.0-or-later).
package org.primedgun.v2.launcher

import android.content.Intent
import android.net.Uri
import android.os.Bundle
import android.view.LayoutInflater
import android.view.View
import android.view.ViewGroup
import androidx.activity.result.contract.ActivityResultContracts
import androidx.core.content.ContextCompat
import androidx.core.view.isVisible
import androidx.fragment.app.Fragment
import androidx.lifecycle.lifecycleScope
import com.google.android.material.dialog.MaterialAlertDialogBuilder
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import org.primedgun.v2.QuestStorage
import org.primedgun.v2.R
import org.primedgun.v2.databinding.FragmentPrimedgunSetupBinding

/**
 * The Setup tab: the game disc, Play / Stop, and the old-save transfer.
 *
 * Select Game copies the picked image into the user folder (see [DiscCopy]), because
 * the game runs in its own process and reads a path. The desktop window's Transfer
 * button searches folders next to the executable for an old install; a headset has no
 * such neighbourhood, so here the player picks the old card, settings or exported user
 * data through the system file picker ([PrimedGunTransfer]).
 */
class PrimedGunSetupFragment : Fragment(), PrimedGunRefreshable {

    private val updates = QuestUpdateController(this)

    private var binding: FragmentPrimedgunSetupBinding? = null
    private var discCheckJob: Job? = null
    // The selected disc's name when its check last found a PAL disc, so a refresh keeps
    // the label's "(PAL, experimental)" while the check runs again.
    private var palDiscName: String? = null

    private val launcher: LauncherActivity? get() = activity as? LauncherActivity

    // A result that arrives while the tab is not resumed waits for onResume's replay, which
    // runs after the window's own resume refresh (that would clear the status line).
    private val copyListener: (DiscCopy.State) -> Unit = { state ->
        if (isResumed) onCopyState(state) else if (binding != null) refresh()
    }
    private val transferListener: (PrimedGunTransfer.Outcome) -> Unit = { onTransferOutcome() }

    private val requestGameFile = registerForActivityResult(ActivityResultContracts.OpenDocument()) { uri: Uri? ->
        if (uri != null) onGamePicked(uri)
    }

    private val requestTransferFile = registerForActivityResult(ActivityResultContracts.OpenDocument()) { uri: Uri? ->
        if (uri != null) transfer(uri, settingsOnly = false)
    }

    private val requestSettingsFile = registerForActivityResult(ActivityResultContracts.OpenDocument()) { uri: Uri? ->
        if (uri != null) {
            transfer(uri, settingsOnly = true)
        } else {
            PrimedGunTransfer.report += getString(R.string.primedgun_transfer_settings_skipped)
            finishTransfer()
        }
    }

    override fun onCreateView(inflater: LayoutInflater, container: ViewGroup?, savedInstanceState: Bundle?): View {
        val inflated = FragmentPrimedgunSetupBinding.inflate(inflater, container, false)
        binding = inflated
        return inflated.root
    }

    override fun onViewCreated(view: View, savedInstanceState: Bundle?) {
        super.onViewCreated(view, savedInstanceState)
        val b = binding ?: return
        b.setupUpdates.setOnClickListener { updates.show() }
        b.setupNotes.setOnClickListener { showSetupNotes() }
        b.setupSelectGame.setOnClickListener { requestGameFile.launch(arrayOf("*/*")) }
        b.setupPlay.setOnClickListener { launcher?.play() }
        b.setupStop.setOnClickListener { launcher?.stop() }
        b.setupGameOptions.setOnClickListener { showGameOptions() }
        b.setupTransferOldSave.setOnClickListener { confirmTransfer() }
    }

    override fun onStart() {
        super.onStart()
        DiscCopy.addListener(copyListener)
        PrimedGunTransfer.addListener(transferListener)
        refresh()
    }

    override fun onResume() {
        super.onResume()
        // A copy or a transfer that ended while this tab was not shown still says how it
        // went. Here rather than in onStart: LauncherActivity.onResume refreshes the tab
        // first (fragments resume after it), which would clear the status line.
        DiscCopy.pendingResult()?.let(::onCopyState)
        onTransferOutcome()
    }

    override fun onStop() {
        super.onStop()
        DiscCopy.removeListener(copyListener)
        PrimedGunTransfer.removeListener(transferListener)
    }

    override fun onDestroyView() {
        super.onDestroyView()
        updates.close()
        binding = null
    }

    override fun refresh() {
        val b = binding ?: return
        val context = requireContext()
        val running = launcher?.gameRunning ?: false
        val copying = DiscCopy.isRunning
        // A transfer being staged keeps Play off too: it would race the hand-over.
        val staging = PrimedGunTransfer.isStaging
        val busy = copying || staging
        val disc = QuestStorage.discImage(context)
        val haveDisc = disc.isFile && disc.length() > 0

        val name = DiscCopy.selectedName(context) ?: disc.name
        b.setupSelectedGame.isVisible = haveDisc
        b.setupSelectedGame.text = selectedGameText(name, pal = name == palDiscName)

        b.setupSelectGame.isEnabled = !running && !busy
        b.setupPlay.isEnabled = !running && !busy && haveDisc
        b.setupPlay.text = getString(if (running) R.string.primedgun_running else R.string.primedgun_play)
        b.setupStop.isVisible = running
        b.setupGameOptions.isEnabled = !running && !busy
        b.setupTransferOldSave.isEnabled = !running && !busy
        b.setupCopyProgress.isVisible = copying

        b.setupTransferPending.isVisible = PrimedGunTransfer.cardPending(context)

        val progress = DiscCopy.state as? DiscCopy.State.Copying
        when {
            running -> setStatus(launcher?.runStatus ?: getString(R.string.primedgun_running_locked), R.color.primedgun_good)
            copying && progress != null -> {
                b.setupCopyProgress.progress = progress.percent
                setStatus(
                    if (progress.done == 0L) getString(R.string.primedgun_disc_checking, progress.name)
                    else getString(R.string.primedgun_disc_copying_name, progress.name, progress.percent)
                )
            }
            staging -> setStatus(getString(R.string.primedgun_transfer_staging))
            else -> setStatus(null)
        }

        // The copy's own check (the game's, on its disc.iso), off the UI thread.
        b.setupRevisionWarning.isVisible = false
        if (haveDisc && !copying) {
            discCheckJob?.cancel()
            discCheckJob = viewLifecycleOwner.lifecycleScope.launch {
                val (problem, pal) = withContext(Dispatchers.IO) {
                    val found = QuestStorage.checkDisc(disc)
                    found to (found == null && QuestStorage.cardRegion(context) == "EUR")
                }
                val current = binding ?: return@launch
                current.setupRevisionWarning.isVisible = problem != null
                current.setupRevisionWarning.text = problem?.let { shortWarning(it) }
                palDiscName = if (pal) name else null
                current.setupSelectedGame.text = selectedGameText(name, pal)
            }
        }
    }

    // The game's PAL support is experimental, and its log says so too.
    private fun selectedGameText(name: String, pal: Boolean): String =
        getString(if (pal) R.string.primedgun_selected_game_pal else R.string.primedgun_selected_game, name)

    private fun shortWarning(problem: String): String = when {
        problem.contains("revision") -> getString(R.string.primedgun_wrong_revision)
        problem.contains("not Metroid Prime") -> getString(R.string.primedgun_wrong_game)
        else -> getString(R.string.primedgun_game_unreadable_short)
    }

    private fun setStatus(text: String?, colorId: Int = R.color.primedgun_text_muted) {
        val b = binding ?: return
        b.setupRunStatus.isVisible = !text.isNullOrEmpty()
        b.setupRunStatus.text = text
        b.setupRunStatus.setTextColor(ContextCompat.getColor(requireContext(), colorId))
    }

    // ---------- Game selection ----------

    private fun onGamePicked(uri: Uri) {
        val context = requireContext()
        try {
            context.contentResolver.takePersistableUriPermission(uri, Intent.FLAG_GRANT_READ_URI_PERMISSION)
        } catch (e: SecurityException) {
            // The picker's own grant covers this process for the copy.
        }
        DiscCopyService.start(context, uri)
        refresh()
    }

    // refresh() shows a running copy itself; this adds what a finished one says.
    private fun onCopyState(state: DiscCopy.State) {
        if (binding == null) return
        refresh()
        // Only the first to claim a finished result shows it.
        if ((state is DiscCopy.State.Done || state is DiscCopy.State.Failed) && !DiscCopy.claimResult(state)) {
            return
        }
        when (state) {
            is DiscCopy.State.Done ->
                setStatus(getString(R.string.primedgun_disc_copied, state.name), R.color.primedgun_good)
            is DiscCopy.State.Failed -> showMessage(R.string.primedgun_select_game_title, state.message)
            is DiscCopy.State.Copying, DiscCopy.State.Idle -> Unit
        }
    }

    private fun showGameOptions() {
        val context = requireContext()
        val disc = QuestStorage.discImage(context)
        val options = mutableListOf(
            getString(R.string.primedgun_show_user_folder) to { launcher?.showUserFolder() },
            getString(R.string.primedgun_show_card_folder) to {
                showMessage(R.string.primedgun_game_options, getString(R.string.primedgun_card_folder_message, QuestStorage.cardFolder(context).path))
            },
            getString(R.string.primedgun_show_last_log) to { launcher?.showLastLog() },
        )
        if (disc.isFile) {
            options += getString(R.string.primedgun_forget_game, disc.length() / (1024f * 1024f * 1024f)) to { confirmForget() }
        }
        MaterialAlertDialogBuilder(context)
            .setTitle(R.string.primedgun_game_options)
            .setItems(options.map { it.first }.toTypedArray()) { _, which -> options[which].second() }
            .setNegativeButton(R.string.primedgun_close, null)
            .show()
    }

    private fun confirmForget() {
        MaterialAlertDialogBuilder(requireContext())
            .setTitle(R.string.primedgun_game_options)
            .setMessage(R.string.primedgun_forget_game_confirmation)
            .setPositiveButton(R.string.primedgun_remove) { _, _ ->
                DiscCopy.forget(requireContext())
                refresh()
            }
            .setNegativeButton(R.string.primedgun_cancel, null)
            .show()
    }

    private fun showSetupNotes() {
        MaterialAlertDialogBuilder(requireContext())
            .setTitle(R.string.primedgun_setup_notes)
            .setMessage(R.string.primedgun_setup_notes_text)
            .setPositiveButton(R.string.primedgun_close, null)
            .show()
    }

    // ---------- Transfer old memory card / settings ----------

    private fun confirmTransfer() {
        if (launcher?.gameRunning == true) {
            showMessage(R.string.primedgun_transfer_title, getString(R.string.primedgun_transfer_running))
            return
        }
        MaterialAlertDialogBuilder(requireContext())
            .setTitle(R.string.primedgun_transfer_title)
            .setMessage(getString(R.string.primedgun_transfer_prompt) + "\n\n" + getString(R.string.primedgun_transfer_prompt_detail))
            .setPositiveButton(R.string.primedgun_transfer) { _, _ -> requestTransferFile.launch(arrayOf("*/*")) }
            .setNegativeButton(R.string.primedgun_cancel, null)
            .show()
    }

    private fun transfer(uri: Uri, settingsOnly: Boolean) {
        val context = requireContext()
        val name = DiscCopy.displayName(context, uri) ?: uri.toString()
        val acceptable = if (settingsOnly) PrimedGunTransfer.extensionOf(name) == "ini" else PrimedGunTransfer.isTransferFile(name)
        if (!acceptable) {
            showMessage(
                R.string.primedgun_transfer_title,
                getString(if (settingsOnly) R.string.primedgun_transfer_wrong_settings_file else R.string.primedgun_transfer_wrong_extension)
            )
            if (settingsOnly) finishTransfer()
            return
        }
        if (!settingsOnly) {
            PrimedGunTransfer.report = ""
        }
        if (!PrimedGunTransfer.begin(context, uri, name, settingsOnly)) return
        refresh()
    }

    // A transfer step has finished, now or while this tab was not shown.
    private fun onTransferOutcome() {
        if (binding == null) return
        val outcome = PrimedGunTransfer.consumeOutcome() ?: return
        launcher?.onSettingsEdited()
        refresh()
        val result = outcome.result
        val name = outcome.name
        if (result.error != null) {
            if (outcome.settingsOnly) {
                PrimedGunTransfer.report += getString(R.string.primedgun_transfer_settings_failed, name)
                finishTransfer()
            } else {
                showMessage(R.string.primedgun_transfer_title, result.error)
            }
            return
        }
        if (result.cardStaged) {
            PrimedGunTransfer.report = getString(R.string.primedgun_transfer_card_staged, name)
        } else if (result.zipWithoutCard) {
            PrimedGunTransfer.report = getString(R.string.primedgun_transfer_zip_no_card, name, result.cardRegion)
        }
        if (outcome.appliedFrom != null) {
            PrimedGunTransfer.report += getString(R.string.primedgun_transfer_settings_done, outcome.appliedFrom)
        }
        if (!outcome.settingsOnly && result.cardStaged && outcome.appliedFrom == null) {
            // A card alone: PrimedGun's second step, the settings beside it.
            MaterialAlertDialogBuilder(requireContext())
                .setTitle(R.string.primedgun_transfer_title)
                .setMessage(R.string.primedgun_transfer_import_settings_prompt)
                .setPositiveButton(R.string.primedgun_transfer_import_settings) { _, _ ->
                    requestSettingsFile.launch(arrayOf("*/*"))
                }
                .setNegativeButton(R.string.primedgun_transfer_skip) { _, _ ->
                    PrimedGunTransfer.report += getString(R.string.primedgun_transfer_settings_skipped)
                    finishTransfer()
                }
                .setCancelable(false)
                .show()
        } else {
            finishTransfer()
        }
    }

    private fun finishTransfer() {
        val message = PrimedGunTransfer.report.trim()
        PrimedGunTransfer.report = ""
        if (message.isNotEmpty()) {
            showMessage(R.string.primedgun_transfer_title, message)
        }
    }

    private fun showMessage(title: Int, message: String) {
        MaterialAlertDialogBuilder(requireContext())
            .setTitle(title)
            .setMessage(message)
            .setPositiveButton(R.string.primedgun_ok, null)
            .show()
    }
}
