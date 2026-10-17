package org.capyps5.emulator

import android.content.Context
import android.os.Environment
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableFloatStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import java.io.File

enum class OverlayMode(val label: String) { Off("Desactivado"), Compact("Compacto"), Full("Completo") }

enum class TouchControlsMode(val label: String) { Auto("Automático"), Always("Siempre"), Never("Nunca") }

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
