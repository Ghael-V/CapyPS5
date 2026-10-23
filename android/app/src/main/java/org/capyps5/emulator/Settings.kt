// SPDX-FileCopyrightText: Copyright 2026 CapyPS5 Project
// SPDX-License-Identifier: GPL-2.0-only

package org.capyps5.emulator

import android.content.Context
import android.os.Environment
import androidx.annotation.StringRes
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableFloatStateOf
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import java.io.File

enum class OverlayMode(@StringRes val labelRes: Int) {
    Off(R.string.overlay_off), Compact(R.string.overlay_compact), Full(R.string.overlay_full);

    fun label(context: Context) = context.getString(labelRes)
}

enum class TouchControlsMode(@StringRes val labelRes: Int) {
    Auto(R.string.mode_auto), Always(R.string.mode_always), Never(R.string.mode_never);

    fun label(context: Context) = context.getString(labelRes)
}

/** Face buttons of a physical pad. Nintendo-labelled pads (AYN Odin) report A/B and X/Y by label. */
enum class ButtonLayout(@StringRes val labelRes: Int) {
    Standard(R.string.layout_standard), Nintendo(R.string.layout_nintendo);

    fun label(context: Context) = context.getString(labelRes)
}

/**
 * Emulator core option, passed to the core as its debug.capy.* property before a game starts.
 * Each has a global value and an optional per-game override.
 */
enum class CoreOption(
    @StringRes val titleRes: Int,
    @StringRes val descriptionRes: Int,
    val property: String,
    val choices: List<Pair<Long, Int>>,
    val default: Long,
) {
    ResolutionScale(
        R.string.option_resolution,
        R.string.option_resolution_desc,
        "debug.capy.res_scale",
        listOf(
            1000L to R.string.resolution_native,
            750L to R.string.resolution_75,
            500L to R.string.resolution_50,
            250L to R.string.resolution_25,
        ),
        1000L,
    ),
    AsyncShaders(
        R.string.option_async_shaders,
        R.string.option_async_shaders_desc,
        "debug.capy.async_pipelines",
        listOf(1L to R.string.enabled, 0L to R.string.disabled),
        1L,
    ),
    ;

    fun title(context: Context) = context.getString(titleRes)

    fun description(context: Context) = context.getString(descriptionRes)

    fun label(context: Context, value: Long) =
        choices.firstOrNull { it.first == value }?.second?.let(context::getString) ?: value.toString()
}

/** User preferences backed by SharedPreferences and exposed as Compose state. */
class Settings(context: Context) {
    private val prefs = context.getSharedPreferences("capyps5", Context.MODE_PRIVATE)

    var overlayMode by mutableStateOf(
        runCatching { OverlayMode.valueOf(prefs.getString("overlay_mode", null)!!) }.getOrDefault(OverlayMode.Compact)
    )
        private set
    var touchControls by mutableStateOf(
        runCatching { TouchControlsMode.valueOf(prefs.getString("touch_controls", null)!!) }
            .getOrDefault(TouchControlsMode.Auto)
    )
        private set
    var controlsOpacity by mutableFloatStateOf(prefs.getFloat("controls_opacity", 0.6f))
        private set
    /** On-screen touchpad: Auto shows it with the touch controls, Always also with a physical pad. */
    var touchpadOverlay by mutableStateOf(
        runCatching { TouchControlsMode.valueOf(prefs.getString("touchpad_overlay", null)!!) }
            .getOrDefault(TouchControlsMode.Auto)
    )
        private set

    fun updateTouchpadOverlay(value: TouchControlsMode) {
        touchpadOverlay = value
        prefs.edit().putString("touchpad_overlay", value.name).apply()
    }
    var buttonLayout by mutableStateOf(
        runCatching { ButtonLayout.valueOf(prefs.getString("button_layout", null)!!) }
            .getOrDefault(ButtonLayout.Standard)
    )
        private set

    fun updateButtonLayout(value: ButtonLayout) {
        buttonLayout = value
        prefs.edit().putString("button_layout", value.name).apply()
    }
    /** Folders scanned for games (each game a subfolder with eboot.bin); the first one also holds logs. */
    var libraryPaths by mutableStateOf(
        prefs.getString("library_paths", null)?.split('\n')?.filter { it.isNotBlank() }?.takeIf { it.isNotEmpty() }
            ?: listOf(
                prefs.getString("library_path", null)
                    ?: File(Environment.getExternalStorageDirectory(), "CapyPS5").absolutePath
            )
    )
        private set

    /** Main library folder (first in the list). */
    val libraryPath: String get() = libraryPaths.first()

    fun addLibraryPath(value: String) {
        if (value in libraryPaths) return
        storeLibraryPaths(libraryPaths + value)
    }

    fun removeLibraryPath(value: String) {
        // At least one folder stays: the launcher needs somewhere to look and to write logs.
        if (libraryPaths.size <= 1) return
        storeLibraryPaths(libraryPaths - value)
    }

    private fun storeLibraryPaths(values: List<String>) {
        libraryPaths = values
        prefs.edit().putString("library_paths", values.joinToString("\n")).apply()
    }

    // Bumped on every core option change so composables reading the prefs recompose.
    var coreOptionsVersion by mutableIntStateOf(0)
        private set

    /** Global value of a core option. */
    fun coreOption(option: CoreOption): Long = prefs.getLong("opt_${option.name}", option.default)

    fun updateCoreOption(option: CoreOption, value: Long) {
        prefs.edit().putLong("opt_${option.name}", value).apply()
        coreOptionsVersion++
    }

    /** Per-game override, or null when the game follows the global value. */
    fun gameOption(titleId: String, option: CoreOption): Long? {
        val key = "game_${titleId}_${option.name}"
        return if (prefs.contains(key)) prefs.getLong(key, option.default) else null
    }

    fun updateGameOption(titleId: String, option: CoreOption, value: Long?) {
        val key = "game_${titleId}_${option.name}"
        prefs.edit().apply { if (value == null) remove(key) else putLong(key, value) }.apply()
        coreOptionsVersion++
    }

    /** Global GPU driver id ("" = system driver). */
    fun gpuDriver(): String = prefs.getString("gpu_driver", "").orEmpty()

    fun updateGpuDriver(id: String) {
        prefs.edit().putString("gpu_driver", id).apply()
        coreOptionsVersion++
    }

    /** Per-game GPU driver: null follows the global one, "" forces the system driver. */
    fun gameGpuDriver(titleId: String): String? = prefs.getString("game_${titleId}_gpu_driver", null)

    fun updateGameGpuDriver(titleId: String, id: String?) {
        val key = "game_${titleId}_gpu_driver"
        prefs.edit().apply { if (id == null) remove(key) else putString(key, id) }.apply()
        coreOptionsVersion++
    }

    fun effectiveGpuDriver(titleId: String): String = gameGpuDriver(titleId) ?: gpuDriver()

    /**
     * Resolution scale for Unreal Engine games without their own setting: they render near 4K and
     * are bound by pixel work on mobile GPUs, so a lower default keeps them playable.
     */
    fun ueResolution(): Long = prefs.getLong("ue_resolution", 250L)

    fun updateUeResolution(value: Long) {
        prefs.edit().putLong("ue_resolution", value).apply()
        coreOptionsVersion++
    }

    /** Value a game runs with: its own setting, then the Unreal default, then the global one. */
    fun effectiveOption(titleId: String, option: CoreOption, unreal: Boolean = false): Long =
        gameOption(titleId, option)
            ?: if (unreal && option == CoreOption.ResolutionScale) ueResolution() else coreOption(option)

    /** Whether the launcher looks for a newer release on start. */
    var updateChecks by mutableStateOf(prefs.getBoolean("update_checks", true))
        private set

    fun updateUpdateChecks(value: Boolean) {
        updateChecks = value
        prefs.edit().putBoolean("update_checks", value).apply()
    }

    /** Release the user chose to skip; no notice until a newer one appears. */
    var skippedRelease: String
        get() = prefs.getString("skipped_release", "").orEmpty()
        set(value) = prefs.edit().putString("skipped_release", value).apply()

    /** Version whose "what's new" card was last shown; empty before the first one. */
    var lastSeenVersion: String
        get() = prefs.getString("last_seen_version", "").orEmpty()
        set(value) = prefs.edit().putString("last_seen_version", value).apply()

    /** Counts launcher starts and returns the new count (the support card shows every 10th). */
    fun countLaunch(): Int {
        val count = prefs.getInt("launch_count", 0) + 1
        prefs.edit().putInt("launch_count", count).apply()
        return count
    }

    fun updateOverlayMode(value: OverlayMode) {
        overlayMode = value
        prefs.edit().putString("overlay_mode", value.name).apply()
    }

    fun updateTouchControls(value: TouchControlsMode) {
        touchControls = value
        prefs.edit().putString("touch_controls", value.name).apply()
    }

    fun updateControlsOpacity(value: Float) {
        controlsOpacity = value
        prefs.edit().putFloat("controls_opacity", value).apply()
    }
}
