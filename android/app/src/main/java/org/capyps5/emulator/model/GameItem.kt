// SPDX-FileCopyrightText: Copyright 2026 CapyPS5 Project
// SPDX-License-Identifier: GPL-2.0-only

package org.capyps5.emulator.model

import java.io.File

data class GameItem(
    val titleId: String,
    val titleName: String,
    val directoryPath: String,
    val elfPath: String,
    val iconPath: String? = null,
    val backgroundPath: String? = null,
    val version: String? = null,
    /** True for PS4 games (sce_sys/param.sfo), which run through the backward compatibility layer. */
    val isPs4: Boolean = false,
) {
    /** Unreal Engine build (UE4/UE5 command-line file or engine folder in app0). */
    val isUnreal: Boolean get() = isUnrealDir(File(directoryPath))

    companion object {
        fun isUnrealDir(dir: File): Boolean =
            File(dir, "ue4commandline.txt").isFile || File(dir, "uecommandline.txt").isFile ||
                File(dir, "engine").isDirectory || File(dir, "Engine").isDirectory

        /** Builds an item from a dumped game folder (eboot.bin + sce_sys/param.json). */
        fun fromGameDir(dir: File, elf: File = File(dir, "eboot.bin")): GameItem {
            var titleId = dir.name
            var titleName = dir.name
            var version: String? = null
            val sceSys = File(dir, "sce_sys")
            val paramJson = File(sceSys, "param.json")
            val paramSfo = File(sceSys, "param.sfo")
            if (!paramJson.exists() && paramSfo.exists()) {
                readSfo(paramSfo)?.let { sfo ->
                    titleId = (sfo["TITLE_ID"] as? String)?.ifEmpty { null } ?: titleId
                    version = (sfo["APP_VER"] as? String)?.ifEmpty { null }
                    // TITLE_xx holds the title per system language (01 English, 03 Spanish...).
                    val localized = SFO_LANGUAGES[java.util.Locale.getDefault().language]
                        ?.let { sfo["TITLE_%02d".format(it)] as? String }
                    titleName = localized?.ifEmpty { null } ?: (sfo["TITLE"] as? String)?.ifEmpty { null } ?: titleName
                }
                return GameItem(
                    titleId = titleId,
                    titleName = titleName,
                    directoryPath = dir.absolutePath,
                    elfPath = elf.absolutePath,
                    iconPath = File(sceSys, "icon0.png").takeIf { it.exists() }?.absolutePath,
                    backgroundPath = File(sceSys, "pic1.png").takeIf { it.exists() }?.absolutePath
                        ?: File(sceSys, "pic0.png").takeIf { it.exists() }?.absolutePath,
                    version = version,
                    isPs4 = true,
                )
            }
            if (paramJson.exists()) {
                try {
                    val json = org.json.JSONObject(paramJson.readText())
                    titleId = json.optString("titleId", titleId)
                    version = json.optString("contentVersion").ifEmpty { null }
                    val loc = json.optJSONObject("localizedParameters")
                    // Prefer the device language (es-ES, then any es-*), then the game's default.
                    val device = java.util.Locale.getDefault()
                    val lang = loc?.let { l ->
                        l.optJSONObject(device.toLanguageTag())
                            ?: l.keys().asSequence().firstOrNull { it.startsWith(device.language + "-") }
                                ?.let(l::optJSONObject)
                    }
                        ?: loc?.optString("defaultLanguage")?.let { loc.optJSONObject(it) }
                        ?: loc?.optJSONObject("en-US")
                    titleName = lang?.optString("titleName")?.ifEmpty { null } ?: titleName
                } catch (_: Exception) {
                }
            }
            return GameItem(
                titleId = titleId,
                titleName = titleName,
                directoryPath = dir.absolutePath,
                elfPath = elf.absolutePath,
                iconPath = File(sceSys, "icon0.png").takeIf { it.exists() }?.absolutePath,
                backgroundPath = File(sceSys, "pic0.png").takeIf { it.exists() }?.absolutePath,
                version = version,
            )
        }

        /** PSF language indices used by TITLE_xx, by ISO language code. */
        private val SFO_LANGUAGES = mapOf(
            "ja" to 0, "en" to 1, "fr" to 2, "es" to 3, "de" to 4, "it" to 5, "nl" to 6, "pt" to 7,
            "ru" to 8, "ko" to 9, "fi" to 12, "sv" to 13, "da" to 14, "no" to 15, "pl" to 16,
            "tr" to 19, "ar" to 21, "cs" to 23, "hu" to 24, "el" to 25, "ro" to 26, "th" to 27,
            "vi" to 28, "id" to 29,
        )

        /** Reads a binary PSF (param.sfo): key name to String or Int. */
        private fun readSfo(file: File): Map<String, Any>? = runCatching {
            val bytes = file.readBytes()
            val buf = java.nio.ByteBuffer.wrap(bytes).order(java.nio.ByteOrder.LITTLE_ENDIAN)
            if (bytes.size < 20 || buf.getInt(0) != 0x46535000) return null
            val keys = buf.getInt(8)
            val data = buf.getInt(12)
            val count = buf.getInt(16)
            if (count !in 0..1024 || 20 + count * 16 > bytes.size) return null
            val result = mutableMapOf<String, Any>()
            for (i in 0 until count) {
                val entry = 20 + i * 16
                val keyAt = keys + (buf.getShort(entry).toInt() and 0xffff)
                val format = buf.getShort(entry + 2).toInt() and 0xffff
                val length = buf.getInt(entry + 4)
                val dataAt = data + buf.getInt(entry + 12)
                if (keyAt !in bytes.indices || dataAt < 0 || length < 0 || dataAt + length > bytes.size) continue
                var keyEnd = keyAt
                while (keyEnd < bytes.size && bytes[keyEnd] != 0.toByte()) keyEnd++
                val name = String(bytes, keyAt, keyEnd - keyAt, Charsets.UTF_8)
                when (format) {
                    0x0404 -> if (length >= 4) result[name] = buf.getInt(dataAt)
                    0x0204, 0x0004 -> {
                        var end = dataAt
                        while (end < dataAt + length && bytes[end] != 0.toByte()) end++
                        result[name] = String(bytes, dataAt, end - dataAt, Charsets.UTF_8)
                    }
                }
            }
            result
        }.getOrNull()

        /** Scans a root folder for game directories and loose executables. */
        fun scan(root: File): List<GameItem> {
            val entries = root.listFiles() ?: return emptyList()
            return entries.sortedBy { it.name.lowercase() }.mapNotNull { f ->
                when {
                    f.isDirectory && File(f, "eboot.bin").exists() -> fromGameDir(f)
                    f.isFile && f.extension.lowercase() in setOf("elf", "self") ->
                        GameItem(f.nameWithoutExtension, f.name, f.parent ?: root.absolutePath, f.absolutePath)
                    else -> null
                }
            }.sortedBy { it.titleName.lowercase() }
        }
    }
}
