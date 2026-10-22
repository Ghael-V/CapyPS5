// SPDX-FileCopyrightText: Copyright 2026 CapyPS5 Project
// SPDX-License-Identifier: GPL-2.0-only

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
import androidx.compose.material.icons.automirrored.outlined.ArrowBack
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
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.platform.LocalContext
import org.capyps5.emulator.ButtonLayout
import androidx.compose.ui.res.stringResource
import org.capyps5.emulator.GpuDrivers
import org.capyps5.emulator.R
import org.capyps5.emulator.CoreOption
import org.capyps5.emulator.OverlayMode
import org.capyps5.emulator.Settings
import org.capyps5.emulator.TouchControlsMode
import org.capyps5.emulator.ui.NavKey
import org.capyps5.emulator.ui.OnNavKey
import org.capyps5.emulator.ui.theme.Capy

/** Title of a full-screen page with a back arrow in front of it. */
@Composable
fun ScreenHeader(title: String, onBack: () -> Unit) {
    Row(
        Modifier
            .fillMaxWidth()
            .clickable(onClick = onBack)
            .padding(start = 14.dp, end = 16.dp, top = 4.dp, bottom = 14.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Icon(
            Icons.AutoMirrored.Outlined.ArrowBack, stringResource(R.string.hint_back),
            Modifier.padding(end = 10.dp).size(24.dp), tint = Capy.TextPrimary,
        )
        Text(title, style = MaterialTheme.typography.headlineSmall, maxLines = 1, overflow = TextOverflow.Ellipsis)
    }
}

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

private fun openLink(context: android.content.Context, url: Int) {
    runCatching {
        context.startActivity(
            android.content.Intent(android.content.Intent.ACTION_VIEW, android.net.Uri.parse(context.getString(url)))
        )
    }
}

fun <T> cycle(values: List<T>, current: T, delta: Int): T =
    values[(values.indexOf(current).coerceAtLeast(0) + delta).mod(values.size)]

/** Full-screen settings with categories on the left, usable with touch or a controller. */
@Composable
fun SettingsScreen(
    settings: Settings,
    versionText: String,
    onAddFolder: () -> Unit,
    onImportDriver: () -> Unit,
    driverMessage: String?,
    onClearShaderCache: () -> String,
    onSaveLog: () -> String?,
    onClose: () -> Unit,
) {
    val context = LocalContext.current
    var message by remember { mutableStateOf<String?>(null) }
    var showDrivers by remember { mutableStateOf(false) }
    // Read so the option rows recompose when a value changes.
    val optionsVersion = settings.coreOptionsVersion

    fun coreItem(option: CoreOption): SettingItem {
        val values = option.choices.map { it.first }
        val current = settings.coreOption(option)
        return SettingItem(
            option.title(context), option.description(context), option.label(context, current),
            onStep = { settings.updateCoreOption(option, cycle(values, current, it)) },
        )
    }

    val categories = remember(optionsVersion, settings.libraryPaths, message, driverMessage, settings.updateChecks) {
        listOf(
            SettingsCategory(context.getString(R.string.category_graphics), Icons.Outlined.Image) {
                val drivers = GpuDrivers.list(context)
                val ids = listOf("") + drivers.map { it.id }
                val current = settings.gpuDriver().takeIf { id -> id in ids }.orEmpty()
                val scales = CoreOption.ResolutionScale.choices.map { it.first }
                val ueScale = settings.ueResolution()
                listOf(
                    coreItem(CoreOption.ResolutionScale),
                    SettingItem(
                        context.getString(R.string.ue_resolution),
                        context.getString(R.string.ue_resolution_desc),
                        CoreOption.ResolutionScale.label(context, ueScale),
                        onStep = { settings.updateUeResolution(cycle(scales, ueScale, it)) },
                    ),
                    SettingItem(
                        context.getString(R.string.gpu_driver),
                        context.getString(R.string.gpu_driver_desc),
                        drivers.firstOrNull { it.id == current }?.label ?: context.getString(R.string.gpu_driver_system),
                        onStep = { settings.updateGpuDriver(cycle(ids, current, it)) },
                    ),
                    SettingItem(
                        context.getString(R.string.driver_download), context.getString(R.string.driver_download_desc),
                        onConfirm = { showDrivers = true },
                    ),
                    SettingItem(
                        context.getString(R.string.gpu_driver_import),
                        context.getString(R.string.gpu_driver_import_desc),
                        onConfirm = onImportDriver,
                    ),
                ) + listOfNotNull(
                    drivers.firstOrNull { it.id == current }?.let { driver ->
                        SettingItem(
                            context.getString(R.string.gpu_driver_delete, driver.label),
                            context.getString(R.string.gpu_driver_delete_desc), danger = true,
                            onConfirm = {
                                GpuDrivers.delete(context, driver.id)
                                settings.updateGpuDriver("")
                            },
                        )
                    },
                    driverMessage?.let { SettingItem(it) },
                )
            },
            SettingsCategory(context.getString(R.string.category_performance), Icons.Outlined.Speed) {
                listOf(
                    coreItem(CoreOption.AsyncShaders),
                    SettingItem(
                        context.getString(R.string.perf_overlay), context.getString(R.string.perf_overlay_desc),
                        settings.overlayMode.label(context),
                        onStep = { settings.updateOverlayMode(cycle(OverlayMode.entries, settings.overlayMode, it)) },
                    ),
                )
            },
            SettingsCategory(context.getString(R.string.category_controls), Icons.Outlined.SportsEsports) {
                listOf(
                    SettingItem(
                        context.getString(R.string.touch_controls), context.getString(R.string.touch_controls_desc),
                        settings.touchControls.label(context),
                        onStep = {
                            settings.updateTouchControls(cycle(TouchControlsMode.entries, settings.touchControls, it))
                        },
                    ),
                    SettingItem(
                        context.getString(R.string.touchpad_overlay),
                        context.getString(R.string.touchpad_overlay_desc),
                        settings.touchpadOverlay.label(context),
                        onStep = {
                            settings.updateTouchpadOverlay(cycle(TouchControlsMode.entries, settings.touchpadOverlay, it))
                        },
                    ),
                    SettingItem(
                        context.getString(R.string.button_layout),
                        context.getString(R.string.button_layout_desc),
                        settings.buttonLayout.label(context),
                        onStep = { settings.updateButtonLayout(cycle(ButtonLayout.entries, settings.buttonLayout, it)) },
                    ),
                    SettingItem(
                        context.getString(R.string.controls_opacity), null, "%.0f %%".format(settings.controlsOpacity * 100),
                        onStep = { settings.updateControlsOpacity((settings.controlsOpacity + it * 0.1f).coerceIn(0.1f, 1f)) },
                    ),
                )
            },
            SettingsCategory(context.getString(R.string.category_folders), Icons.Outlined.Folder) {
                settings.libraryPaths.map { path ->
                    SettingItem(
                        path,
                        context.getString(
                            if (settings.libraryPaths.size > 1) R.string.folder_remove else R.string.folder_main,
                        ),
                        onConfirm = if (settings.libraryPaths.size > 1) ({ settings.removeLibraryPath(path) }) else null,
                    )
                } + SettingItem(
                    context.getString(R.string.folder_add), context.getString(R.string.folder_add_desc),
                    onConfirm = onAddFolder,
                )
            },
            SettingsCategory(context.getString(R.string.category_system), Icons.Outlined.Settings) {
                listOf(
                    SettingItem(
                        context.getString(R.string.update_checks), context.getString(R.string.update_checks_desc),
                        context.getString(if (settings.updateChecks) R.string.enabled else R.string.disabled),
                        onStep = { settings.updateUpdateChecks(!settings.updateChecks) },
                    ),
                    SettingItem(
                        context.getString(R.string.clear_shader_cache), context.getString(R.string.clear_shader_cache_desc),
                        danger = true,
                        onConfirm = { message = onClearShaderCache() },
                    ),
                    SettingItem(
                        context.getString(R.string.save_log), context.getString(R.string.save_log_desc),
                        onConfirm = {
                            message = onSaveLog()?.let { context.getString(R.string.log_saved_in, it) }
                                ?: context.getString(R.string.log_save_failed)
                        },
                    ),
                ) + listOfNotNull(message?.let { SettingItem(it) })
            },
            SettingsCategory(context.getString(R.string.category_about), Icons.Outlined.Info) {
                listOf(
                    SettingItem("CapyPS5", "$versionText (Ghael-V)"),
                    SettingItem(
                        context.getString(R.string.support_discord), context.getString(R.string.discord_link),
                        onConfirm = { openLink(context, R.string.discord_link) },
                    ),
                    SettingItem(
                        context.getString(R.string.support_kofi), context.getString(R.string.kofi_link),
                        onConfirm = { openLink(context, R.string.kofi_link) },
                    ),
                    SettingItem(
                        context.getString(R.string.support_buymeacoffee), context.getString(R.string.buymeacoffee_link),
                        onConfirm = { openLink(context, R.string.buymeacoffee_link) },
                    ),
                    SettingItem(
                        context.getString(R.string.license),
                        context.getString(R.string.license_desc),
                    ),
                    SettingItem(
                        context.getString(R.string.device),
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

    OnNavKey(enabled = !showDrivers) { key ->
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

    if (showDrivers) {
        DriverDownloadScreen(settings, onClose = { showDrivers = false })
        return
    }
    Row(Modifier.fillMaxSize().background(Capy.Background)) {
        Column(
            Modifier.width(260.dp).fillMaxHeight().background(Capy.Surface).padding(vertical = 22.dp),
            verticalArrangement = Arrangement.spacedBy(2.dp),
        ) {
            // The system bars are hidden, so touch users need their own way back.
            ScreenHeader(stringResource(R.string.settings), onBack = onClose)
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
