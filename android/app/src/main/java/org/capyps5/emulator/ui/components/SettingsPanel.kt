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
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.unit.dp
import org.capyps5.emulator.ButtonLayout
import org.capyps5.emulator.CoreOption
import org.capyps5.emulator.NativeBridge
import org.capyps5.emulator.OverlayMode
import org.capyps5.emulator.R
import org.capyps5.emulator.Settings
import org.capyps5.emulator.TouchControlsMode
import org.capyps5.emulator.model.GameItem
import org.capyps5.emulator.ui.NavKey
import org.capyps5.emulator.ui.OnNavKey
import org.capyps5.emulator.ui.theme.Capy

/** Core options the core reads while a game runs, so the in-game panel can change them live. */
private val LiveOptions = listOf(CoreOption.FrameLimit, CoreOption.FrameGen, CoreOption.AsyncShaders)

/** What a focused row does with the controller: left/right adjusts it, confirm acts on it. */
private class RowAction(val adjust: (Int) -> Unit, val confirm: (() -> Unit)?)

/**
 * Side panel with emulator preferences; fully usable with touch or a controller. With [game] it is
 * the in-game panel: it also offers the button layout and the core options that apply live, saved
 * as that game's own values.
 */
@Composable
fun SettingsPanel(
    settings: Settings,
    versionText: String,
    onClose: () -> Unit,
    modifier: Modifier = Modifier,
    onPickLibrary: (() -> Unit)? = null,
    game: GameItem? = null,
) {
    val context = LocalContext.current
    var focused by remember { mutableIntStateOf(0) }
    val unreal = remember(game) { game?.isUnreal ?: false }
    // Read so the rows recompose when a core option changes.
    @Suppress("UNUSED_VARIABLE")
    val optionsVersion = settings.coreOptionsVersion

    // Rows register their controller actions in display order while composing.
    val actions = mutableListOf<RowAction>()
    fun register(adjust: (Int) -> Unit, confirm: (() -> Unit)? = null): Int {
        actions += RowAction(adjust, confirm)
        return actions.lastIndex
    }
    fun <T> cycle(values: List<T>, current: T, delta: Int): T =
        values[(values.indexOf(current) + delta).mod(values.size)]

    fun setGameOption(game: GameItem, option: CoreOption, value: Long) {
        settings.updateGameOption(game.titleId, option, value)
        NativeBridge.nativeSetOption(option.property, value)
    }

    OnNavKey { key ->
        val rows = actions.size.coerceAtLeast(1)
        when (key) {
            NavKey.Up -> focused = (focused - 1).mod(rows)
            NavKey.Down -> focused = (focused + 1).mod(rows)
            NavKey.Left -> actions.getOrNull(focused)?.adjust?.invoke(-1)
            NavKey.Confirm -> actions.getOrNull(focused)?.let { it.confirm?.invoke() ?: it.adjust(1) }
            NavKey.Right -> actions.getOrNull(focused)?.adjust?.invoke(1)
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

        val overlayRow = register({ settings.updateOverlayMode(cycle(OverlayMode.entries, settings.overlayMode, it)) })
        SettingRow(stringResource(R.string.perf_overlay), stringResource(R.string.perf_overlay_desc), focused == overlayRow) {
            Segmented(OverlayMode.entries.map { stringResource(it.labelRes) }, OverlayMode.entries.indexOf(settings.overlayMode)) {
                focused = overlayRow
                settings.updateOverlayMode(OverlayMode.entries[it])
            }
        }
        val touchRow = register({
            settings.updateTouchControls(cycle(TouchControlsMode.entries, settings.touchControls, it))
        })
        SettingRow(stringResource(R.string.touch_controls), stringResource(R.string.touch_controls_desc), focused == touchRow) {
            Segmented(
                TouchControlsMode.entries.map { stringResource(it.labelRes) },
                TouchControlsMode.entries.indexOf(settings.touchControls),
            ) {
                focused = touchRow
                settings.updateTouchControls(TouchControlsMode.entries[it])
            }
        }
        val opacityRow = register({
            settings.updateControlsOpacity((settings.controlsOpacity + it * 0.1f).coerceIn(0.1f, 1f))
        })
        SettingRow(stringResource(R.string.controls_opacity), "%.0f%%".format(settings.controlsOpacity * 100), focused == opacityRow) {
            Slider(
                value = settings.controlsOpacity,
                onValueChange = {
                    focused = opacityRow
                    settings.updateControlsOpacity(it)
                },
                valueRange = 0.1f..1f,
                colors = SliderDefaults.colors(thumbColor = Capy.Accent, activeTrackColor = Capy.Accent),
            )
        }

        if (game != null) {
            val touchpadRow = register({
                settings.updateTouchpadOverlay(cycle(TouchControlsMode.entries, settings.touchpadOverlay, it))
            })
            SettingRow(stringResource(R.string.touchpad_overlay), stringResource(R.string.touchpad_overlay_desc), focused == touchpadRow) {
                Segmented(
                    TouchControlsMode.entries.map { stringResource(it.labelRes) },
                    TouchControlsMode.entries.indexOf(settings.touchpadOverlay),
                ) {
                    focused = touchpadRow
                    settings.updateTouchpadOverlay(TouchControlsMode.entries[it])
                }
            }
            val layoutRow = register({ settings.updateButtonLayout(cycle(ButtonLayout.entries, settings.buttonLayout, it)) })
            SettingRow(stringResource(R.string.button_layout), stringResource(R.string.button_layout_desc), focused == layoutRow) {
                Segmented(ButtonLayout.entries.map { stringResource(it.labelRes) }, ButtonLayout.entries.indexOf(settings.buttonLayout)) {
                    focused = layoutRow
                    settings.updateButtonLayout(ButtonLayout.entries[it])
                }
            }
            LiveOptions.forEach { option ->
                val values = option.choices.map { it.first }
                val current = settings.effectiveOption(game.titleId, option, unreal)
                val row = register({ setGameOption(game, option, cycle(values, current, it)) })
                val own = settings.gameOption(game.titleId, option) != null
                SettingRow(
                    option.title(context),
                    if (own) stringResource(R.string.ingame_option_saved) else option.description(context),
                    focused == row,
                ) {
                    Segmented(option.choices.map { stringResource(it.second) }, values.indexOf(current)) {
                        focused = row
                        setGameOption(game, option, values[it])
                    }
                }
            }
            if (settings.hasGameOverrides(game.titleId)) {
                val reset = {
                    settings.resetGameOptions(game.titleId)
                    // Only the live options change now; the rest apply at the next launch.
                    LiveOptions.forEach { option ->
                        NativeBridge.nativeSetOption(option.property, settings.effectiveOption(game.titleId, option, unreal))
                    }
                }
                val resetRow = register({}, reset)
                SettingRow(stringResource(R.string.reset_to_global), stringResource(R.string.reset_to_global_desc), focused == resetRow) {
                    ActionButton(stringResource(R.string.reset_to_global)) {
                        focused = resetRow
                        reset()
                    }
                }
            } else {
                Text(stringResource(R.string.game_uses_global), style = MaterialTheme.typography.bodySmall, color = Capy.TextMuted)
            }
        }

        if (onPickLibrary != null) {
            val folderRow = register({}, onPickLibrary)
            SettingRow(stringResource(R.string.category_folders), stringResource(R.string.folder_add_desc), focused == folderRow) {
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
                ActionButton(stringResource(R.string.folder_add_a)) {
                    focused = folderRow
                    onPickLibrary()
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
private fun ActionButton(label: String, onClick: () -> Unit) {
    Box(
        modifier = Modifier
            .fillMaxWidth()
            .background(Capy.Accent, RoundedCornerShape(8.dp))
            .clickable(onClick = onClick)
            .padding(vertical = 8.dp),
        contentAlignment = Alignment.Center,
    ) {
        Text(label, style = MaterialTheme.typography.labelLarge, color = Color.White)
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
