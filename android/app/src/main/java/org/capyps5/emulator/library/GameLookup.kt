// SPDX-FileCopyrightText: Copyright 2026 CapyPS5 Project
// SPDX-License-Identifier: GPL-2.0-only

package org.capyps5.emulator.library

import android.app.ActivityManager
import android.content.Context
import android.os.SystemClock
import org.capyps5.emulator.Settings
import org.capyps5.emulator.model.GameItem
import java.io.File

/**
 * Finds one game of the library by title id or by the path of its eboot.bin, for other apps
 * (frontends) that already know the game. Only games in the library folders are found, and only
 * those with a real title id: everything other apps can read is keyed by it.
 */
object GameLookup {
    private val TITLE_ID = Regex("^[A-Z]{4}[0-9]{5}$")
    private const val CACHE_MS = 10_000L

    @Volatile
    private var cached: Pair<Long, List<GameItem>>? = null

    fun isTitleId(value: String?): Boolean = value != null && TITLE_ID.matches(value)

    /** The library's games; scanned again at most every few seconds. */
    fun games(context: Context): List<GameItem> {
        val now = SystemClock.elapsedRealtime()
        cached?.let { (at, games) -> if (now - at < CACHE_MS) return games }
        val games = Settings(context).libraryPaths.flatMap { GameItem.scan(File(it)) }
            .distinctBy { it.elfPath }
            .filter { isTitleId(it.titleId) }
        cached = now to games
        return games
    }

    /** With both keys, [path] decides. Null when the game is not in the library. */
    fun find(context: Context, titleId: String?, path: String?): GameItem? {
        val games = games(context)
        if (!path.isNullOrBlank()) {
            val wanted = canonical(path) ?: return null
            return games.firstOrNull { canonical(it.elfPath) == wanted || canonical(it.directoryPath) == wanted }
        }
        if (!isTitleId(titleId)) return null
        return games.firstOrNull { it.titleId == titleId }
    }

    /** Whether a game is running now (it has its own process, see GameActivity). */
    fun isGameRunning(context: Context): Boolean {
        val manager = context.getSystemService(ActivityManager::class.java) ?: return false
        val name = "${context.packageName}:game"
        return manager.runningAppProcesses.orEmpty().any { it.processName == name }
    }

    private fun canonical(path: String): String? = runCatching { File(path).canonicalPath }.getOrNull()
}
