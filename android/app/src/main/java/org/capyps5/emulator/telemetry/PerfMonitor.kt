// SPDX-FileCopyrightText: Copyright 2026 CapyPS5 Project
// SPDX-License-Identifier: GPL-2.0-only

package org.capyps5.emulator.telemetry

import android.app.ActivityManager
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.os.BatteryManager
import android.os.SystemClock
import org.capyps5.emulator.NativeBridge
import java.io.File

/** One overlay sample. Fields are null when the device does not expose that sensor. */
data class PerfSnapshot(
    val fps: Float = 0f,
    val hostFps: Float = 0f,
    val frameTimeMs: Float = 0f,
    val maxFrameTimeMs: Float = 0f,
    val cpuPercent: Float? = null,
    val cpuClusterMhz: List<Int> = emptyList(),
    val cpuTempC: Float? = null,
    val gpuPercent: Float? = null,
    val gpuMhz: Int? = null,
    val gpuTempC: Float? = null,
    val appRamMb: Int = 0,
    val deviceRamUsedMb: Int = 0,
    val deviceRamTotalMb: Int = 0,
    val batteryTempC: Float? = null,
    val powerWatts: Float? = null,
    val syncTimeoutsPerSec: Float = 0f,
    val unmappedWaitsPerSec: Float = 0f,
    val nullBuffersPerSec: Float = 0f,
)

/**
 * Samples emulator counters and device sensors. Not thread-safe: call [sample] from a single
 * background coroutine. Sensor paths are probed once; unreadable ones are dropped silently.
 */
class PerfMonitor(private val context: Context) {
    private val activityManager = context.getSystemService(ActivityManager::class.java)
    private val batteryManager = context.getSystemService(BatteryManager::class.java)
    private val pageSizeKb = android.system.Os.sysconf(android.system.OsConstants._SC_PAGESIZE) / 1024

    private val cpuPolicies: List<File> =
        File("/sys/devices/system/cpu/cpufreq").listFiles { f -> f.name.startsWith("policy") }
            ?.sortedBy { it.name.removePrefix("policy").toIntOrNull() ?: 0 }
            ?.map { File(it, "scaling_cur_freq") }
            ?.filter { it.canRead() }
            .orEmpty()
    private val gpuBusy = readable("/sys/class/kgsl/kgsl-3d0/gpu_busy_percentage")
    private val gpuClock = readable("/sys/class/kgsl/kgsl-3d0/gpuclk")
        ?: readable("/sys/class/kgsl/kgsl-3d0/devfreq/cur_freq")
    private val gpuTemp = readable("/sys/class/kgsl/kgsl-3d0/temp")
    private val cpuThermalZones: List<File> = (0 until 128).mapNotNull { i ->
        val zone = File("/sys/class/thermal/thermal_zone$i")
        val type = runCatching { File(zone, "type").readText().trim() }.getOrNull() ?: return@mapNotNull null
        if (type.startsWith("cpu") || type.startsWith("cpuss")) File(zone, "temp").takeIf { it.canRead() } else null
    }

    private var lastTime = 0L
    private var lastStats: LongArray? = null
    private var lastCpuTotal = 0L
    private var lastCpuIdle = 0L

    private fun readable(path: String) = File(path).takeIf { it.canRead() }

    private fun readLong(file: File?): Long? =
        file?.let { runCatching { it.readText().trim().split(' ', '\t').first().toLong() }.getOrNull() }

    fun sample(): PerfSnapshot {
        val now = SystemClock.elapsedRealtimeNanos()
        val stats = runCatching { NativeBridge.nativeGetStats() }.getOrNull()
        val prev = lastStats
        val dtSec = if (lastTime == 0L) 0f else (now - lastTime) / 1e9f
        lastTime = now
        lastStats = stats

        fun rate(index: Int): Float {
            if (stats == null || prev == null || dtSec <= 0f || index >= stats.size) return 0f
            return (stats[index] - prev[index]).coerceAtLeast(0) / dtSec
        }

        val fps = rate(NativeBridge.STAT_GAME_FRAMES)
        val memInfo = ActivityManager.MemoryInfo().also { activityManager.getMemoryInfo(it) }
        val batteryIntent = context.registerReceiver(null, IntentFilter(Intent.ACTION_BATTERY_CHANGED))
        val batteryTemp = batteryIntent?.getIntExtra(BatteryManager.EXTRA_TEMPERATURE, Int.MIN_VALUE)
            ?.takeIf { it != Int.MIN_VALUE }?.div(10f)
        val voltageMv = batteryIntent?.getIntExtra(BatteryManager.EXTRA_VOLTAGE, 0) ?: 0
        val currentUa = batteryManager.getLongProperty(BatteryManager.BATTERY_PROPERTY_CURRENT_NOW)
        val power = if (voltageMv > 0 && currentUa != Long.MIN_VALUE && currentUa != 0L) {
            // Sign convention differs between vendors; discharge power is what matters here.
            kotlin.math.abs(currentUa / 1e6f * voltageMv / 1e3f)
        } else null

        return PerfSnapshot(
            fps = fps,
            hostFps = rate(NativeBridge.STAT_HOST_PRESENTS),
            frameTimeMs = if (fps > 0f) 1000f / fps else 0f,
            maxFrameTimeMs = (stats?.getOrNull(NativeBridge.STAT_MAX_FRAME_NS) ?: 0L) / 1e6f,
            cpuPercent = sampleCpu(),
            cpuClusterMhz = cpuPolicies.mapNotNull { readLong(it)?.div(1000)?.toInt() },
            cpuTempC = cpuThermalZones.mapNotNull { readLong(it) }.maxOrNull()?.let { milliToC(it) },
            gpuPercent = readLong(gpuBusy)?.toFloat(),
            gpuMhz = readLong(gpuClock)?.let { (it / 1_000_000).toInt() },
            gpuTempC = readLong(gpuTemp)?.let { milliToC(it) },
            appRamMb = readAppRssMb(),
            deviceRamUsedMb = ((memInfo.totalMem - memInfo.availMem) / (1024 * 1024)).toInt(),
            deviceRamTotalMb = (memInfo.totalMem / (1024 * 1024)).toInt(),
            batteryTempC = batteryTemp,
            powerWatts = power,
            syncTimeoutsPerSec = rate(NativeBridge.STAT_WAIT_TIMEOUTS),
            unmappedWaitsPerSec = rate(NativeBridge.STAT_WAIT_UNMAPPED),
            nullBuffersPerSec = rate(NativeBridge.STAT_NULL_BUFFERS),
        )
    }

    /** Sensors report either milli-degrees or degrees depending on the driver. */
    private fun milliToC(value: Long): Float = if (value > 1000) value / 1000f else value.toFloat()

    private fun sampleCpu(): Float? {
        val line = runCatching { File("/proc/stat").bufferedReader().use { it.readLine() } }.getOrNull()
            ?: return null
        val fields = line.split(Regex("\\s+")).drop(1).mapNotNull { it.toLongOrNull() }
        if (fields.size < 5) return null
        val idle = fields[3] + fields[4]
        val total = fields.sum()
        val dTotal = total - lastCpuTotal
        val dIdle = idle - lastCpuIdle
        val first = lastCpuTotal == 0L
        lastCpuTotal = total
        lastCpuIdle = idle
        if (first || dTotal <= 0) return null
        return (100f * (dTotal - dIdle) / dTotal).coerceIn(0f, 100f)
    }

    private fun readAppRssMb(): Int {
        val statm = runCatching { File("/proc/self/statm").readText().split(' ') }.getOrNull() ?: return 0
        val residentPages = statm.getOrNull(1)?.toLongOrNull() ?: return 0
        return (residentPages * pageSizeKb / 1024).toInt()
    }
}
