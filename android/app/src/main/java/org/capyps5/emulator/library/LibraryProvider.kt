// SPDX-FileCopyrightText: Copyright 2026 CapyPS5 Project
// SPDX-License-Identifier: GPL-2.0-only

package org.capyps5.emulator.library

import android.content.ContentProvider
import android.content.ContentValues
import android.content.Context
import android.database.Cursor
import android.database.MatrixCursor
import android.net.Uri
import android.os.ParcelFileDescriptor
import org.capyps5.emulator.Grade
import org.capyps5.emulator.R
import org.capyps5.emulator.Trophies
import org.capyps5.emulator.model.GameItem
import java.io.File
import java.io.FileNotFoundException

/**
 * Read-only view of one library game for frontends, at content://<applicationId>.library:
 *
 * - title?title_id=… | path=… : play time and trophy counts
 * - achievements?… : one row per trophy; achievement_icon/<title id>/<id> is its PNG
 * - saves?… : whether there is saved data; save_zip/<title id> is it as a ZIP
 *   (save_zip needs a signature permission: other apps get a one-time grant through EXPORT_SAVES)
 * - screenshots?… : always empty (the emulator takes none)
 *
 * It answers only for a game the caller names, never lists the library and never writes. Unknown
 * games and missing data give empty cursors or null columns; only openFile throws.
 */
class LibraryProvider : ContentProvider() {

    override fun onCreate(): Boolean = true

    private val authority get() = "${requireContext().packageName}.library"

    override fun query(
        uri: Uri,
        projection: Array<out String>?,
        selection: String?,
        selectionArgs: Array<out String>?,
        sortOrder: String?,
    ): Cursor {
        val context = requireContext()
        val kind = uri.pathSegments.singleOrNull()
        val columns = when (kind) {
            "title" -> TITLE_COLUMNS
            "achievements" -> ACHIEVEMENT_COLUMNS
            "saves" -> SAVE_COLUMNS
            "screenshots" -> SCREENSHOT_COLUMNS
            else -> emptyArray()
        }
        val cursor = MatrixCursor(columns)
        if (kind == "title") cursor.setNotificationUri(context.contentResolver, titleUri(context))
        runCatching {
            val game = GameLookup.find(context, uri.getQueryParameter("title_id"), uri.getQueryParameter("path"))
                ?: return cursor
            when (kind) {
                "title" -> addTitle(context, game, cursor)
                "achievements" -> addAchievements(context, game, cursor)
                "saves" -> addSaves(context, game, cursor)
            }
        }.onFailure { android.util.Log.w(TAG, "query $kind failed", it) }
        return cursor
    }

    private fun addTitle(context: Context, game: GameItem, cursor: MatrixCursor) {
        val play = PlayTime.read(context, game.titleId)
        val trophies = Trophies.load(context, game.directoryPath, game.titleId)
        cursor.addRow(
            arrayOf<Any?>(
                game.titleId,
                game.titleName,
                game.version,
                if (game.isPs4) "ps4" else "ps5",
                play?.seconds,
                play?.lastPlayed?.takeIf { it > 0 },
                play?.sessions,
                trophies?.trophies?.size,
                trophies?.earned,
            )
        )
    }

    private fun addAchievements(context: Context, game: GameItem, cursor: MatrixCursor) {
        val set = Trophies.load(context, game.directoryPath, game.titleId) ?: return
        val hiddenName = context.getString(R.string.trophy_hidden)
        for (trophy in set.trophies) {
            // A hidden trophy keeps its name and text secret until it is earned, as in the app.
            val secret = trophy.hidden && !trophy.unlocked
            cursor.addRow(
                arrayOf<Any?>(
                    trophy.id.toString(),
                    if (secret) hiddenName else trophy.name,
                    if (secret) null else trophy.detail.ifEmpty { null },
                    if (trophy.unlocked) 1 else 0,
                    trophy.unlockedAt?.takeIf { it > 0 },
                    when (trophy.grade) {
                        Grade.Platinum -> "platinum"
                        Grade.Gold -> "gold"
                        Grade.Silver -> "silver"
                        Grade.Bronze -> "bronze"
                    },
                    null,
                    if (trophy.hidden) 1 else 0,
                    trophy.icon?.takeIf { !secret }?.let { "content://$authority/achievement_icon/${game.titleId}/${trophy.id}" },
                )
            )
        }
    }

    private fun addSaves(context: Context, game: GameItem, cursor: MatrixCursor) {
        val info = SaveData.info(context, game.titleId)
        cursor.addRow(
            arrayOf<Any?>(
                if (info != null) 1 else 0,
                info?.sizeBytes ?: 0L,
                info?.updatedAt,
                info?.let { "content://$authority/save_zip/${game.titleId}" },
            )
        )
    }

    override fun openFile(uri: Uri, mode: String): ParcelFileDescriptor {
        if (mode != "r") throw FileNotFoundException("read only")
        val context = requireContext()
        val segments = uri.pathSegments
        val file = runCatching {
            when (segments.firstOrNull()) {
                "achievement_icon" -> if (segments.size == 3) trophyIcon(context, segments[1], segments[2]) else null
                "save_zip" -> if (segments.size == 2) saveZip(context, segments[1]) else null
                else -> null
            }
        }.onFailure { android.util.Log.w(TAG, "openFile failed", it) }.getOrNull()
            ?: throw FileNotFoundException(uri.toString())
        return ParcelFileDescriptor.open(file, ParcelFileDescriptor.MODE_READ_ONLY)
    }

    /** The trophy's PNG, copied out of the game's trophy package into the cache once. */
    private fun trophyIcon(context: Context, titleId: String, id: String): File? {
        if (!GameLookup.isTitleId(titleId) || !ICON_ID.matches(id)) return null
        val game = GameLookup.find(context, titleId, null) ?: return null
        val cached = File(context.cacheDir, "library_icons/$titleId/${id.toInt()}.png")
        val source = Trophies.packageFile(game.directoryPath)
        // Loaded every time: whether a hidden trophy's icon may be shown depends on its unlock.
        val set = Trophies.load(context, game.directoryPath, game.titleId) ?: return null
        val trophy = set.trophies.firstOrNull { it.id == id.toInt() } ?: return null
        if (trophy.hidden && !trophy.unlocked) return null
        if (cached.isFile && cached.lastModified() >= source.lastModified()) return cached
        val bytes = Trophies.readIcon(set, trophy) ?: return null
        cached.parentFile?.mkdirs()
        val temp = File(cached.parentFile, "${cached.name}.tmp")
        temp.writeBytes(bytes)
        if (!temp.renameTo(cached)) return null
        return cached
    }

    /** The game's saved data as a fresh ZIP in the cache; older exports are removed. */
    private fun saveZip(context: Context, titleId: String): File? {
        if (!GameLookup.isTitleId(titleId)) return null
        GameLookup.find(context, titleId, null) ?: return null
        SaveData.info(context, titleId) ?: return null
        val dir = File(context.cacheDir, "save_export").apply { mkdirs() }
        val stale = System.currentTimeMillis() - 60 * 60 * 1000L
        dir.listFiles()?.filter { it.lastModified() < stale }?.forEach { it.delete() }
        val zip = File(dir, "$titleId.zip")
        val temp = File(dir, "$titleId.zip.tmp")
        temp.outputStream().use { SaveData.export(context, titleId, it) }
        if (!temp.renameTo(zip)) return null
        return zip
    }

    override fun getType(uri: Uri): String? = when (uri.pathSegments.firstOrNull()) {
        "title", "achievements", "saves", "screenshots" -> "vnd.android.cursor.dir/vnd.$authority.${uri.pathSegments[0]}"
        "achievement_icon" -> "image/png"
        "save_zip" -> "application/zip"
        else -> null
    }

    override fun insert(uri: Uri, values: ContentValues?): Uri? = null

    override fun update(uri: Uri, values: ContentValues?, selection: String?, selectionArgs: Array<out String>?): Int = 0

    override fun delete(uri: Uri, selection: String?, selectionArgs: Array<out String>?): Int = 0

    companion object {
        private const val TAG = "CapyPS5-Library"
        private val ICON_ID = Regex("^[0-9]{1,4}$")

        private val TITLE_COLUMNS = arrayOf(
            "title_id", "name", "version", "platform",
            "play_seconds", "last_played", "sessions",
            "achievements_total", "achievements_unlocked",
        )
        private val ACHIEVEMENT_COLUMNS = arrayOf(
            "id", "name", "description", "unlocked", "unlocked_at", "grade", "points", "hidden", "icon_uri",
        )
        private val SAVE_COLUMNS = arrayOf("has_saves", "size_bytes", "updated_at", "zip_uri")
        private val SCREENSHOT_COLUMNS = arrayOf("uri", "taken_at", "width", "height")

        fun titleUri(context: Context): Uri = Uri.parse("content://${context.packageName}.library/title")

        /** Tells listening frontends that play time, trophies or saves of some game changed. */
        fun notifyChanged(context: Context) {
            runCatching { context.contentResolver.notifyChange(titleUri(context), null) }
        }
    }
}
