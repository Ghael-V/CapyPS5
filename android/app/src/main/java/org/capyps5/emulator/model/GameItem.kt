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
