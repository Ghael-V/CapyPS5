// SPDX-FileCopyrightText: Copyright 2026 CapyPS5 Project
// SPDX-License-Identifier: GPL-2.0-only

package org.capyps5.emulator

import android.content.Context
import android.content.Intent
import android.os.Bundle
import android.os.Process
import androidx.activity.compose.setContent
import org.capyps5.emulator.model.GameItem
import org.libsdl.app.SDL
import org.capyps5.emulator.ui.screens.EmulationScreen
import org.capyps5.emulator.ui.theme.CapyPS5Theme
import java.io.File

/**
 * Runs one game in its own process (":game"). The native core cannot be torn down and started
 * again inside one process (guest threads and global state outlive Emulator::Run), so leaving the
 * game ends this process and the launcher, in the main process, stays where it was.
 */
class GameActivity : CapyActivity() {

    companion object {
        private const val EXTRA_TITLE_ID = "title_id"
        private const val EXTRA_TITLE_NAME = "title_name"
        private const val EXTRA_DIR = "game_dir"
        private const val EXTRA_ELF = "game_elf"

        /** Native fatal reports land here (see Log::SetFatalReportPath). */
        fun errorFile(context: Context) = File(context.filesDir, "last_error.txt")

        fun launch(context: Context, game: GameItem) {
            context.startActivity(
                Intent(context, GameActivity::class.java)
                    .putExtra(EXTRA_TITLE_ID, game.titleId)
                    .putExtra(EXTRA_TITLE_NAME, game.titleName)
                    .putExtra(EXTRA_DIR, game.directoryPath)
                    .putExtra(EXTRA_ELF, game.elfPath)
            )
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        val elf = intent.getStringExtra(EXTRA_ELF)
        if (elf == null) {
            finish()
            return
        }
        val game = GameItem(
            titleId = intent.getStringExtra(EXTRA_TITLE_ID) ?: "",
            titleName = intent.getStringExtra(EXTRA_TITLE_NAME) ?: File(elf).name,
            directoryPath = intent.getStringExtra(EXTRA_DIR) ?: (File(elf).parent ?: ""),
            elfPath = elf,
        )
        // A report left by an earlier session must not be shown for this one.
        errorFile(this).delete()
        NativeBridge.nativeInit(filesDir = filesDir.absolutePath, cacheDir = cacheDir.absolutePath)
        // SDL (statically linked in the core) needs its Java side for audio output on Android; the
        // frontend is not an SDLActivity, so set it up by hand before the game opens any device.
        runCatching {
            SDL.setupJNI()
            SDL.initialize()
            SDL.setContext(this)
        }.onFailure { android.util.Log.e("CapyPS5-Audio", "SDL Java setup failed", it) }
        InputRouter.onRouteChanged = { toUi -> if (toUi) releaseEmulatedPad() }

        val settings = Settings(this)
        swapFaceButtons = settings.buttonLayout == ButtonLayout.Nintendo
        // Global settings with this game's overrides, applied before the core reads them.
        CoreOption.entries.forEach { option ->
            NativeBridge.nativeSetOption(option.property, settings.effectiveOption(game.titleId, option, game.isUnreal))
        }
        val driver = GpuDrivers.find(this, settings.effectiveGpuDriver(game.titleId))
        NativeBridge.nativeSetGpuDriver(
            nativeLibraryDir = applicationInfo.nativeLibraryDir,
            driverDir = driver?.dir?.absolutePath.orEmpty(),
            library = driver?.library.orEmpty(),
        )
        val versionText = Updater.versionText(this)
        setContent {
            CapyPS5Theme {
                EmulationScreen(
                    game = game,
                    settings = settings,
                    versionText = versionText,
                    onExit = ::exitGame,
                )
            }
        }
    }

    private fun exitGame() {
        runCatching { NativeBridge.nativeStopGame() }
        finish()
        // The launcher activity lives in the main process; ending this one frees everything the
        // emulator held (guest memory, threads, Vulkan device).
        Process.killProcess(Process.myPid())
    }
}
