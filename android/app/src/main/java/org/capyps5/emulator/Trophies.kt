// SPDX-FileCopyrightText: Copyright 2026 CapyPS5 Project
// SPDX-License-Identifier: GPL-2.0-only

package org.capyps5.emulator

import android.content.Context
import org.json.JSONArray
import org.json.JSONObject
import java.io.File
import java.io.RandomAccessFile
import java.util.Locale

/** One trophy of a game, with its unlock time when earned (epoch milliseconds). */
data class Trophy(
    val id: Int,
    val grade: Grade,
    val name: String,
    val detail: String,
    val hidden: Boolean,
    val group: Int,
    val unlockedAt: Long?,
    /** Byte range of trophy icon (trop0000.png) inside the package. */
    val icon: LongRange?,
) {
    val unlocked: Boolean get() = unlockedAt != null
}

enum class Grade { Platinum, Gold, Silver, Bronze }

data class TrophySet(val title: String, val packagePath: String, val trophies: List<Trophy>) {
    val earned: Int get() = trophies.count { it.unlocked }
    val percent: Int get() = if (trophies.isEmpty()) 0 else earned * 100 / trophies.size
}

/**
 * Reads the trophy list a game ships in sce_sys/trophy2/trophyNN.ucp and the unlocks the core
 * records in files/_Trophies/<title id>/trophies_<user>_<service>.json.
 */
object Trophies {
    private const val MAGIC = 0xb228c60aL
    private const val MAX_ENTRIES = 4096

    /** Microseconds from 0001-01-01 to the Unix epoch: the base of the core's timestamps. */
    private const val UNIX_EPOCH_TICK = 62135596800000000L

    fun packageFile(gameDir: String, service: Int = 0): File =
        File(gameDir, "sce_sys/trophy2/trophy%02d.ucp".format(service))

    fun hasTrophies(gameDir: String): Boolean = packageFile(gameDir).isFile

    /** Returns null when the game has no trophy package or it cannot be read. */
    fun load(context: Context, gameDir: String, titleId: String, service: Int = 0): TrophySet? {
        val file = packageFile(gameDir, service)
        if (!file.isFile) return null
        return runCatching {
            RandomAccessFile(file, "r").use { raf ->
                val entries = readToc(raf) ?: return null
                fun json(name: String): JSONObject? =
                    entries[name]?.let { JSONObject(String(readRange(raf, it), Charsets.UTF_8)) }

                val conf = json("tropconf.json") ?: return null
                val meta = metadataFor(entries.keys, conf.optString("defaultLanguage"))?.let(::json)
                    ?.optJSONObject("metadata") ?: return null
                val texts = mutableMapOf<Int, JSONObject>()
                meta.optJSONArray("trophyMetadata")?.forEachObject { texts[id(it.opt("id"))] = it }
                val unlocks = loadUnlocks(context, titleId, service)

                val trophies = mutableListOf<Trophy>()
                conf.optJSONArray("trophies")?.forEachObject { def ->
                    val id = id(def.opt("id"))
                    val grade = when (def.optString("grade")) {
                        "P" -> Grade.Platinum
                        "G" -> Grade.Gold
                        "S" -> Grade.Silver
                        "B" -> Grade.Bronze
                        else -> null
                    }
                    if (id < 0 || grade == null) return@forEachObject
                    val text = texts[id]
                    trophies += Trophy(
                        id = id,
                        grade = grade,
                        name = text?.optString("name").orEmpty(),
                        detail = text?.optString("detail").orEmpty(),
                        hidden = def.optBoolean("hidden"),
                        group = if (def.has("groupId")) id(def.opt("groupId")) else -1,
                        unlockedAt = unlocks[id],
                        icon = entries["trop%04d.png".format(id)],
                    )
                }
                val title = meta.optJSONObject("titleMetadata")?.optString("name").orEmpty()
                TrophySet(title, file.absolutePath, trophies.sortedBy { it.id })
            }
        }.getOrNull()
    }

    fun readIcon(set: TrophySet, trophy: Trophy): ByteArray? {
        val range = trophy.icon ?: return null
        return runCatching { RandomAccessFile(set.packagePath, "r").use { readRange(it, range) } }.getOrNull()
    }

    /** Unlocks of every user that played on this device, keyed by trophy id. */
    private fun loadUnlocks(context: Context, titleId: String, service: Int): Map<Int, Long> {
        val dir = File(context.filesDir, "_Trophies/$titleId")
        val files = dir.listFiles { f -> f.name.startsWith("trophies_") && f.name.endsWith("_$service.json") }
            ?: return emptyMap()
        val result = mutableMapOf<Int, Long>()
        for (file in files) {
            val json = runCatching { JSONObject(file.readText()) }.getOrNull() ?: continue
            val times = json.optJSONObject("unlockedAt")
            val ids = json.optJSONArray("unlockedTrophies") ?: continue
            for (i in 0 until ids.length()) {
                val id = id(ids.opt(i))
                if (id < 0) continue
                val tick = times?.optLong(id.toString(), 0L) ?: 0L
                val millis = if (tick > UNIX_EPOCH_TICK) (tick - UNIX_EPOCH_TICK) / 1000 else 0L
                result[id] = minOf(result[id] ?: Long.MAX_VALUE, millis)
            }
        }
        return result
    }

    /** The device language first (es-ES, then any es-*), then the game's default, then English. */
    private fun metadataFor(names: Set<String>, defaultLanguage: String): String? {
        val device = Locale.getDefault()
        val candidates = listOf(device.toLanguageTag(), "${device.language}-${device.country}")
        return candidates.map { "tropmeta_$it.json" }.firstOrNull { it in names }
            ?: names.sorted().firstOrNull { it.startsWith("tropmeta_${device.language}-") }
            ?: "tropmeta_$defaultLanguage.json".takeIf { it in names }
            ?: "tropmeta_en-US.json".takeIf { it in names }
            ?: names.firstOrNull { it.startsWith("tropmeta_") }
    }

    /** File table of the package: name to byte range. Big-endian header, 0x40-byte entries. */
    private fun readToc(raf: RandomAccessFile): Map<String, LongRange>? {
        if (raf.length() < 0x40) return null
        raf.seek(0)
        if (raf.readInt().toLong() and 0xffffffffL != MAGIC || raf.readInt() != 1) return null
        val size = raf.readLong()
        val count = raf.readInt()
        val toc = raf.readInt().toLong() and 0xffffffffL
        if (size < 0x40 || size > raf.length() || count !in 0..MAX_ENTRIES || toc > size ||
            0x20L + count * 0x40L > size - toc
        ) return null
        val entries = mutableMapOf<String, LongRange>()
        val entry = ByteArray(0x40)
        for (i in 0 until count) {
            raf.seek(toc + 0x20 + i * 0x40L)
            raf.readFully(entry)
            val end = entry.indexOf(0).let { if (it in 0 until 0x20) it else 0x20 }
            val name = String(entry, 0, end, Charsets.US_ASCII)
            val offset = java.nio.ByteBuffer.wrap(entry, 0x20, 8).long
            val length = java.nio.ByteBuffer.wrap(entry, 0x28, 8).long
            if (offset < 0 || length < 0 || offset > size || length > size - offset) return null
            entries[name] = offset until offset + length
        }
        return entries
    }

    private fun readRange(raf: RandomAccessFile, range: LongRange): ByteArray {
        val bytes = ByteArray((range.last - range.first + 1).toInt())
        raf.seek(range.first)
        raf.readFully(bytes)
        return bytes
    }

    private fun id(value: Any?): Int = when (value) {
        is Number -> value.toInt().takeIf { it >= 0 } ?: -1
        is String -> value.toIntOrNull()?.takeIf { it >= 0 } ?: -1
        else -> -1
    }

    private inline fun JSONArray.forEachObject(block: (JSONObject) -> Unit) {
        for (i in 0 until length()) optJSONObject(i)?.let(block)
    }
}
