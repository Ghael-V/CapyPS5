// SPDX-FileCopyrightText: Copyright 2026 CapyPS5 Project
// SPDX-License-Identifier: GPL-2.0-only

package org.capyps5.emulator

import android.content.Context
import android.os.Build
import android.util.Log
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import org.json.JSONArray
import java.io.File
import java.net.HttpURLConnection
import java.net.URL
import java.time.Instant

/**
 * A GitHub repository that publishes Vulkan driver packages in the AdrenoTools layout.
 * [useTag] shows the release tag instead of its title (repos whose titles are all alike);
 * [assetSuffix] keeps only the driver packages of repos that also publish other files.
 */
data class DriverSource(
    val name: String,
    val repo: String,
    val mali: Boolean = false,
    val useTag: Boolean = false,
    val assetSuffix: String = ".zip",
)

/** One downloadable driver package. */
data class DriverPackage(
    val source: DriverSource,
    val title: String,
    val assetName: String,
    val url: String,
    val size: Long,
    val published: Instant?,
    val prerelease: Boolean,
)

/** Driver sources (the same community builds other Android emulators offer) and their releases. */
object DriverCatalog {
    private const val TAG = "CapyPS5-Drivers"

    private val SOURCES = listOf(
        DriverSource("Lemon-Ade Turnip", "Ghael-V/Turnip-Lemon-Ade"),
        DriverSource("Mr. Purple Turnip", "MrPurple666/purple-turnip"),
        DriverSource("GameHub Adreno 8xx", "crueter/GameHub-8Elite-Drivers"),
        DriverSource("KIMCHI Turnip", "K11MCH1/AdrenoToolsDrivers", useTag = true),
        DriverSource("Weab-Chan Freedreno", "Weab-chan/freedreno_turnip-CI"),
        DriverSource("Whitebelyash Turnip", "whitebelyash/freedreno_turnip-CI"),
        DriverSource("StevenMXZ Turnip", "StevenMXZ/Adreno-Tools-Drivers"),
        DriverSource("Balemuni Apex Turnip", "Balemuni/Balemunis-Aurora"),
        DriverSource("PanVK G615 (Mali, Android 15+)", "zenithblue-oss/panvk-kbase-android", mali = true,
            assetSuffix = ".adpkg.zip"),
        DriverSource("PanVK G720 (Mali, Android 15+)", "wonderkast02/panvk-g720-kbase-csf", mali = true),
        DriverSource("PanVK G52 (Mali, Android 9+)", "LukeValen/panvk-mali-g52", mali = true),
    )

    /** Snapdragon devices get the Turnip builds (Adreno), everything else the PanVK ones (Mali). */
    fun sources(): List<DriverSource> {
        val qualcomm = listOf(Build.SOC_MANUFACTURER, Build.HARDWARE, Build.BOARD)
            .any { it.contains("qcom", true) || it.contains("qti", true) || it.contains("qualcomm", true) }
        return SOURCES.filter { it.mali != qualcomm }
    }

    /** Newest packages of [source] first; empty when GitHub does not answer. */
    suspend fun packages(source: DriverSource): Result<List<DriverPackage>> = withContext(Dispatchers.IO) {
        runCatching {
            val connection = (URL("https://api.github.com/repos/${source.repo}/releases?per_page=20")
                .openConnection() as HttpURLConnection).apply {
                connectTimeout = 8000
                readTimeout = 8000
                setRequestProperty("Accept", "application/vnd.github+json")
                setRequestProperty("User-Agent", "CapyPS5")
            }
            try {
                check(connection.responseCode == HttpURLConnection.HTTP_OK) { "HTTP ${connection.responseCode}" }
                val releases = JSONArray(connection.inputStream.bufferedReader().use { it.readText() })
                (0 until releases.length()).flatMap { i ->
                    val release = releases.getJSONObject(i)
                    if (release.optBoolean("draft")) return@flatMap emptyList()
                    val tag = release.optString("tag_name")
                    val title = if (source.useTag) tag else release.optString("name").ifBlank { tag }
                    val published = runCatching { Instant.parse(release.optString("published_at")) }.getOrNull()
                    val assets = release.optJSONArray("assets") ?: JSONArray()
                    (0 until assets.length()).map { assets.getJSONObject(it) }
                        .filter { it.optString("name").endsWith(source.assetSuffix, ignoreCase = true) }
                        .map { asset ->
                            DriverPackage(
                                source = source,
                                title = title,
                                assetName = asset.optString("name"),
                                url = asset.optString("browser_download_url"),
                                size = asset.optLong("size"),
                                published = published,
                                prerelease = release.optBoolean("prerelease"),
                            )
                        }
                }.sortedByDescending { it.published }
            } finally {
                connection.disconnect()
            }
        }.onFailure { Log.w(TAG, "${source.repo}: $it") }
    }

    /** Downloads [pkg] and installs it as a driver; progress goes from 0 to 1. */
    suspend fun install(context: Context, pkg: DriverPackage, onProgress: (Float) -> Unit): Result<GpuDriver> =
        withContext(Dispatchers.IO) {
            val file = File(File(context.cacheDir, "driver_download").apply { mkdirs() }, "package.zip")
            try {
                runCatching {
                    val connection = (URL(pkg.url).openConnection() as HttpURLConnection).apply {
                        connectTimeout = 10000
                        readTimeout = 30000
                        setRequestProperty("User-Agent", "CapyPS5")
                    }
                    try {
                        check(connection.responseCode == HttpURLConnection.HTTP_OK) { "HTTP ${connection.responseCode}" }
                        val total = connection.contentLengthLong.takeIf { it > 0 } ?: pkg.size
                        connection.inputStream.use { input ->
                            file.outputStream().use { output ->
                                val buffer = ByteArray(64 * 1024)
                                var done = 0L
                                while (true) {
                                    val read = input.read(buffer)
                                    if (read < 0) break
                                    output.write(buffer, 0, read)
                                    done += read
                                    if (total > 0) onProgress((done.toFloat() / total).coerceIn(0f, 1f))
                                }
                            }
                        }
                    } finally {
                        connection.disconnect()
                    }
                }.mapCatching { GpuDrivers.import(context, file).getOrThrow() }
            } finally {
                file.delete()
            }
        }
}
