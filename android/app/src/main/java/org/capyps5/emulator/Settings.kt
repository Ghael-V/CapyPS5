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
    /** Folder scanned for games (each game a subfolder with eboot.bin). */
    var libraryPath by mutableStateOf(
        prefs.getString("library_path", null)
            ?: File(Environment.getExternalStorageDirectory(), "CapyPS5").absolutePath
    )
        private set

    fun updateLibraryPath(value: String) {
        libraryPath = value
        prefs.edit().putString("library_path", value).apply()
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
