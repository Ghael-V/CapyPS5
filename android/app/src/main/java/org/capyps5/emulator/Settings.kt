package org.capyps5.emulator

import android.content.Context
import android.os.Environment
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableFloatStateOf
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import java.io.File

enum class OverlayMode(val label: String) { Off("Desactivado"), Compact("Compacto"), Full("Completo") }

enum class TouchControlsMode(val label: String) { Auto("Automático"), Always("Siempre"), Never("Nunca") }

/** Face buttons of a physical pad. Nintendo-labelled pads (AYN Odin) report A/B and X/Y by label. */
enum class ButtonLayout(val label: String) { Standard("Estándar (Xbox)"), Nintendo("Nintendo (PS5)") }

/**
 * Emulator core option, passed to the core as its debug.capy.* property before a game starts.
 * Each has a global value and an optional per-game override.
 */
enum class CoreOption(
    val title: String,
    val description: String,
    val property: String,
    val choices: List<Pair<Long, String>>,
    val default: Long,
) {
    ResolutionScale(
        "Resolución interna",
        "Por eje: un juego 4K al 50 % se dibuja a 1080p (un cuarto del trabajo). " +
            "Menos resolución alivia la GPU; algunos juegos no la admiten",
        "debug.capy.res_scale",
        listOf(
            1000L to "Nativa",
            750L to "75 % (4K → 1620p)",
            500L to "50 % (4K → 1080p)",
            250L to "25 % (4K → 540p)",
        ),
        1000L,
    ),
    AsyncShaders(
        "Compilar shaders en segundo plano",
        "Evita tirones; un objeto puede tardar un instante en aparecer",
        "debug.capy.async_pipelines",
        listOf(1L to "Activado", 0L to "Desactivado"),
        1L,
    ),
    ;

    fun label(value: Long) = choices.firstOrNull { it.first == value }?.second ?: value.toString()
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

    /** Value a game runs with. */
    fun effectiveOption(titleId: String, option: CoreOption): Long = gameOption(titleId, option) ?: coreOption(option)

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
