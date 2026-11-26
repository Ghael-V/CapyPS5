// SPDX-FileCopyrightText: Copyright 2026 CapyPS5 Project
// SPDX-License-Identifier: GPL-2.0-only

package org.capyps5.emulator.library

import android.content.Context
import android.net.Uri
import java.io.File
import java.io.IOException
import java.io.OutputStream
import java.util.zip.ZipEntry
import java.util.zip.ZipInputStream
import java.util.zip.ZipOutputStream

/**
 * Saved data of one game, files/_SaveData/<title id>/…, exported and imported as a ZIP whose only
 * top folder is the title id.
 */
object SaveData {
    data class Info(val sizeBytes: Long, val updatedAt: Long)

    private const val MAX_ENTRIES = 100_000
    private const val MAX_BYTES = 8L shl 30

    fun dir(context: Context, titleId: String) = File(context.filesDir, "_SaveData/$titleId")

    /** Null when the game has no saved data. */
    fun info(context: Context, titleId: String): Info? {
        if (!GameLookup.isTitleId(titleId)) return null
        val files = dir(context, titleId).walkTopDown().filter { it.isFile }.toList()
        if (files.isEmpty()) return null
        return Info(files.sumOf { it.length() }, files.maxOf { it.lastModified() })
    }

    fun export(context: Context, titleId: String, out: OutputStream) {
        require(GameLookup.isTitleId(titleId))
        val root = dir(context, titleId)
        ZipOutputStream(out.buffered()).use { zip ->
            root.walkTopDown().filter { it != root }.sortedBy { it.path }.forEach { file ->
                val relative = file.relativeTo(root).invariantSeparatorsPath
                if (file.isDirectory) {
                    zip.putNextEntry(ZipEntry("$titleId/$relative/"))
                } else {
                    zip.putNextEntry(ZipEntry("$titleId/$relative").apply { time = file.lastModified() })
                    file.inputStream().use { it.copyTo(zip) }
                }
                zip.closeEntry()
            }
        }
    }

    /** Title id a saves ZIP belongs to, or null when it is not one (several or no top folders). */
    fun zipTitleId(context: Context, uri: Uri): String? = runCatching {
        var titleId: String? = null
        var count = 0
        context.contentResolver.openInputStream(uri)?.use { input ->
            ZipInputStream(input.buffered()).use { zip ->
                while (true) {
                    val entry = zip.nextEntry ?: break
                    val top = safeName(entry.name)?.substringBefore('/') ?: return null
                    if (!GameLookup.isTitleId(top) || (titleId != null && titleId != top)) return null
                    titleId = top
                    if (++count > MAX_ENTRIES) return null
                }
            }
        }
        titleId
    }.getOrNull()

    /**
     * Replaces the game's saved data with the ZIP's. The ZIP is unpacked next to the saves first,
     * so a broken file leaves the current saves as they were.
     */
    fun import(context: Context, titleId: String, uri: Uri) {
        require(GameLookup.isTitleId(titleId))
        val target = dir(context, titleId)
        val base = target.parentFile ?: throw IOException("no save folder")
        base.mkdirs()
        val staging = File(base, ".import-$titleId")
        val old = File(base, ".old-$titleId")
        staging.deleteRecursively()
        old.deleteRecursively()
        try {
            unpack(context, titleId, uri, staging)
            val unpacked = File(staging, titleId)
            if (!unpacked.isDirectory) throw IOException("empty file")
            if (target.exists() && !target.renameTo(old)) throw IOException("cannot move the current saves")
            if (!unpacked.renameTo(target)) {
                old.renameTo(target)
                throw IOException("cannot place the new saves")
            }
            old.deleteRecursively()
        } finally {
            staging.deleteRecursively()
        }
    }

    private fun unpack(context: Context, titleId: String, uri: Uri, staging: File) {
        val stagingPath = staging.canonicalPath + File.separator
        var count = 0
        var bytes = 0L
        val input = context.contentResolver.openInputStream(uri) ?: throw IOException("cannot open the file")
        input.use {
            ZipInputStream(it.buffered()).use { zip ->
                while (true) {
                    val entry = zip.nextEntry ?: break
                    val name = safeName(entry.name) ?: throw IOException("bad entry")
                    if (name != titleId && !name.startsWith("$titleId/")) throw IOException("another game's entry")
                    if (++count > MAX_ENTRIES) throw IOException("too many entries")
                    val out = File(staging, name)
                    if (!(out.canonicalPath + File.separator).startsWith(stagingPath)) throw IOException("bad entry")
                    if (entry.isDirectory) {
                        out.mkdirs()
                        continue
                    }
                    out.parentFile?.mkdirs()
                    out.outputStream().use { file ->
                        val buffer = ByteArray(64 * 1024)
                        while (true) {
                            val read = zip.read(buffer)
                            if (read < 0) break
                            bytes += read
                            if (bytes > MAX_BYTES) throw IOException("file too large")
                            file.write(buffer, 0, read)
                        }
                    }
                    if (entry.time > 0) out.setLastModified(entry.time)
                }
            }
        }
    }

    /** The entry name without a trailing slash; null for absolute, empty or "..", or "\\" names. */
    private fun safeName(raw: String): String? {
        val name = raw.trimEnd('/')
        if (name.isEmpty() || name.startsWith('/') || '\\' in name || ':' in name) return null
        if (name.split('/').any { it.isEmpty() || it == "." || it == ".." }) return null
        return name
    }
}
