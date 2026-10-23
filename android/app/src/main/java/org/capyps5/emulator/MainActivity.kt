// SPDX-FileCopyrightText: Copyright 2026 CapyPS5 Project
// SPDX-License-Identifier: GPL-2.0-only

package org.capyps5.emulator

import android.content.Intent
import android.os.Bundle
import android.os.Environment
import androidx.activity.compose.setContent
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateListOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.unit.dp
import org.capyps5.emulator.model.GameItem
import org.capyps5.emulator.ui.NavKey
import org.capyps5.emulator.ui.OnNavKey
import org.capyps5.emulator.ui.screens.HomeScreen
import org.capyps5.emulator.ui.theme.Capy
import org.capyps5.emulator.ui.theme.CapyPS5Theme
import java.io.File
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale
import androidx.compose.ui.res.stringResource
import androidx.lifecycle.lifecycleScope
import kotlinx.coroutines.launch
import org.capyps5.emulator.ui.components.SupportDialog
import org.capyps5.emulator.ui.components.UpdateDialog
import org.capyps5.emulator.ui.components.WhatsNewDialog

/** Launcher: game library and settings. Each game runs in GameActivity, in its own process. */
class MainActivity : CapyActivity() {

    private lateinit var settings: Settings
    private val libraryDir get() = File(settings.libraryPath)
    // Bumped when all-files access may have just been granted, so the library is scanned again.
    private var storageCheck by mutableIntStateOf(0)
    // Fatal report the last game session left behind, shown once when the launcher returns.
    private var lastError by mutableStateOf<String?>(null)
    // Newer release found at start, and whether this launch shows the support card.
    private var update by mutableStateOf<Release?>(null)
    private var showSupport by mutableStateOf(false)
    // First start after an update: the "what's new" card for the installed version.
    private var showWhatsNew by mutableStateOf(false)

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        settings = Settings(this)
        // Games are read by path, which needs all-files access; a fresh install asks for it once.
        if (!Environment.isExternalStorageManager()) {
            runCatching {
                startActivity(
                    Intent(android.provider.Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION)
                        .setData(android.net.Uri.parse("package:$packageName"))
                )
            }
        } else if (!libraryDir.exists()) {
            runCatching { libraryDir.mkdirs() }
        }

        val versionText = Updater.versionText(this)
        val installed = Updater.installedVersion(this)
        if (savedInstanceState == null) {
            val launches = settings.countLaunch()
            showSupport = launches % SUPPORT_EVERY == 0
            // Shown after an update, not on a fresh install; v0.0.1 did not record a version, so
            // an earlier launch also counts as an update.
            val seen = settings.lastSeenVersion
            showWhatsNew = seen != installed && (seen.isNotEmpty() || launches > 1)
            settings.lastSeenVersion = installed
            if (settings.updateChecks) {
                lifecycleScope.launch {
                    update = Updater.checkForUpdate(installed)?.takeIf { it.tag != settings.skippedRelease }
                }
            }
        }
        // `adb shell am start ... --es launch_elf <path> [--es launch_dir <dir>]` starts a game directly.
        intent?.getStringExtra("launch_elf")?.takeIf { File(it).exists() }?.let { elf ->
            val dir = File(intent.getStringExtra("launch_dir") ?: File(elf).parent ?: "")
            GameActivity.launch(this, GameItem.fromGameDir(dir, File(elf)))
        }

        setContent {
            CapyPS5Theme {
                val games = remember { mutableStateListOf<GameItem>() }
                LaunchedEffect(settings.libraryPaths, storageCheck) {
                    games.clear()
                    games.addAll(scanLibrary())
                }
                Box(Modifier.fillMaxSize()) {
                    HomeScreen(
                        games = games,
                        libraryPath = settings.libraryPath,
                        settings = settings,
                        versionText = versionText,
                        onGameSelected = { GameActivity.launch(this@MainActivity, it) },
                        onAddGameFile = { uriString ->
                            val localFile = if (uriString.startsWith("content://")) {
                                copyUriToLocal(android.net.Uri.parse(uriString))
                            } else {
                                File(uriString)
                            }
                            if (localFile != null && localFile.exists()) {
                                val newGame = GameItem(
                                    titleId = "LOCAL",
                                    titleName = localFile.name,
                                    directoryPath = localFile.parent ?: filesDir.absolutePath,
                                    elfPath = localFile.absolutePath,
                                )
                                games.add(newGame)
                                GameActivity.launch(this@MainActivity, newGame)
                            }
                        },
                        onRefresh = {
                            games.clear()
                            games.addAll(scanLibrary())
                        },
                        onLibraryPicked = { settings.addLibraryPath(it) },
                        onClearShaderCache = ::clearShaderCache,
                        onSaveLog = ::saveLog,
                        overlayActive = lastError != null || showWhatsNew || update != null || showSupport,
                    )
                    // One notice at a time: the crash report, what's new, an update, the support card.
                    val report = lastError
                    val pending = update
                    when {
                        report != null -> CrashDialog(
                            report = report,
                            onSaveLog = ::saveLog,
                            onClose = { lastError = null },
                        )
                        showWhatsNew -> WhatsNewDialog(version = installed, onClose = { showWhatsNew = false })
                        pending != null -> UpdateDialog(
                            release = pending,
                            installed = installed,
                            onSkip = {
                                settings.skippedRelease = pending.tag
                                update = null
                            },
                            onClose = { update = null },
                        )
                        showSupport -> SupportDialog(onClose = { showSupport = false })
                    }
                }
            }
        }
    }

    override fun onResume() {
        super.onResume()
        InputRouter.setRouteToUi(true)
        // Returning from the all-files access screen: scan again now that the folder is readable.
        if (Environment.isExternalStorageManager()) storageCheck++
        // Returning from a game that closed itself: show why, once.
        val report = GameActivity.errorFile(this)
        if (report.exists()) {
            lastError = runCatching { report.readText() }.getOrNull()?.takeIf { it.isNotBlank() }
            report.delete()
        }
    }

    private companion object {
        const val SUPPORT_EVERY = 10
    }

    /** Deletes the persisted pipeline cache of one game, or of every game when [titleId] is null. */
    private fun clearShaderCache(titleId: String?): String {
        val dir = File(filesDir, "_PipelineCache")
        val files = dir.listFiles()?.filter { titleId == null || it.name == "$titleId.bin" } ?: emptyList()
        val removed = files.count { it.delete() }
        return getString(if (removed == 0) R.string.shader_cache_empty else R.string.shader_cache_cleared)
    }

    /** Games of every library folder, by name; a game found in two folders shows once. */
    private fun scanLibrary(): List<GameItem> =
        settings.libraryPaths.flatMap { GameItem.scan(File(it)) }
            .distinctBy { it.elfPath }
            .sortedBy { it.titleName.lowercase() }

    /**
     * Writes this app's recent log (both processes share the app's UID, so the game's lines are
     * included) next to the game library, where users can find and attach it.
     */
    private fun saveLog(): String? = runCatching {
        val dir = File(libraryDir, "logs").apply { mkdirs() }
        val stamp = SimpleDateFormat("yyyyMMdd-HHmmss", Locale.US).format(Date())
        val file = File(dir, "capyps5-$stamp.txt")
        val process = Runtime.getRuntime().exec(arrayOf("logcat", "-d", "-v", "time"))
        process.inputStream.use { input -> file.outputStream().use { output -> input.copyTo(output) } }
        process.waitFor()
        file.absolutePath
    }.getOrNull()

    private fun copyUriToLocal(uri: android.net.Uri): File? = try {
        var name = "eboot.bin"
        contentResolver.query(uri, null, null, null, null)?.use { cursor ->
            if (cursor.moveToFirst()) {
                val idx = cursor.getColumnIndex(android.provider.OpenableColumns.DISPLAY_NAME)
                if (idx != -1) cursor.getString(idx)?.let { name = it }
            }
        }
        val targetDir = File(filesDir, "imported").apply { mkdirs() }
        val targetFile = File(targetDir, name)
        contentResolver.openInputStream(uri)?.use { input ->
            targetFile.outputStream().use { output -> input.copyTo(output) }
        }
        targetFile
    } catch (_: Exception) {
        null
    }
}

@Composable
private fun CrashDialog(report: String, onSaveLog: () -> String?, onClose: () -> Unit) {
    var savedPath by remember { mutableStateOf<String?>(null) }
    var focused by remember { mutableIntStateOf(0) }
    val failedText = stringResource(R.string.log_save_failed)
    val actions = listOf(
        stringResource(R.string.save_log) to { savedPath = onSaveLog() ?: failedText },
        stringResource(R.string.close) to onClose,
    )
    OnNavKey { key ->
        when (key) {
            NavKey.Left -> focused = (focused - 1).mod(actions.size)
            NavKey.Right -> focused = (focused + 1).mod(actions.size)
            NavKey.Confirm -> actions[focused].second()
            NavKey.Back -> onClose()
            else -> Unit
        }
    }
    Box(Modifier.fillMaxSize().background(Capy.Scrim), contentAlignment = Alignment.Center) {
        Column(
            modifier = Modifier
                .width(560.dp)
                .background(Capy.Surface, RoundedCornerShape(16.dp))
                .padding(22.dp),
            verticalArrangement = Arrangement.spacedBy(12.dp),
        ) {
            Text(stringResource(R.string.crash_title), style = MaterialTheme.typography.titleLarge, color = Capy.Bad)
            Text(
                report.trim(),
                style = MaterialTheme.typography.bodySmall.copy(fontFamily = FontFamily.Monospace),
                color = Capy.TextSecondary,
                modifier = Modifier.heightIn(max = 220.dp).verticalScroll(rememberScrollState()),
            )
            savedPath?.let {
                Text(stringResource(R.string.log_saved_in_multiline, it), style = MaterialTheme.typography.bodySmall, color = Capy.TextPrimary)
            }
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
