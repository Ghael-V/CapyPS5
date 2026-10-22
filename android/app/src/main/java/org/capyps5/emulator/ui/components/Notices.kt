// SPDX-FileCopyrightText: Copyright 2026 CapyPS5 Project
// SPDX-License-Identifier: GPL-2.0-only

package org.capyps5.emulator.ui.components

import android.content.Intent
import android.net.Uri
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.outlined.Forum
import androidx.compose.material.icons.outlined.LocalCafe
import androidx.compose.material.icons.outlined.SystemUpdate
import androidx.compose.material.icons.outlined.VolunteerActivism
import androidx.compose.material3.Icon
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableFloatStateOf
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.unit.dp
import kotlinx.coroutines.launch
import org.capyps5.emulator.R
import org.capyps5.emulator.Release
import org.capyps5.emulator.Updater
import org.capyps5.emulator.ui.NavKey
import org.capyps5.emulator.ui.OnNavKey
import org.capyps5.emulator.ui.theme.Capy
import java.io.File

/** A button of a notice: label, optional icon and action. */
class NoticeAction(val label: String, val icon: ImageVector? = null, val onClick: () -> Unit)

/**
 * Centered card over the launcher, driven by touch or a controller: the actions are a row
 * (left/right) or, when [stacked], a column (up/down); Ⓑ runs [onBack].
 */
@Composable
fun NoticeDialog(
    title: String,
    icon: ImageVector,
    actions: List<NoticeAction>,
    onBack: () -> Unit,
    stacked: Boolean = false,
    content: @Composable () -> Unit,
) {
    var focused by remember(actions.size) { mutableIntStateOf(0) }
    OnNavKey { key ->
        when (key) {
            NavKey.Left, NavKey.Up -> focused = (focused - 1).mod(actions.size)
            NavKey.Right, NavKey.Down -> focused = (focused + 1).mod(actions.size)
            NavKey.Confirm -> actions.getOrNull(focused)?.onClick?.invoke()
            NavKey.Back -> onBack()
            else -> Unit
        }
    }
    Box(Modifier.fillMaxSize().background(Capy.Scrim), contentAlignment = Alignment.Center) {
        Column(
            modifier = Modifier
                .width(560.dp)
                .background(Capy.Surface, RoundedCornerShape(16.dp))
                .padding(22.dp),
            verticalArrangement = Arrangement.spacedBy(12.dp),
        ) {
            Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(10.dp)) {
                Icon(icon, contentDescription = null, tint = Capy.Accent, modifier = Modifier.size(28.dp))
                Text(title, style = MaterialTheme.typography.titleLarge, color = Capy.TextPrimary)
            }
            content()
            val button: @Composable (Int, NoticeAction, Modifier) -> Unit = { i, action, modifier ->
                Row(
                    modifier = modifier
                        .background(if (i == focused) Capy.Accent else Capy.SurfaceHigh, RoundedCornerShape(50))
                        .clickable { focused = i; action.onClick() }
                        .padding(horizontal = 18.dp, vertical = 9.dp),
                    verticalAlignment = Alignment.CenterVertically,
                    horizontalArrangement = Arrangement.spacedBy(8.dp, Alignment.CenterHorizontally),
                ) {
                    action.icon?.let { Icon(it, contentDescription = null, tint = Color.White, modifier = Modifier.size(18.dp)) }
                    Text(action.label, style = MaterialTheme.typography.labelLarge, color = Color.White)
                }
            }
            if (stacked) {
                Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
                    actions.forEachIndexed { i, action -> button(i, action, Modifier.fillMaxWidth()) }
                }
            } else {
                Row(horizontalArrangement = Arrangement.spacedBy(10.dp)) {
                    actions.forEachIndexed { i, action -> button(i, action, Modifier) }
                }
            }
        }
    }
}

private sealed interface UpdateState {
    data object Available : UpdateState
    data object Downloading : UpdateState
    data class Ready(val apk: File) : UpdateState
    data object Failed : UpdateState
}

/** Offers [release]: download it with progress, then open the system installer. */
@Composable
fun UpdateDialog(release: Release, installed: String, onSkip: () -> Unit, onClose: () -> Unit) {
    val context = LocalContext.current
    val scope = rememberCoroutineScope()
    var state by remember { mutableStateOf<UpdateState>(UpdateState.Available) }
    var progress by remember { mutableFloatStateOf(0f) }
    var needsPermission by remember { mutableStateOf(false) }

    fun install(apk: File) {
        if (!Updater.canInstall(context)) {
            needsPermission = true
            Updater.requestInstallPermission(context)
            return
        }
        needsPermission = false
        if (!Updater.install(context, apk)) state = UpdateState.Failed
    }

    fun download() {
        state = UpdateState.Downloading
        progress = 0f
        scope.launch {
            val apk = Updater.download(context, release) { progress = it }
            state = if (apk != null) UpdateState.Ready(apk) else UpdateState.Failed
            if (apk != null) install(apk)
        }
    }

    val later = NoticeAction(stringResource(R.string.update_later), onClick = onClose)
    val actions = when (val current = state) {
        UpdateState.Available -> listOf(
            NoticeAction(stringResource(R.string.update_download), Icons.Outlined.SystemUpdate, ::download),
            later,
            NoticeAction(stringResource(R.string.update_skip), onClick = onSkip),
        )
        UpdateState.Downloading -> emptyList()
        is UpdateState.Ready -> listOf(
            NoticeAction(stringResource(R.string.update_install), Icons.Outlined.SystemUpdate) { install(current.apk) },
            later,
        )
        UpdateState.Failed -> listOf(NoticeAction(stringResource(R.string.update_retry), onClick = ::download), later)
    }
    NoticeDialog(
        title = stringResource(R.string.update_title),
        icon = Icons.Outlined.SystemUpdate,
        actions = actions,
        onBack = { if (state != UpdateState.Downloading) onClose() },
    ) {
        Text(
            stringResource(R.string.update_versions, release.name, installed),
            style = MaterialTheme.typography.bodyMedium,
            color = Capy.TextSecondary,
        )
        if (release.notes.isNotEmpty()) {
            Text(
                release.notes,
                style = MaterialTheme.typography.bodySmall,
                color = Capy.TextPrimary,
                modifier = Modifier
                    .heightIn(max = 180.dp)
                    .fillMaxWidth()
                    .background(Capy.SurfaceHigh, RoundedCornerShape(8.dp))
                    .padding(12.dp)
                    .verticalScroll(rememberScrollState()),
            )
        }
        when (state) {
            UpdateState.Downloading -> {
                LinearProgressIndicator(progress = { progress }, modifier = Modifier.fillMaxWidth(), color = Capy.Accent)
                Text(
                    stringResource(R.string.update_downloading, (progress * 100).toInt()),
                    style = MaterialTheme.typography.bodySmall,
                    color = Capy.TextSecondary,
                )
            }
            UpdateState.Failed -> Text(
                stringResource(R.string.update_failed),
                style = MaterialTheme.typography.bodySmall,
                color = Capy.Bad,
            )
            else -> Unit
        }
        if (needsPermission) {
            Text(
                stringResource(R.string.update_permission),
                style = MaterialTheme.typography.bodySmall,
                color = Capy.Warn,
            )
        }
        Spacer(Modifier.size(2.dp))
    }
}

/** Community and donation links, shown every few launches. */
@Composable
fun SupportDialog(onClose: () -> Unit) {
    val context = LocalContext.current
    fun open(url: Int) {
        runCatching {
            context.startActivity(
                Intent(Intent.ACTION_VIEW, Uri.parse(context.getString(url))).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
            )
        }
        onClose()
    }
    NoticeDialog(
        title = stringResource(R.string.support_title),
        icon = Icons.Outlined.VolunteerActivism,
        stacked = true,
        actions = listOf(
            NoticeAction(stringResource(R.string.support_discord), Icons.Outlined.Forum) { open(R.string.discord_link) },
            NoticeAction(stringResource(R.string.support_kofi), Icons.Outlined.LocalCafe) { open(R.string.kofi_link) },
            NoticeAction(stringResource(R.string.support_buymeacoffee), Icons.Outlined.LocalCafe) {
                open(R.string.buymeacoffee_link)
            },
            NoticeAction(stringResource(R.string.close), onClick = onClose),
        ),
        onBack = onClose,
    ) {
        Text(
            stringResource(R.string.support_description),
            style = MaterialTheme.typography.bodyMedium,
            color = Capy.TextSecondary,
        )
    }
}
