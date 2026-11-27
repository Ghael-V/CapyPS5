// SPDX-FileCopyrightText: Copyright 2026 CapyPS5 Project
// SPDX-License-Identifier: GPL-2.0-only

package org.capyps5.emulator.aiconfig

import android.app.ActivityManager
import android.content.Context
import android.content.pm.PackageManager
import android.opengl.EGL14
import android.opengl.GLES20
import android.os.Build
import android.util.Log
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import org.capyps5.emulator.BuildConfig
import org.capyps5.emulator.GpuDrivers
import org.capyps5.emulator.Settings
import org.json.JSONObject
import java.io.ByteArrayOutputStream
import java.net.HttpURLConnection
import java.net.URL

/**
 * Asks the suggestions server (it holds the AI key and the cache) for a game's settings. The app
 * sends only the game and what the device is: no account and nothing personal.
 */
object AiConfigClient {
    private const val TAG = "CapyPS5-AiConfig"
    const val CLIENT = "capyps5"
    private const val MAX_BYTES = 256 * 1024

    class Device(
        val soc: String,
        val gpu: String,
        val gpuFamily: String,
        val driver: String,
        val vulkan: String,
        val ramGb: Int,
        val androidApi: Int,
    )

    class Request(
        val appVersion: String,
        val schemaVersion: Int,
        val titleId: String,
        val titleName: String,
        val gameVersion: String,
        val language: String,
        val device: Device,
        /** Effective values that differ from the schema default ("true"/"false" or the number). */
        val current: Map<String, String>,
    )

    /** One change as the server sent it; [value] is a JSON value (Boolean or Number). */
    class Change(val key: String, val value: Any?, val reason: String, val experimental: Boolean)

    class Suggestion(val answerId: String, val source: String, val changes: List<Change>, val notes: String)

    sealed interface Result {
        class Ok(val suggestion: Suggestion) : Result

        /** The server answered with an error ([code] from its body, e.g. rate_limited). */
        class ServerError(val code: String) : Result

        data object NetworkError : Result
    }

    enum class Outcome(val wire: String) {
        CRASHED("crashed"),
        NO_BOOT("no_boot"),
        REVERTED("reverted"),
    }

    /** GPU name and driver version as OpenGL ES reports them (no Vulkan in the launcher process). */
    @Volatile
    private var glInfo: Pair<String, String>? = null

    private fun glStrings(): Pair<String, String> = glInfo ?: runCatching {
        val display = EGL14.eglGetDisplay(EGL14.EGL_DEFAULT_DISPLAY)
        val version = IntArray(2)
        check(EGL14.eglInitialize(display, version, 0, version, 1))
        try {
            val configs = arrayOfNulls<android.opengl.EGLConfig>(1)
            val count = IntArray(1)
            val attributes = intArrayOf(
                EGL14.EGL_RENDERABLE_TYPE, EGL14.EGL_OPENGL_ES2_BIT,
                EGL14.EGL_SURFACE_TYPE, EGL14.EGL_PBUFFER_BIT, EGL14.EGL_NONE,
            )
            check(EGL14.eglChooseConfig(display, attributes, 0, configs, 0, 1, count, 0) && count[0] > 0)
            val context = EGL14.eglCreateContext(
                display, configs[0], EGL14.EGL_NO_CONTEXT,
                intArrayOf(EGL14.EGL_CONTEXT_CLIENT_VERSION, 2, EGL14.EGL_NONE), 0,
            )
            val surface = EGL14.eglCreatePbufferSurface(
                display, configs[0], intArrayOf(EGL14.EGL_WIDTH, 1, EGL14.EGL_HEIGHT, 1, EGL14.EGL_NONE), 0,
            )
            try {
                check(EGL14.eglMakeCurrent(display, surface, surface, context))
                (GLES20.glGetString(GLES20.GL_RENDERER).orEmpty() to GLES20.glGetString(GLES20.GL_VERSION).orEmpty())
            } finally {
                EGL14.eglMakeCurrent(display, EGL14.EGL_NO_SURFACE, EGL14.EGL_NO_SURFACE, EGL14.EGL_NO_CONTEXT)
                EGL14.eglDestroySurface(display, surface)
                EGL14.eglDestroyContext(display, context)
            }
        } finally {
            EGL14.eglTerminate(display)
        }
    }.getOrDefault("" to "").also { if (it.first.isNotEmpty()) glInfo = it }

    /** The Vulkan version the system driver supports (FEATURE_VULKAN_HARDWARE_VERSION), e.g. "1.3.0". */
    private fun vulkanVersion(context: Context): String {
        val feature = context.packageManager.systemAvailableFeatures
            .firstOrNull { it.name == PackageManager.FEATURE_VULKAN_HARDWARE_VERSION } ?: return ""
        val v = feature.version
        return "${v ushr 22}.${(v ushr 12) and 0x3ff}.${v and 0xfff}"
    }

    /** The device as the request describes it, with the GPU driver [titleId] runs with. Not on the main thread. */
    fun device(context: Context, settings: Settings, titleId: String): Device {
        val (gpu, glVersion) = glStrings()
        val custom = GpuDrivers.find(context, settings.effectiveGpuDriver(titleId))
        val driver = if (custom == null) {
            // Adreno: "OpenGL ES 3.2 V@0762.15 (GIT@...)": the V@ part is the driver build.
            "system " + (Regex("V@\\S+").find(glVersion)?.value ?: glVersion)
        } else {
            val text = "${custom.label} ${custom.vendor}".lowercase()
            val kind = if ("turnip" in text || "mesa" in text) "custom Turnip" else "custom"
            "$kind ${custom.label}"
        }
        val memory = ActivityManager.MemoryInfo()
        (context.getSystemService(Context.ACTIVITY_SERVICE) as ActivityManager).getMemoryInfo(memory)
        return Device(
            soc = Build.SOC_MODEL.take(64),
            gpu = gpu.take(96),
            gpuFamily = gpuFamily(gpu),
            driver = driver.trim().take(128),
            vulkan = vulkanVersion(context).take(64),
            ramGb = ((memory.totalMem + (1L shl 29)) shr 30).toInt().coerceIn(0, 128),
            androidApi = Build.VERSION.SDK_INT,
        )
    }

    /** "Adreno (TM) 830" -> "adreno8xx", "Mali-G925-Immortalis" -> "mali-g9xx", else the name. */
    fun gpuFamily(gpu: String): String {
        val lower = gpu.lowercase()
        Regex("adreno\\D*(\\d)\\d\\d").find(lower)?.let { return "adreno${it.groupValues[1]}xx" }
        Regex("mali-?([gt])(\\d)\\d+").find(lower)?.let { return "mali-${it.groupValues[1]}${it.groupValues[2]}xx" }
        if ("xclipse" in lower) return "xclipse"
        if ("powervr" in lower) return "powervr"
        return lower.filter { it.isLetterOrDigit() || it == '-' }.take(32)
    }

    private fun Request.toJson(): String = JSONObject()
        .put("client", CLIENT)
        .put("app_version", appVersion)
        .put("schema_version", schemaVersion)
        .put("title_id", titleId)
        .put("title_name", titleName)
        .put("game_version", gameVersion)
        .put("language", language)
        .put(
            "device",
            JSONObject()
                .put("soc", device.soc)
                .put("gpu", device.gpu)
                .put("gpu_family", device.gpuFamily)
                .put("driver", device.driver)
                .put("vulkan", device.vulkan)
                .put("ram_gb", device.ramGb)
                .put("android_api", device.androidApi),
        )
        .put("current", JSONObject().apply { current.forEach { (key, value) -> put(key, value) } })
        .toString()

    private class Response(val code: Int, val body: String?)

    private fun post(path: String, body: String, readTimeoutMs: Int): Response {
        val connection = (URL("${BuildConfig.AI_CONFIG_URL}$path").openConnection() as HttpURLConnection).apply {
            requestMethod = "POST"
            connectTimeout = 15_000
            // The model can take a while when nothing is cached.
            readTimeout = readTimeoutMs
            doOutput = true
            setRequestProperty("Content-Type", "application/json; charset=utf-8")
            setRequestProperty("Accept", "application/json")
        }
        try {
            connection.outputStream.use { it.write(body.toByteArray(Charsets.UTF_8)) }
            val code = connection.responseCode
            val stream = if (code in 200..299) connection.inputStream else connection.errorStream
            val text = stream?.use { input ->
                val out = ByteArrayOutputStream()
                val buffer = ByteArray(8192)
                while (true) {
                    val n = input.read(buffer)
                    if (n < 0) break
                    out.write(buffer, 0, n)
                    if (out.size() > MAX_BYTES) return@use null
                }
                out.toString(Charsets.UTF_8.name())
            }
            return Response(code, text)
        } finally {
            connection.disconnect()
        }
    }

    suspend fun suggest(request: Request): Result = withContext(Dispatchers.IO) {
        try {
            val response = post("/ai/game-config", request.toJson(), 90_000)
            if (response.code !in 200..299) {
                val code = response.body?.let {
                    runCatching { JSONObject(it).getJSONObject("error").getString("code") }.getOrNull()
                } ?: "http_${response.code}"
                Log.w(TAG, "Server answered ${response.code} ($code)")
                return@withContext Result.ServerError(code)
            }
            val suggestion = response.body?.let { runCatching { parse(JSONObject(it)) }.getOrNull() }
                ?: return@withContext Result.ServerError("bad_answer")
            Result.Ok(suggestion)
        } catch (e: Exception) {
            Log.w(TAG, "Could not reach the server: ${e.javaClass.simpleName}")
            Result.NetworkError
        }
    }

    private fun parse(o: JSONObject): Suggestion {
        // A null list or text is the same as an empty one.
        val list = o.optJSONArray("changes")
        val changes = if (list == null) emptyList() else (0 until list.length()).mapNotNull { i ->
            val c = list.optJSONObject(i) ?: return@mapNotNull null
            Change(
                key = c.optString("key"),
                value = if (c.isNull("value")) null else c.get("value"),
                reason = if (c.isNull("reason")) "" else c.optString("reason"),
                experimental = c.optBoolean("experimental", false),
            )
        }
        return Suggestion(
            answerId = o.getString("answer_id"),
            source = o.optString("source"),
            changes = changes,
            notes = if (o.isNull("notes")) "" else o.optString("notes"),
        )
    }

    /** Best effort, not on the main thread: a lost report only means the server learns a little less. */
    fun feedback(answerId: String, outcome: Outcome) {
        if (answerId.isEmpty()) return
        try {
            post(
                "/ai/game-config/feedback",
                JSONObject().put("answer_id", answerId).put("outcome", outcome.wire).toString(),
                15_000,
            )
        } catch (e: Exception) {
            Log.w(TAG, "Could not send feedback: ${e.javaClass.simpleName}")
        }
    }
}
