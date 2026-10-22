// SPDX-FileCopyrightText: Copyright 2026 CapyPS5 Project
// SPDX-License-Identifier: GPL-2.0-only

package org.capyps5.emulator

import android.content.Context
import android.net.Uri
import org.json.JSONObject
import java.io.File
import java.io.InputStream
import java.util.zip.ZipInputStream

/** A user-installed Vulkan driver (Turnip, PanVK...) loaded through libadrenotools. */
data class GpuDriver(
    val id: String,
    val name: String,
    val version: String,
    val vendor: String,
    val library: String,
    val dir: File,
) {
    val label: String get() = if (version.isBlank()) name else "$name $version"
}

/**
 * Drivers live in app-private storage (dlopen refuses world-writable paths), one folder per
 * driver. Packages use the AdrenoTools layout shared by other emulators: a zip with the driver
 * library and a meta.json naming it (`libraryName`, `name`, `driverVersion`, `vendor`).
 */
object GpuDrivers {
    private const val META = "meta.json"

    private fun root(context: Context) = File(context.filesDir, "gpu_drivers").apply { mkdirs() }

    fun list(context: Context): List<GpuDriver> =
        root(context).listFiles().orEmpty().filter { it.isDirectory }.mapNotNull(::read).sortedBy { it.label }

    fun find(context: Context, id: String?): GpuDriver? =
        if (id.isNullOrEmpty()) null else list(context).firstOrNull { it.id == id }

    fun delete(context: Context, id: String) {
        File(root(context), id).takeIf { it.isDirectory }?.deleteRecursively()
    }

    /** Installs a driver package; returns the driver or an error message. */
    fun import(context: Context, uri: Uri): Result<GpuDriver> =
        import(context) { context.contentResolver.openInputStream(uri) }

    /** Installs a downloaded driver package. */
    fun import(context: Context, file: File): Result<GpuDriver> = import(context) { file.inputStream() }

    private fun import(context: Context, open: () -> InputStream?): Result<GpuDriver> = runCatching {
        val staging = File(context.cacheDir, "driver_import").apply { deleteRecursively(); mkdirs() }
        try {
            val input = open() ?: error(context.getString(R.string.driver_error_open))
            ZipInputStream(input.buffered()).use { zip ->
                generateSequence { zip.nextEntry }.forEach { entry ->
                    // Flatten: packages keep their files at the top level; ignore any folders.
                    val name = File(entry.name).name
                    if (!entry.isDirectory && name.isNotEmpty() && !name.startsWith(".")) {
                        File(staging, name).outputStream().use { zip.copyTo(it) }
                    }
                }
            }
            val meta = File(staging, META).takeIf { it.isFile }?.let { JSONObject(it.readText()) }
            val libraries = staging.listFiles().orEmpty().filter { it.name.endsWith(".so") }
            val library = meta?.optString("libraryName")?.takeIf { it.isNotBlank() }
                ?: libraries.singleOrNull()?.name
                ?: error(context.getString(R.string.driver_error_no_library))
            if (!File(staging, library).isFile) error(context.getString(R.string.driver_error_missing, library))
            val name = meta?.optString("name")?.takeIf { it.isNotBlank() } ?: library.removeSuffix(".so")
            val version = meta?.optString("driverVersion").orEmpty()
            val id = "$name $version".trim().replace(Regex("[^A-Za-z0-9._-]+"), "_")
            val target = File(root(context), id)
            target.deleteRecursively()
            if (!staging.renameTo(target)) {
                staging.copyRecursively(target, overwrite = true)
            }
            if (meta == null) {
                JSONObject().put("name", name).put("libraryName", library)
                    .let { File(target, META).writeText(it.toString()) }
            }
            read(target) ?: error(context.getString(R.string.driver_error_unreadable))
        } finally {
            staging.deleteRecursively()
        }
    }

    private fun read(dir: File): GpuDriver? {
        val meta = File(dir, META).takeIf { it.isFile }?.let { runCatching { JSONObject(it.readText()) }.getOrNull() }
            ?: return null
        val library = meta.optString("libraryName").takeIf { it.isNotBlank() && File(dir, it).isFile } ?: return null
        return GpuDriver(
            id = dir.name,
            name = meta.optString("name", library),
            version = meta.optString("driverVersion"),
            vendor = meta.optString("vendor"),
            library = library,
            dir = dir,
        )
    }
}
