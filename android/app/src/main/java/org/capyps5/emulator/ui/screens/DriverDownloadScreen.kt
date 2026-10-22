// SPDX-FileCopyrightText: Copyright 2026 CapyPS5 Project
// SPDX-License-Identifier: GPL-2.0-only

package org.capyps5.emulator.ui.screens

import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxHeight
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.itemsIndexed
import androidx.compose.foundation.lazy.rememberLazyListState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableFloatStateOf
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateMapOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import kotlinx.coroutines.launch
import org.capyps5.emulator.DriverCatalog
import org.capyps5.emulator.DriverPackage
import org.capyps5.emulator.R
import org.capyps5.emulator.Settings
import org.capyps5.emulator.ui.NavKey
import org.capyps5.emulator.ui.OnNavKey
import org.capyps5.emulator.ui.theme.Capy
import java.time.ZoneId
import java.time.format.DateTimeFormatter

/** Lists community driver builds for this GPU and installs the chosen one as the global driver. */
@Composable
fun DriverDownloadScreen(settings: Settings, onClose: () -> Unit) {
    val context = LocalContext.current
    val scope = rememberCoroutineScope()
    val sources = remember { DriverCatalog.sources() }
    // Per source: its packages, or the error that stopped the listing.
    val listings = remember { mutableStateMapOf<Int, Result<List<DriverPackage>>>() }
    var source by remember { mutableIntStateOf(0) }
    var inContent by remember { mutableStateOf(false) }
    var row by remember { mutableIntStateOf(0) }
    var installing by remember { mutableStateOf<DriverPackage?>(null) }
    var progress by remember { mutableFloatStateOf(0f) }
    var message by remember { mutableStateOf<String?>(null) }
    val listState = rememberLazyListState()

    LaunchedEffect(source) {
        if (source !in listings.keys && sources.isNotEmpty()) listings[source] = DriverCatalog.packages(sources[source])
    }
    val packages = listings[source]?.getOrNull().orEmpty()
    LaunchedEffect(row, inContent) { if (inContent && packages.isNotEmpty()) listState.animateScrollToItem(row) }

    fun install(pkg: DriverPackage) {
        if (installing != null) return
        installing = pkg
        progress = 0f
        message = null
        scope.launch {
            message = DriverCatalog.install(context, pkg) { progress = it }.fold(
                onSuccess = { driver ->
                    settings.updateGpuDriver(driver.id)
                    context.getString(R.string.gpu_driver_installed, driver.label)
                },
                onFailure = { context.getString(R.string.gpu_driver_import_failed, it.message.orEmpty()) },
            )
            installing = null
        }
    }

    OnNavKey { key ->
        if (installing != null) return@OnNavKey
        if (!inContent) {
            when (key) {
                NavKey.Up -> { source = (source - 1).mod(sources.size.coerceAtLeast(1)); row = 0 }
                NavKey.Down -> { source = (source + 1).mod(sources.size.coerceAtLeast(1)); row = 0 }
                NavKey.Right, NavKey.Confirm -> if (packages.isNotEmpty()) inContent = true
                NavKey.Back, NavKey.Menu -> onClose()
                else -> Unit
            }
        } else {
            when (key) {
                NavKey.Up -> row = (row - 1).mod(packages.size.coerceAtLeast(1))
                NavKey.Down -> row = (row + 1).mod(packages.size.coerceAtLeast(1))
                NavKey.Confirm -> packages.getOrNull(row)?.let(::install)
                NavKey.Left, NavKey.Back -> inContent = false
                NavKey.Menu -> onClose()
                else -> Unit
            }
        }
    }

    Row(Modifier.fillMaxSize().background(Capy.Background)) {
        Column(
            Modifier.width(300.dp).fillMaxHeight().background(Capy.Surface).padding(vertical = 22.dp),
            verticalArrangement = Arrangement.spacedBy(2.dp),
        ) {
            ScreenHeader(stringResource(R.string.driver_download_title), onBack = onClose)
            sources.forEachIndexed { i, entry ->
                Text(
                    entry.name,
                    style = MaterialTheme.typography.titleSmall,
                    color = if (i == source) Capy.TextPrimary else Capy.TextSecondary,
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis,
                    modifier = Modifier
                        .fillMaxWidth()
                        .background(if (i == source) Capy.SurfaceHigh else Color.Transparent)
                        .clickable { source = i; row = 0; inContent = false }
                        .padding(horizontal = 24.dp, vertical = 11.dp),
                )
            }
        }
        Column(Modifier.weight(1f).fillMaxHeight().padding(horizontal = 28.dp, vertical = 22.dp)) {
            val current = sources.getOrNull(source)
            Text(current?.repo.orEmpty(), style = MaterialTheme.typography.bodySmall, color = Capy.TextMuted)
            Spacer(Modifier.height(10.dp))
            installing?.let { pkg ->
                Text(stringResource(R.string.driver_downloading, pkg.title), style = MaterialTheme.typography.bodyMedium)
                LinearProgressIndicator(progress = { progress }, modifier = Modifier.fillMaxWidth().padding(vertical = 8.dp), color = Capy.Accent)
            }
            message?.let {
                Text(it, style = MaterialTheme.typography.bodyMedium, color = Capy.TextPrimary, modifier = Modifier.padding(bottom = 8.dp))
            }
            val listing = listings[source]
            when {
                sources.isEmpty() -> Text(stringResource(R.string.driver_none_for_gpu), color = Capy.TextSecondary)
                listing == null -> Text(stringResource(R.string.driver_loading), color = Capy.TextSecondary)
                listing.isFailure -> Text(stringResource(R.string.driver_list_failed), color = Capy.Bad)
                packages.isEmpty() -> Text(stringResource(R.string.driver_no_packages), color = Capy.TextSecondary)
                else -> LazyColumn(state = listState, verticalArrangement = Arrangement.spacedBy(6.dp)) {
                    itemsIndexed(packages) { i, pkg ->
                        val focused = inContent && i == row
                        Column(
                            Modifier
                                .fillMaxWidth()
                                .background(if (focused) Capy.AccentSoft else Capy.Surface, RoundedCornerShape(10.dp))
                                .clickable { row = i; inContent = true; install(pkg) }
                                .padding(horizontal = 16.dp, vertical = 10.dp),
                        ) {
                            Text(
                                pkg.title + if (pkg.prerelease) " · pre-release" else "",
                                style = MaterialTheme.typography.titleSmall,
                                color = Capy.TextPrimary,
                                maxLines = 1,
                                overflow = TextOverflow.Ellipsis,
                            )
                            Text(
                                listOfNotNull(
                                    pkg.assetName,
                                    pkg.published?.atZone(ZoneId.systemDefault())?.format(DATE),
                                    pkg.size.takeIf { it > 0 }?.let { "%.1f MB".format(it / 1048576.0) },
                                ).joinToString(" · "),
                                style = MaterialTheme.typography.bodySmall,
                                color = Capy.TextSecondary,
                                maxLines = 1,
                                overflow = TextOverflow.Ellipsis,
                            )
                        }
                    }
                }
            }
        }
    }
}

private val DATE = DateTimeFormatter.ofPattern("yyyy-MM-dd")
