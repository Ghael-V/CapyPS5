// SPDX-FileCopyrightText: Copyright 2026 CapyPS5 Project
// SPDX-License-Identifier: GPL-2.0-only

package org.capyps5.emulator.library

import android.app.Activity
import android.content.Context
import android.content.Intent
import android.net.Uri
import android.os.Bundle
import android.widget.Toast
import androidx.activity.compose.setContent
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.unit.dp
import androidx.lifecycle.lifecycleScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import org.capyps5.emulator.CapyActivity
import org.capyps5.emulator.InputRouter
import org.capyps5.emulator.MainActivity
import org.capyps5.emulator.R
import org.capyps5.emulator.model.GameItem
import org.capyps5.emulator.ui.NavKey
import org.capyps5.emulator.ui.OnNavKey
import org.capyps5.emulator.ui.theme.Capy
import org.capyps5.emulator.ui.theme.CapyPS5Theme

/**
 * Entry points other apps (frontends) start, each for one library game named by the extras
 * title_id or path:
 *
 * - <applicationId>.OPEN_GAME_SETTINGS opens the game's options in the launcher.
 * - <applicationId>.IMPORT_SAVES replaces the game's saved data with the ZIP in the intent data,
 *   only after the user confirms it here.
 * - <applicationId>.EXPORT_SAVES, started for a result, asks the user whether the calling app may
 *   have a copy of the game's saved data. If they allow it, the result is RESULT_OK with the
 *   save_zip URI and a one-time read grant (save_zip itself needs a signature permission, so no
 *   other app can read saves on its own); anything else is RESULT_CANCELED.
 */
class ExternalActionActivity : CapyActivity() {

    companion object {
        const val EXTRA_TITLE_ID = "title_id"
        const val EXTRA_PATH = "path"

        fun openSettingsAction(context: Context) = "${context.packageName}.OPEN_GAME_SETTINGS"

        fun importSavesAction(context: Context) = "${context.packageName}.IMPORT_SAVES"

        fun exportSavesAction(context: Context) = "${context.packageName}.EXPORT_SAVES"

        /** The app's own import (game options): the same confirmation as for other apps. */
        fun importSaves(context: Context, game: GameItem, zip: Uri) {
            context.startActivity(
                Intent(importSavesAction(context), zip, context, ExternalActionActivity::class.java)
                    .putExtra(EXTRA_TITLE_ID, game.titleId)
                    .addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION)
            )
        }
    }

    private sealed interface ImportState {
        data object Checking : ImportState
        data class Confirm(val game: GameItem, val replaces: Boolean) : ImportState
        data class ExportConfirm(val game: GameItem, val caller: String) : ImportState
        data object Working : ImportState
        data class Done(val message: String, val ok: Boolean) : ImportState
    }

    private var state by mutableStateOf<ImportState>(ImportState.Checking)

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        when (intent?.action) {
            openSettingsAction(this) -> openSettings()
            importSavesAction(this) -> startImport()
            exportSavesAction(this) -> startExport()
            else -> finish()
        }
    }

    override fun onResume() {
        super.onResume()
        InputRouter.setRouteToUi(true)
    }

    private fun requestedTitleId() = intent.getStringExtra(EXTRA_TITLE_ID)

    private fun requestedPath() = intent.getStringExtra(EXTRA_PATH)

    private fun openSettings() {
        lifecycleScope.launch {
            val game = withContext(Dispatchers.IO) { GameLookup.find(this@ExternalActionActivity, requestedTitleId(), requestedPath()) }
            val launcher = Intent(this@ExternalActionActivity, MainActivity::class.java)
                .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
            if (GameLookup.isGameRunning(this@ExternalActionActivity)) {
                // Clearing the launcher's task would end the running game: bring it back instead.
                toast(R.string.external_game_running)
            } else {
                launcher.addFlags(Intent.FLAG_ACTIVITY_CLEAR_TOP or Intent.FLAG_ACTIVITY_SINGLE_TOP)
                if (game != null) {
                    launcher.putExtra(MainActivity.EXTRA_OPEN_OPTIONS, game.elfPath)
                } else {
                    toast(R.string.external_game_unknown)
                }
            }
            startActivity(launcher)
            finish()
        }
    }

    private fun startImport() {
        val zip = intent.data
        if (zip == null) {
            finish()
            return
        }
        setResult(Activity.RESULT_CANCELED)
        setContent { CapyPS5Theme { ImportScreen() } }
        lifecycleScope.launch {
            state = withContext(Dispatchers.IO) { checkImport(zip) }
        }
    }

    private fun checkImport(zip: Uri): ImportState {
        if (GameLookup.isGameRunning(this)) return ImportState.Done(getString(R.string.external_game_running), false)
        val titleId = SaveData.zipTitleId(this, zip)
            ?: return ImportState.Done(getString(R.string.saves_import_not_saves), false)
        val game = GameLookup.find(this, titleId, null)
            ?: return ImportState.Done(getString(R.string.saves_import_not_in_library, titleId), false)
        // The caller named a game: the file must hold that game's saves.
        if (requestedTitleId() != null || requestedPath() != null) {
            val wanted = GameLookup.find(this, requestedTitleId(), requestedPath())
            if (wanted?.titleId != game.titleId) {
                return ImportState.Done(getString(R.string.saves_import_other_game, titleId), false)
            }
        }
        return ImportState.Confirm(game, SaveData.info(this, game.titleId) != null)
    }

    private fun startExport() {
        setResult(Activity.RESULT_CANCELED)
        // Only a caller that waits for the result can receive the grant (and be named to the user).
        val caller = callingPackage
        if (caller == null) {
            finish()
            return
        }
        val label = runCatching {
            packageManager.getApplicationLabel(packageManager.getApplicationInfo(caller, 0)).toString()
        }.getOrDefault(caller)
        setContent { CapyPS5Theme { ImportScreen() } }
        lifecycleScope.launch {
            state = withContext(Dispatchers.IO) {
                val game = GameLookup.find(this@ExternalActionActivity, requestedTitleId(), requestedPath())
                when {
                    game == null -> ImportState.Done(getString(R.string.external_game_unknown), false)
                    SaveData.info(this@ExternalActionActivity, game.titleId) == null ->
                        ImportState.Done(getString(R.string.saves_export_none, game.titleName), false)
                    else -> ImportState.ExportConfirm(game, label)
                }
            }
        }
    }

    private fun confirmExport(game: GameItem) {
        val zip = Uri.parse("content://$packageName.library/save_zip/${game.titleId}")
        setResult(Activity.RESULT_OK, Intent().setData(zip).addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION))
        finish()
    }

    private fun confirmImport(game: GameItem) {
        val zip = intent.data ?: return
        state = ImportState.Working
        lifecycleScope.launch {
            state = withContext(Dispatchers.IO) {
                if (GameLookup.isGameRunning(this@ExternalActionActivity)) {
                    ImportState.Done(getString(R.string.external_game_running), false)
                } else {
                    runCatching { SaveData.import(this@ExternalActionActivity, game.titleId, zip) }.fold(
                        onSuccess = {
                            setResult(Activity.RESULT_OK)
                            LibraryProvider.notifyChanged(this@ExternalActionActivity)
                            ImportState.Done(getString(R.string.saves_import_done, game.titleName), true)
                        },
                        onFailure = { ImportState.Done(getString(R.string.saves_import_failed, it.message.orEmpty()), false) },
                    )
                }
            }
        }
    }

    private fun toast(message: Int) = Toast.makeText(this, message, Toast.LENGTH_LONG).show()

    @Composable
    private fun ImportScreen() {
        val current = state
        val actions: List<Pair<String, () -> Unit>> = when (current) {
            is ImportState.Confirm -> listOf(
                stringResource(R.string.saves_import_confirm) to { confirmImport(current.game) },
                stringResource(R.string.cancel) to { finish() },
            )
            is ImportState.ExportConfirm -> listOf(
                stringResource(R.string.saves_export_allow) to { confirmExport(current.game) },
                stringResource(R.string.cancel) to { finish() },
            )
            is ImportState.Done -> listOf(stringResource(R.string.close) to { finish() })
            else -> emptyList()
        }
        // Confirmations start on Cancel.
        var focused by remember(current) {
            mutableIntStateOf(if (current is ImportState.Confirm || current is ImportState.ExportConfirm) 1 else 0)
        }
        OnNavKey { key ->
            when (key) {
                NavKey.Left -> if (actions.isNotEmpty()) focused = (focused - 1).mod(actions.size)
                NavKey.Right -> if (actions.isNotEmpty()) focused = (focused + 1).mod(actions.size)
                NavKey.Confirm -> actions.getOrNull(focused)?.second?.invoke()
                NavKey.Back -> if (current !is ImportState.Working) finish()
                else -> Unit
            }
        }
        Box(Modifier.fillMaxSize().background(Capy.Background), contentAlignment = Alignment.Center) {
            Column(
                modifier = Modifier
                    .width(560.dp)
                    .background(Capy.Surface, RoundedCornerShape(16.dp))
                    .padding(22.dp),
                verticalArrangement = Arrangement.spacedBy(12.dp),
            ) {
                val exporting = intent?.action == exportSavesAction(this@ExternalActionActivity)
                Text(
                    stringResource(if (exporting) R.string.saves_export_request_title else R.string.saves_import_title),
                    style = MaterialTheme.typography.titleLarge,
                )
                val text = when (current) {
                    ImportState.Checking -> stringResource(R.string.saves_import_checking)
                    ImportState.Working -> stringResource(R.string.saves_import_working)
                    is ImportState.Confirm -> stringResource(
                        if (current.replaces) R.string.saves_import_replaces else R.string.saves_import_adds,
                        current.game.titleName,
                        current.game.titleId,
                    )
                    is ImportState.ExportConfirm -> stringResource(
                        R.string.saves_export_request,
                        current.caller,
                        current.game.titleName,
                        current.game.titleId,
                    )
                    is ImportState.Done -> current.message
                }
                Text(
                    text,
                    style = MaterialTheme.typography.bodyMedium,
                    color = if (current is ImportState.Done && !current.ok) Capy.Bad else Capy.TextSecondary,
                )
                if (actions.isNotEmpty()) {
                    Row(horizontalArrangement = Arrangement.spacedBy(10.dp)) {
                        actions.forEachIndexed { i, (label, action) ->
                            Text(
                                label,
                                style = MaterialTheme.typography.labelLarge,
                                color = Color.White,
                                modifier = Modifier
                                    .background(if (i == focused) Capy.Accent else Capy.SurfaceHigh, RoundedCornerShape(50))
                                    .clickable { focused = i; action() }
                                    .padding(horizontal = 18.dp, vertical = 9.dp),
                            )
                        }
                    }
                }
            }
        }
    }
}
