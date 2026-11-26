// SPDX-FileCopyrightText: Copyright 2026 CapyPS5 Project
// SPDX-License-Identifier: GPL-2.0-only

package org.capyps5.emulator.library

import android.content.Context
import android.os.SystemClock
import org.json.JSONObject
import java.io.File

/** Time played in one game, as the emulator measured it. */
data class PlayStats(val seconds: Long, val lastPlayed: Long, val sessions: Int)

/** Time played per game, in files/_PlayTime/<title id>.json. */
object PlayTime {
    private fun file(context: Context, titleId: String) = File(context.filesDir, "_PlayTime/$titleId.json")

    fun read(context: Context, titleId: String): PlayStats? {
        if (!GameLookup.isTitleId(titleId)) return null
        val json = runCatching { JSONObject(file(context, titleId).readText()) }.getOrNull() ?: return null
        return PlayStats(
            seconds = json.optLong("seconds").coerceAtLeast(0),
            lastPlayed = json.optLong("last_played").coerceAtLeast(0),
            sessions = json.optInt("sessions").coerceAtLeast(0),
        )
    }

    fun write(context: Context, titleId: String, stats: PlayStats) {
        if (!GameLookup.isTitleId(titleId)) return
        val target = file(context, titleId)
        runCatching {
            target.parentFile?.mkdirs()
            val temp = File(target.parentFile, "${target.name}.tmp")
            temp.writeText(
                JSONObject()
                    .put("seconds", stats.seconds)
                    .put("last_played", stats.lastPlayed)
                    .put("sessions", stats.sessions)
                    .toString()
            )
            // The game process can be killed at any time; a rename never leaves half a file.
            if (!temp.renameTo(target)) temp.delete()
        }
    }

    /** "3 h 20 min", "12 min", or null under a minute. */
    fun format(context: Context, seconds: Long): String? {
        val minutes = seconds / 60
        if (minutes < 1) return null
        val hours = minutes / 60
        return if (hours > 0) {
            context.getString(org.capyps5.emulator.R.string.play_time_hours, hours, minutes % 60)
        } else {
            context.getString(org.capyps5.emulator.R.string.play_time_minutes, minutes)
        }
    }
}

/**
 * Measures one game session while the game is on screen. Written at every pause and every minute,
 * so a crash loses at most a minute.
 */
class PlaySession(private val context: Context, private val titleId: String) {
    private val before = PlayTime.read(context, titleId) ?: PlayStats(0, 0, 0)
    private var playedMs = 0L
    private var resumedAt = -1L
    private var started = false

    fun resume() {
        if (resumedAt >= 0) return
        resumedAt = SystemClock.elapsedRealtime()
        started = true
        save()
    }

    fun pause() {
        if (resumedAt < 0) return
        playedMs += SystemClock.elapsedRealtime() - resumedAt
        resumedAt = -1
        save()
    }

    fun save() {
        if (!started) return
        val running = if (resumedAt >= 0) SystemClock.elapsedRealtime() - resumedAt else 0L
        PlayTime.write(
            context, titleId,
            PlayStats(
                seconds = before.seconds + (playedMs + running) / 1000,
                lastPlayed = System.currentTimeMillis(),
                sessions = before.sessions + 1,
            ),
        )
    }
}
