package org.capyps5.emulator.ui.screens

import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxHeight
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.outlined.Folder
import androidx.compose.material.icons.outlined.Info
import androidx.compose.material.icons.outlined.Image
import androidx.compose.material.icons.outlined.Settings
import androidx.compose.material.icons.outlined.Speed
import androidx.compose.material.icons.outlined.SportsEsports
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.unit.dp
import org.capyps5.emulator.CoreOption
import org.capyps5.emulator.OverlayMode
import org.capyps5.emulator.Settings
import org.capyps5.emulator.TouchControlsMode
import org.capyps5.emulator.ui.NavKey
import org.capyps5.emulator.ui.OnNavKey
import org.capyps5.emulator.ui.theme.Capy

/** One setting: a value cycled with left/right or confirm, or an action run on confirm. */
class SettingItem(
    val title: String,
    val subtitle: String? = null,
    val value: String? = null,
    val danger: Boolean = false,
    val onStep: ((Int) -> Unit)? = null,
    val onConfirm: (() -> Unit)? = null,
)

private class SettingsCategory(val title: String, val icon: ImageVector, val items: () -> List<SettingItem>)

fun <T> cycle(values: List<T>, current: T, delta: Int): T =
    values[(values.indexOf(current).coerceAtLeast(0) + delta).mod(values.size)]

/** Full-screen settings with categories on the left, usable with touch or a controller. */
@Composable
fun SettingsScreen(
    settings: Settings,
    versionText: String,
    onAddFolder: () -> Unit,
    onClearShaderCache: () -> String,
    onSaveLog: () -> String?,
    onClose: () -> Unit,
) {
    var message by remember { mutableStateOf<String?>(null) }
    // Read so the option rows recompose when a value changes.
    val optionsVersion = settings.coreOptionsVersion

    fun coreItem(option: CoreOption): SettingItem {
        val values = option.choices.map { it.first }
        val current = settings.coreOption(option)
        return SettingItem(
            option.title, option.description, option.label(current),
            onStep = { settings.updateCoreOption(option, cycle(values, current, it)) },
        )
    }

    val categories = remember(optionsVersion, settings.libraryPaths, message) {
        listOf(
            SettingsCategory("Gráficos", Icons.Outlined.Image) {
                listOf(coreItem(CoreOption.ResolutionScale))
            },
            SettingsCategory("Rendimiento", Icons.Outlined.Speed) {
                listOf(
                    coreItem(CoreOption.AsyncShaders),
                    SettingItem(
                        "Monitor de rendimiento", "FPS, CPU, GPU, RAM y temperaturas en pantalla",
                        settings.overlayMode.label,
                        onStep = { settings.updateOverlayMode(cycle(OverlayMode.entries, settings.overlayMode, it)) },
                    ),
                )
            },
            SettingsCategory("Controles", Icons.Outlined.SportsEsports) {
                listOf(
                    SettingItem(
                        "Controles táctiles", "En automático se ocultan si hay un mando físico",
                        settings.touchControls.label,
                        onStep = {
                            settings.updateTouchControls(cycle(TouchControlsMode.entries, settings.touchControls, it))
                        },
                    ),
                    SettingItem(
                        "Touchpad en pantalla",
                        "Automático: con los controles táctiles · Siempre: también con un mando físico",
                        settings.touchpadOverlay.label,
                        onStep = {
                            settings.updateTouchpadOverlay(cycle(TouchControlsMode.entries, settings.touchpadOverlay, it))
                        },
                    ),
                    SettingItem(
                        "Opacidad de los controles", null, "%.0f %%".format(settings.controlsOpacity * 100),
                        onStep = { settings.updateControlsOpacity((settings.controlsOpacity + it * 0.1f).coerceIn(0.1f, 1f)) },
                    ),
                )
            },
            SettingsCategory("Carpetas de juegos", Icons.Outlined.Folder) {
                settings.libraryPaths.map { path ->
                    SettingItem(
                        path,
                        if (settings.libraryPaths.size > 1) "Ⓐ quitar esta carpeta" else "Carpeta principal",
                        onConfirm = if (settings.libraryPaths.size > 1) ({ settings.removeLibraryPath(path) }) else null,
                    )
                } + SettingItem("Añadir carpeta", "Se buscan juegos en todas las carpetas", onConfirm = onAddFolder)
            },
            SettingsCategory("Sistema", Icons.Outlined.Settings) {
                listOf(
                    SettingItem(
                        "Borrar caché de shaders", "Se vuelven a compilar al jugar", danger = true,
                        onConfirm = { message = onClearShaderCache() },
                    ),
                    SettingItem(
                        "Guardar log", "En la carpeta logs de tu carpeta principal",
                        onConfirm = { message = onSaveLog()?.let { "Log guardado en $it" } ?: "No se pudo guardar el log" },
                    ),
                ) + listOfNotNull(message?.let { SettingItem(it) })
            },
            SettingsCategory("Acerca de", Icons.Outlined.Info) {
                listOf(
                    SettingItem("CapyPS5", versionText),
                    SettingItem(
                        "Licencia",
                        "Software libre bajo GPL v2. Incluye Kyty (GPL-2.0), FEX (MIT), SDL (zlib), FFmpeg " +
                            "(LGPL-2.1) y otros componentes de código abierto. No incluye software ni claves de Sony.",
                    ),
                    SettingItem(
                        "Dispositivo",
                        "${android.os.Build.MANUFACTURER} ${android.os.Build.MODEL} · ${android.os.Build.SOC_MODEL} · " +
                            "Android ${android.os.Build.VERSION.RELEASE}",
                    ),
                )
            },
        )
    }

    var category by remember { mutableIntStateOf(0) }
    var inContent by remember { mutableStateOf(false) }
    var row by remember { mutableIntStateOf(0) }
    val items = categories[category].items()
    if (row >= items.size) row = (items.size - 1).coerceAtLeast(0)

    OnNavKey { key ->
        if (!inContent) {
            when (key) {
                NavKey.Up -> { category = (category - 1).mod(categories.size); row = 0 }
                NavKey.Down -> { category = (category + 1).mod(categories.size); row = 0 }
                NavKey.Right, NavKey.Confirm -> inContent = true
                NavKey.Back, NavKey.Menu -> onClose()
                else -> Unit
            }
        } else {
            val item = items.getOrNull(row)
            when (key) {
                NavKey.Up -> row = (row - 1).mod(items.size.coerceAtLeast(1))
                NavKey.Down -> row = (row + 1).mod(items.size.coerceAtLeast(1))
                NavKey.Left -> if (item?.onStep != null) item.onStep.invoke(-1) else inContent = false
                NavKey.Right -> item?.onStep?.invoke(1)
                NavKey.Confirm -> item?.onConfirm?.invoke() ?: item?.onStep?.invoke(1)
                NavKey.Back -> inContent = false
                NavKey.Menu -> onClose()
                else -> Unit
            }
        }
    }

    Row(Modifier.fillMaxSize().background(Capy.Background)) {
        Column(
            Modifier.width(260.dp).fillMaxHeight().background(Capy.Surface).padding(vertical = 22.dp),
            verticalArrangement = Arrangement.spacedBy(2.dp),
        ) {
            Text("Ajustes", style = MaterialTheme.typography.headlineSmall, modifier = Modifier.padding(start = 24.dp, bottom = 14.dp))
            categories.forEachIndexed { i, entry ->
                val selected = i == category
                Row(
                    modifier = Modifier
                        .fillMaxWidth()
                        .background(if (selected) Capy.SurfaceHigh else Color.Transparent)
                        .clickable { category = i; row = 0; inContent = false }
                        .padding(horizontal = 24.dp, vertical = 12.dp),
                    verticalAlignment = Alignment.CenterVertically,
                ) {
                    Icon(entry.icon, null, Modifier.size(20.dp), tint = if (selected) Capy.TextPrimary else Capy.TextSecondary)
                    Spacer(Modifier.width(14.dp))
                    Text(
                        entry.title,
                        style = MaterialTheme.typography.titleMedium,
                        color = if (selected) Capy.TextPrimary else Capy.TextSecondary,
                    )
                }
            }
            Spacer(Modifier.weight(1f))
            Text(
                "Ⓑ Volver",
                style = MaterialTheme.typography.labelLarge,
                color = Capy.TextSecondary,
                modifier = Modifier.clickable(onClick = onClose).padding(horizontal = 24.dp, vertical = 8.dp),
            )
        }
        Column(
            Modifier.weight(1f).fillMaxHeight().verticalScroll(rememberScrollState()).padding(28.dp),
            verticalArrangement = Arrangement.spacedBy(8.dp),
        ) {
            Text(categories[category].title, style = MaterialTheme.typography.titleLarge)
            Spacer(Modifier.height(6.dp))
            items.forEachIndexed { i, item ->
                SettingRowView(item, focused = inContent && i == row) { row = i; inContent = true }
            }
        }
    }
}

@Composable
fun SettingRowView(item: SettingItem, focused: Boolean, onFocus: () -> Unit) {
    val shape = RoundedCornerShape(12.dp)
    Row(
        modifier = Modifier
            .fillMaxWidth()
            .background(if (focused) Capy.SurfaceHigh else Capy.Surface, shape)
            .border(1.dp, if (focused) Capy.Accent else Color.Transparent, shape)
            .clickable {
                onFocus()
                item.onConfirm?.invoke() ?: item.onStep?.invoke(1)
            }
            .padding(horizontal = 18.dp, vertical = 14.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Column(Modifier.weight(1f)) {
            Text(item.title, style = MaterialTheme.typography.titleMedium, color = if (item.danger) Capy.Bad else Capy.TextPrimary)
            item.subtitle?.let {
                Text(it, style = MaterialTheme.typography.bodySmall, color = Capy.TextSecondary)
            }
        }
        item.value?.let {
            Box(Modifier.padding(start = 12.dp)) {
                Text(
                    if (item.onStep != null && focused) "‹  $it  ›" else it,
                    style = MaterialTheme.typography.titleMedium,
                    color = Capy.Accent,
                )
            }
        }
    }
}
