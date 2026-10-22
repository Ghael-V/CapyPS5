// SPDX-FileCopyrightText: Copyright 2026 CapyPS5 Project
// SPDX-License-Identifier: GPL-2.0-only

package org.capyps5.emulator

import android.content.Context
import android.content.Intent
import android.net.Uri
import android.os.Build
import android.util.Log
import androidx.core.content.FileProvider
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import org.json.JSONArray
import java.io.File
import java.net.HttpURLConnection
import java.net.URL

/** A published release with an APK newer than the installed version. */
data class Release(val tag: String, val name: String, val notes: String, val apkUrl: String, val apkSize: Long)

/**
 * Checks the project's releases for a newer APK, downloads it and hands it to the system installer.
 * The project's own Forgejo is asked first and the GitHub mirror when it does not answer.
 */
object Updater {
    private const val TAG = "CapyPS5-Updater"
    private val SOURCES = listOf(
        "https://git.capyps5.org/api/v1/repos/CapyPS5/CapyPS5/releases?limit=20",
        "https://api.github.com/repos/Ghael-V/CapyPS5/releases?per_page=20",
    )

    /** What the launcher and the pause menu show, from the APK's own version name. */
    fun versionText(context: Context): String = "CapyPS5 v${installedVersion(context)} Alpha (ARM64 Android)"

    fun installedVersion(context: Context): String =
        runCatching { context.packageManager.getPackageInfo(context.packageName, 0).versionName }.getOrNull() ?: "0"

    /** Newest release whose version is above [current], or null (also when no source answers). */
    suspend fun checkForUpdate(current: String): Release? = withContext(Dispatchers.IO) {
        for (source in SOURCES) {
            val releases = runCatching { fetchReleases(source) }
                .onFailure { Log.w(TAG, "release check failed at $source: $it") }
                .getOrNull() ?: continue
            return@withContext releases
                .filter { compareVersions(it.tag, current) > 0 }
                .maxWithOrNull { a, b -> compareVersions(a.tag, b.tag) }
        }
        null
    }

    private fun fetchReleases(url: String): List<Release> {
        val connection = (URL(url).openConnection() as HttpURLConnection).apply {
            connectTimeout = 8000
            readTimeout = 8000
            setRequestProperty("Accept", "application/json")
            setRequestProperty("User-Agent", "CapyPS5-Updater")
        }
        try {
            check(connection.responseCode == HttpURLConnection.HTTP_OK) { "HTTP ${connection.responseCode}" }
            val array = JSONArray(connection.inputStream.bufferedReader().use { it.readText() })
            return (0 until array.length()).mapNotNull { i ->
                val release = array.getJSONObject(i)
                if (release.optBoolean("draft")) return@mapNotNull null
                val assets = release.optJSONArray("assets") ?: return@mapNotNull null
                val apks = (0 until assets.length()).map { assets.getJSONObject(it) }
                    .filter { it.optString("name").endsWith(".apk", ignoreCase = true) }
                // A release may also ship a debug build; users get the release one.
                val apk = apks.firstOrNull { !it.optString("name").contains("debug", ignoreCase = true) }
                    ?: apks.firstOrNull() ?: return@mapNotNull null
                val tag = release.optString("tag_name").ifEmpty { return@mapNotNull null }
                Release(
                    tag = tag,
                    name = release.optString("name").ifEmpty { tag },
                    notes = release.optString("body").trim(),
                    apkUrl = apk.optString("browser_download_url"),
                    apkSize = apk.optLong("size"),
                )
            }
        } finally {
            connection.disconnect()
        }
    }

    /**
     * Compares "v1.2.3"-style versions by their numeric parts; a leading "v" and any suffix such as
     * "-alpha" are ignored.
     */
    fun compareVersions(a: String, b: String): Int {
        fun parts(version: String) = version.trim().removePrefix("v").removePrefix("V")
            .substringBefore('-').substringBefore('+').split('.')
            .map { part -> part.takeWhile(Char::isDigit).toIntOrNull() ?: 0 }
        val left = parts(a)
        val right = parts(b)
        for (i in 0 until maxOf(left.size, right.size)) {
            val diff = left.getOrElse(i) { 0 } - right.getOrElse(i) { 0 }
            if (diff != 0) return diff
        }
        return 0
    }

    private fun apkFile(context: Context, release: Release): File =
        File(File(context.cacheDir, "updates").apply { mkdirs() }, "CapyPS5-${release.tag}.apk")

    /** Downloads the release's APK, reporting progress from 0 to 1; returns the file or null. */
    suspend fun download(context: Context, release: Release, onProgress: (Float) -> Unit): File? =
        withContext(Dispatchers.IO) {
            val target = apkFile(context, release)
            // Earlier downloads of other versions are no longer needed.
            target.parentFile?.listFiles()?.filter { it != target }?.forEach { it.delete() }
            runCatching {
                val connection = (URL(release.apkUrl).openConnection() as HttpURLConnection).apply {
                    connectTimeout = 10000
                    readTimeout = 30000
                    setRequestProperty("User-Agent", "CapyPS5-Updater")
                }
                try {
                    check(connection.responseCode == HttpURLConnection.HTTP_OK) { "HTTP ${connection.responseCode}" }
                    val total = connection.contentLengthLong.takeIf { it > 0 } ?: release.apkSize
                    val partial = File(target.path + ".part")
                    connection.inputStream.use { input ->
                        partial.outputStream().use { output ->
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
                    check(total <= 0 || partial.length() == total) { "incomplete download" }
                    check(partial.renameTo(target)) { "cannot move the download" }
                    target
                } finally {
                    connection.disconnect()
                }
            }.onFailure { Log.w(TAG, "download failed: $it") }.getOrNull()
        }

    /** False when Android still has to let this app install packages (see [requestInstallPermission]). */
    fun canInstall(context: Context): Boolean =
        Build.VERSION.SDK_INT < Build.VERSION_CODES.O || context.packageManager.canRequestPackageInstalls()

    fun requestInstallPermission(context: Context) {
        runCatching {
            context.startActivity(
                Intent(android.provider.Settings.ACTION_MANAGE_UNKNOWN_APP_SOURCES)
                    .setData(Uri.parse("package:${context.packageName}"))
                    .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
            )
        }
    }

    /** Opens the system installer on [apk]; the user confirms there. */
    fun install(context: Context, apk: File): Boolean = runCatching {
        val uri = FileProvider.getUriForFile(context, "${context.packageName}.updates", apk)
        context.startActivity(
            Intent(Intent.ACTION_VIEW)
                .setDataAndType(uri, "application/vnd.android.package-archive")
                .addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION or Intent.FLAG_ACTIVITY_NEW_TASK)
        )
    }.onFailure { Log.w(TAG, "install failed: $it") }.isSuccess
}
