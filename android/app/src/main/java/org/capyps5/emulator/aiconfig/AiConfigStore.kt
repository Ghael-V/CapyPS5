// SPDX-FileCopyrightText: Copyright 2026 CapyPS5 Project
// SPDX-License-Identifier: GPL-2.0-only

package org.capyps5.emulator.aiconfig

import android.app.ActivityManager
import android.content.Context
import android.os.Handler
import android.os.Looper
import android.os.Process
import android.os.SystemClock
import android.util.Log
import org.capyps5.emulator.CoreOption
import org.capyps5.emulator.Settings
import org.capyps5.emulator.model.GameItem
import org.json.JSONObject
import java.io.File

/**
 * Applies a recommended configuration to a game's own settings and keeps what is needed to take it
 * back. Files live in files/ai_config/, named after the title id:
 * - `<id>.backup.json`: before the first recommendation, the game's own value of every key the
 *   recommendations may change (core options and hidden switches; null = follows the global value).
 *   Later recommendations keep that first copy, so undo always returns to the settings before any
 *   AI change.
 * - `<id>.json`: the state. A configuration is unconfirmed (applied) until a launch on it has shown
 *   frames for 90 s, or the player left a game that had shown frames. While unconfirmed:
 *   - the game process (GameActivity, ":game") marks it "launching" with its pid when the game
 *     starts, watches the first presented frame, and at 90 s confirms it or, without a single frame,
 *     marks it failed ("no_boot"); leaving after a fatal error marks it failed too;
 *   - the launcher process restores the copy for a failed state, and for a "launching" one whose
 *     process is gone (the game crashed, or the whole app died: then on the next start), reports
 *     the outcome to the server and tells the player once.
 * Settings (SharedPreferences) belong to the launcher process; the game process only writes the
 * state file.
 */
object AiConfigStore {
    private const val TAG = "CapyPS5-AiConfig"
    private const val CONFIRM_AFTER_MS = 90_000L
    private const val POLL_MS = 2_000L

    const val APPLIED = "applied"
    const val LAUNCHING = "launching"
    const val CONFIRMED = "confirmed"
    const val FAILED = "failed"
    const val REVERTED = "reverted"

    private val titleIdFormat = Regex("^[A-Z]{4}[0-9]{5}$")

    /** Recommendations need a PlayStation title id (PPSA01234, CUSA01234...). */
    fun supports(game: GameItem) = titleIdFormat.matches(game.titleId)

    class State(
        val answerId: String,
        val appliedAt: Long,
        val state: String,
        val titleName: String,
        val pid: Int = 0,
        val presented: Boolean = false,
        /** crashed or no_boot, for a failed or reverted state. */
        val outcome: String = "",
    ) {
        fun copy(
            state: String = this.state,
            pid: Int = this.pid,
            presented: Boolean = this.presented,
            outcome: String = this.outcome,
        ) = State(answerId, appliedAt, state, titleName, pid, presented, outcome)

        fun toJson(): String = JSONObject()
            .put("answer_id", answerId).put("applied_at", appliedAt).put("state", state)
            .put("title_name", titleName).put("pid", pid).put("presented", presented).put("outcome", outcome)
            .toString()

        companion object {
            fun fromJson(text: String): State {
                val o = JSONObject(text)
                return State(
                    o.optString("answer_id"), o.optLong("applied_at"), o.getString("state"),
                    o.optString("title_name"), o.optInt("pid"), o.optBoolean("presented"), o.optString("outcome"),
                )
            }
        }
    }

    /** A game whose recommended settings were undone automatically; shown to the player once. */
    class Notice(val titleId: String, val titleName: String, val outcome: String)

    private fun dir(context: Context) = File(context.filesDir, "ai_config").apply { mkdirs() }
    private fun stateFile(context: Context, titleId: String) = File(dir(context), "$titleId.json")
    private fun backupFile(context: Context, titleId: String) = File(dir(context), "$titleId.backup.json")

    private fun read(file: File): State? = runCatching { State.fromJson(file.readText()) }.getOrNull()

    private fun write(file: File, text: String) {
        val tmp = File(file.parentFile, file.name + ".tmp")
        tmp.writeText(text)
        if (!tmp.renameTo(file)) {
            file.writeText(text)
            tmp.delete()
        }
    }

    fun state(context: Context, titleId: String): State? = read(stateFile(context, titleId))

    /** True while an AI change can be undone for [titleId]. */
    fun hasActive(context: Context, titleId: String): Boolean =
        state(context, titleId)?.let { it.state != REVERTED } ?: false

    private fun coreOption(key: String): CoreOption? =
        CoreOption.entries.firstOrNull { it.property == AiSchema.PREFIX + key }

    /** The game's own value of [key], or null when it follows the global value / the core default. */
    fun gameValue(settings: Settings, titleId: String, key: String): Long? {
        val option = coreOption(key)
        return if (option != null) settings.gameOption(titleId, option) else settings.gameHiddenOption(titleId, key)
    }

    private fun setGameValue(settings: Settings, titleId: String, key: String, value: Long?) {
        val option = coreOption(key)
        if (option != null) settings.updateGameOption(titleId, option, value)
        else settings.updateGameHiddenOption(titleId, key, value)
    }

    /** The value [game] runs [entry] with. */
    fun effectiveValue(settings: Settings, game: GameItem, entry: AiSchema.Entry): Long {
        val option = coreOption(entry.key)
        return if (option != null) settings.effectiveOption(game.titleId, option, game.isUnreal)
        else settings.gameHiddenOption(game.titleId, entry.key) ?: entry.default
    }

    /** Writes [values] (key -> value, already checked against [schema]) as [game]'s own settings. Launcher process. */
    @Synchronized
    fun apply(context: Context, settings: Settings, schema: AiSchema, game: GameItem, answerId: String, values: Map<String, Long>) {
        val backup = backupFile(context, game.titleId)
        val previous = state(context, game.titleId)
        if (!backup.exists() || previous == null || previous.state == REVERTED) {
            val copy = JSONObject()
            schema.settings.forEach { entry ->
                copy.put(entry.key, gameValue(settings, game.titleId, entry.key) ?: JSONObject.NULL)
            }
            write(backup, copy.toString())
        }
        values.forEach { (key, value) -> setGameValue(settings, game.titleId, key, value) }
        write(stateFile(context, game.titleId), State(answerId, System.currentTimeMillis(), APPLIED, game.titleName).toJson())
        Log.i(TAG, "Applied ${values.size} settings to ${game.titleId}")
    }

    /** Puts [titleId]'s settings back as they were before the AI changes. Launcher process. */
    @Synchronized
    fun undo(context: Context, settings: Settings, titleId: String) {
        val answerId = restore(context, settings, titleId)
        if (answerId != null) report(answerId, AiConfigClient.Outcome.REVERTED)
    }

    /** Forgets the AI change without touching the settings (the player reset the game's settings). */
    @Synchronized
    fun forget(context: Context, titleId: String) {
        backupFile(context, titleId).delete()
        stateFile(context, titleId).delete()
    }

    /** Restores the copy (core options and hidden switches) and drops the AI files. Returns the undone answer id. */
    private fun restore(context: Context, settings: Settings, titleId: String): String? {
        val state = state(context, titleId)
        val backup = backupFile(context, titleId)
        if (backup.exists()) {
            runCatching {
                val copy = JSONObject(backup.readText())
                // The copy's own keys, not the current schema's: an update may have dropped a key.
                copy.keys().forEach { key ->
                    setGameValue(settings, titleId, key, if (copy.isNull(key)) null else copy.getLong(key))
                }
            }.onFailure { Log.w(TAG, "Backup of $titleId unreadable: ${it.message}") }
            backup.delete()
        }
        stateFile(context, titleId).delete()
        Log.i(TAG, "Restored the settings of $titleId from before the AI change")
        return state?.answerId
    }

    private fun report(answerId: String, outcome: AiConfigClient.Outcome) {
        Thread({ AiConfigClient.feedback(answerId, outcome) }, "AiConfigFeedback").start()
    }

    private fun processAlive(context: Context, pid: Int): Boolean {
        if (pid <= 0) return false
        val manager = context.getSystemService(ActivityManager::class.java) ?: return true
        return manager.runningAppProcesses?.any { it.pid == pid } ?: true
    }

    /**
     * Launcher process, when it starts or comes back and before a game launches: undoes the
     * recommendations a launch failed on (fatal error, crash, killed, or no frame in 90 s), reports
     * them and returns every notice the player has not seen yet.
     */
    @Synchronized
    fun recover(context: Context, settings: Settings): List<Notice> {
        val notices = mutableListOf<Notice>()
        val files = dir(context).listFiles { f -> f.name.endsWith(".json") && !f.name.endsWith(".backup.json") }
            ?: return notices
        for (file in files) {
            val titleId = file.name.removeSuffix(".json")
            val state = read(file) ?: continue
            val outcome = when {
                state.state == FAILED -> state.outcome.ifEmpty { AiConfigClient.Outcome.CRASHED.wire }
                state.state == LAUNCHING && !processAlive(context, state.pid) ->
                    if (state.presented) AiConfigClient.Outcome.CRASHED.wire else AiConfigClient.Outcome.NO_BOOT.wire
                else -> null
            }
            if (outcome != null) {
                Log.w(TAG, "$titleId failed with the recommended settings ($outcome); undoing them")
                restore(context, settings, titleId)
                write(file, state.copy(state = REVERTED, pid = 0, outcome = outcome).toJson())
                report(state.answerId, if (outcome == AiConfigClient.Outcome.NO_BOOT.wire) AiConfigClient.Outcome.NO_BOOT else AiConfigClient.Outcome.CRASHED)
            }
            val now = if (outcome != null) read(file) else state
            if (now?.state == REVERTED) notices += Notice(titleId, now.titleName, now.outcome)
        }
        return notices
    }

    /** The player has seen these notices. */
    @Synchronized
    fun clearNotices(context: Context, titleIds: Collection<String>) {
        titleIds.forEach { id ->
            val file = stateFile(context, id)
            if (read(file)?.state == REVERTED) file.delete()
        }
    }

    // ---- Game process ----

    private val handler by lazy { Handler(Looper.getMainLooper()) }
    private var watched: String? = null
    private var startedAt = 0L
    private var framesPresented: () -> Long = { 0L }

    /**
     * Game process, once the game's options are set: arms the guard when the game starts on an
     * unconfirmed recommendation. [frames] returns the number of frames the game has presented.
     */
    fun onGameStarted(context: Context, titleId: String, frames: () -> Long) {
        if (!titleIdFormat.matches(titleId)) return
        val file = stateFile(context, titleId)
        val state = read(file) ?: return
        // A "launching" state left by an earlier process the launcher has not looked at yet is unconfirmed too.
        if (state.state != APPLIED && state.state != LAUNCHING) return
        write(file, state.copy(state = LAUNCHING, pid = Process.myPid(), presented = false).toJson())
        watched = titleId
        startedAt = SystemClock.elapsedRealtime()
        framesPresented = frames
        handler.removeCallbacksAndMessages(null)
        handler.postDelayed(object : Runnable {
            override fun run() {
                if (watched != titleId) return
                val now = read(file) ?: return
                if (now.state != LAUNCHING || now.pid != Process.myPid()) return
                val presented = now.presented || runCatching { framesPresented() }.getOrDefault(0L) > 0
                if (SystemClock.elapsedRealtime() - startedAt >= CONFIRM_AFTER_MS) {
                    watched = null
                    if (presented) {
                        write(file, now.copy(state = CONFIRMED, pid = 0, presented = true).toJson())
                        Log.i(TAG, "$titleId ran fine with the recommended settings")
                    } else {
                        write(file, now.copy(state = FAILED, pid = 0, outcome = AiConfigClient.Outcome.NO_BOOT.wire).toJson())
                        Log.w(TAG, "$titleId showed no frame in 90 s with the recommended settings")
                    }
                    return
                }
                if (presented && !now.presented) write(file, now.copy(presented = true).toJson())
                handler.postDelayed(this, POLL_MS)
            }
        }, POLL_MS)
        Log.i(TAG, "$titleId starts with unconfirmed recommended settings; watching it")
    }

    /**
     * Game process, when the player leaves the game ([fatal]: after a fatal error). A normal exit
     * after the game showed frames confirms the settings; an exit before any frame is no verdict.
     */
    fun onGameExit(context: Context, fatal: Boolean) {
        val titleId = watched ?: return
        watched = null
        handler.removeCallbacksAndMessages(null)
        val file = stateFile(context, titleId)
        val state = read(file) ?: return
        if (state.state != LAUNCHING || state.pid != Process.myPid()) return
        val presented = state.presented || runCatching { framesPresented() }.getOrDefault(0L) > 0
        val next = when {
            fatal -> state.copy(
                state = FAILED, pid = 0, presented = presented,
                outcome = if (presented) AiConfigClient.Outcome.CRASHED.wire else AiConfigClient.Outcome.NO_BOOT.wire,
            )
            presented -> state.copy(state = CONFIRMED, pid = 0, presented = true)
            else -> state.copy(state = APPLIED, pid = 0)
        }
        write(file, next.toJson())
    }
}
