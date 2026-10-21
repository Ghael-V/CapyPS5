// SPDX-FileCopyrightText: Copyright 2026 CapyPS5 Project
// SPDX-License-Identifier: GPL-2.0-only

package org.capyps5.emulator.ui.components

import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxHeight
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Slider
import androidx.compose.material3.SliderDefaults
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.unit.dp
import org.capyps5.emulator.OverlayMode
import org.capyps5.emulator.Settings
import org.capyps5.emulator.TouchControlsMode
import org.capyps5.emulator.ui.NavKey
import org.capyps5.emulator.ui.OnNavKey
import org.capyps5.emulator.ui.theme.Capy
import org.capyps5.emulator.R
import androidx.compose.ui.res.stringResource

/** Side panel with emulator preferences; fully usable with touch or a controller. */
@Composable
fun SettingsPanel(
    settings: Settings,
    versionText: String,
    onClose: () -> Unit,
    modifier: Modifier = Modifier,
    onPickLibrary: (() -> Unit)? = null,
) {
    var focused by remember { mutableIntStateOf(0) }
    val rows = if (onPickLibrary != null) 4 else 3

    fun <T> cycle(values: Array<T>, current: T, delta: Int): T =
        values[(values.indexOf(current) + delta).mod(values.size)]

    fun adjust(row: Int, delta: Int) = when (row) {
        0 -> settings.updateOverlayMode(cycle(OverlayMode.entries.toTypedArray(), settings.overlayMode, delta))
        1 -> settings.updateTouchControls(cycle(TouchControlsMode.entries.toTypedArray(), settings.touchControls, delta))
        2 -> settings.updateControlsOpacity((settings.controlsOpacity + delta * 0.1f).coerceIn(0.1f, 1f))
        else -> Unit
    }

    OnNavKey { key ->
        when (key) {
            NavKey.Up -> focused = (focused - 1).mod(rows)
            NavKey.Down -> focused = (focused + 1).mod(rows)
            NavKey.Left -> adjust(focused, -1)
            NavKey.Confirm -> if (focused == 3) onPickLibrary?.invoke() else adjust(focused, 1)
            NavKey.Right -> adjust(focused, 1)
            NavKey.Back, NavKey.Menu, NavKey.Secondary -> onClose()
            else -> Unit
        }
    }

    Column(
        modifier = modifier
            .fillMaxHeight()
            .width(340.dp)
            .background(Capy.Surface)
            .verticalScroll(rememberScrollState())
            .padding(20.dp),
        verticalArrangement = Arrangement.spacedBy(14.dp),
    ) {
        Row(verticalAlignment = Alignment.CenterVertically) {
            Text(stringResource(R.string.settings), style = MaterialTheme.typography.titleLarge, modifier = Modifier.weight(1f))
            Text(
                stringResource(R.string.close_b),
                style = MaterialTheme.typography.labelLarge,
                color = Capy.TextSecondary,
                modifier = Modifier.clickable(onClick = onClose).padding(6.dp),
            )
        }

        SettingRow(stringResource(R.string.perf_overlay), stringResource(R.string.perf_overlay_desc), focused == 0) {
            Segmented(OverlayMode.entries.map { stringResource(it.labelRes) }, OverlayMode.entries.indexOf(settings.overlayMode)) {
                focused = 0
                settings.updateOverlayMode(OverlayMode.entries[it])
            }
        }
        SettingRow(stringResource(R.string.touch_controls), stringResource(R.string.touch_controls_desc), focused == 1) {
            Segmented(
                TouchControlsMode.entries.map { stringResource(it.labelRes) },
                TouchControlsMode.entries.indexOf(settings.touchControls),
            ) {
                focused = 1
                settings.updateTouchControls(TouchControlsMode.entries[it])
            }
        }
        SettingRow(stringResource(R.string.controls_opacity), "%.0f%%".format(settings.controlsOpacity * 100), focused == 2) {
            Slider(
                value = settings.controlsOpacity,
                onValueChange = {
                    focused = 2
                    settings.updateControlsOpacity(it)
                },
                valueRange = 0.1f..1f,
                colors = SliderDefaults.colors(thumbColor = Capy.Accent, activeTrackColor = Capy.Accent),
            )
        }
        if (onPickLibrary != null) {
            SettingRow(stringResource(R.string.category_folders), stringResource(R.string.folder_add_desc), focused == 3) {
                settings.libraryPaths.forEach { path ->
                    Row(verticalAlignment = Alignment.CenterVertically) {
                        Text(
                            path,
                            style = MaterialTheme.typography.bodySmall,
                            color = Capy.TextPrimary,
                            modifier = Modifier.weight(1f),
                        )
                        if (settings.libraryPaths.size > 1) {
                            Text(
                                "✕",
                                style = MaterialTheme.typography.labelLarge,
                                color = Capy.Bad,
                                modifier = Modifier
                                    .clickable { settings.removeLibraryPath(path) }
                                    .padding(horizontal = 8.dp, vertical = 4.dp),
                            )
                        }
                    }
                }
                Box(
                    modifier = Modifier
                        .fillMaxWidth()
                        .background(Capy.Accent, RoundedCornerShape(8.dp))
                        .clickable {
                            focused = 3
                            onPickLibrary()
                        }
                        .padding(vertical = 8.dp),
                    contentAlignment = Alignment.Center,
                ) {
                    Text(stringResource(R.string.folder_add_a), style = MaterialTheme.typography.labelLarge, color = Color.White)
                }
            }
        }

        Spacer(Modifier.height(4.dp))
        Text(versionText, style = MaterialTheme.typography.bodySmall, color = Capy.TextMuted)
        Text(
            "${android.os.Build.MANUFACTURER} ${android.os.Build.MODEL} · ${android.os.Build.SOC_MODEL} · Android ${android.os.Build.VERSION.RELEASE}",
            style = MaterialTheme.typography.bodySmall,
            color = Capy.TextMuted,
        )
    }
}

@Composable
private fun SettingRow(title: String, subtitle: String, focused: Boolean, content: @Composable () -> Unit) {
    Column(
        modifier = Modifier
            .fillMaxWidth()
            .background(if (focused) Capy.SurfaceHigh else Color.Transparent, RoundedCornerShape(10.dp))
            .border(1.dp, if (focused) Capy.Accent.copy(alpha = 0.6f) else Color.Transparent, RoundedCornerShape(10.dp))
            .padding(12.dp),
        verticalArrangement = Arrangement.spacedBy(8.dp),
    ) {
        Text(title, style = MaterialTheme.typography.titleMedium)
        Text(subtitle, style = MaterialTheme.typography.bodySmall, color = Capy.TextSecondary)
        content()
    }
}

@Composable
private fun Segmented(options: List<String>, selected: Int, onSelect: (Int) -> Unit) {
    Row(
        modifier = Modifier
            .fillMaxWidth()
            .background(Capy.Background, RoundedCornerShape(8.dp))
            .padding(3.dp),
        horizontalArrangement = Arrangement.spacedBy(3.dp),
    ) {
        options.forEachIndexed { i, label ->
            Box(
                modifier = Modifier
                    .weight(1f)
                    .background(if (i == selected) Capy.Accent else Color.Transparent, RoundedCornerShape(6.dp))
                    .clickable { onSelect(i) }
                    .padding(vertical = 7.dp),
                contentAlignment = Alignment.Center,
            ) {
                Text(
                    label,
                    style = MaterialTheme.typography.labelLarge,
                    color = if (i == selected) Color.White else Capy.TextSecondary,
                )
            }
        }
    }
}
