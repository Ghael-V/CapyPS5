// SPDX-FileCopyrightText: Copyright 2026 CapyPS5 Project
// SPDX-License-Identifier: GPL-2.0-only

package org.capyps5.emulator.aiconfig

import android.annotation.SuppressLint
import android.content.Context
import android.content.Intent
import android.net.Uri
import android.widget.Toast
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.outlined.AutoAwesome
import androidx.compose.material.icons.automirrored.outlined.Undo
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import org.capyps5.emulator.BuildConfig
import org.capyps5.emulator.R
import org.capyps5.emulator.Settings
import org.capyps5.emulator.model.GameItem
import org.capyps5.emulator.ui.components.NoticeAction
import org.capyps5.emulator.ui.components.NoticeDialog
import org.capyps5.emulator.ui.theme.Capy
import java.util.Locale

/** One change that passed the schema, with the value the game has now. */
private class Checked(
    val entry: AiSchema.Entry,
    val value: Long,
    val current: Long,
    val reason: String,
    val experimental: Boolean,
)

private sealed interface AiState {
    data object Asking : AiState

    class Suggest(val answerId: String, val notes: String, val changes: List<Checked>) : AiState

    class Message(val text: String) : AiState
}

/**
 * "Recommended settings" for [game]: asks the server, shows what would change and why, and writes
 * the accepted changes to the game's own settings. [onClose] ends the flow (also cancels a request).
 */
@Composable
fun AiConfigDialog(game: GameItem, settings: Settings, onClose: () -> Unit) {
    val context = LocalContext.current
    var state by remember { mutableStateOf<AiState>(AiState.Asking) }
    val schema = remember { AiSchema.load(context) }

    LaunchedEffect(Unit) {
        val request = withContext(Dispatchers.IO) { buildRequest(context, settings, schema, game) }
        state = when (val result = AiConfigClient.suggest(request)) {
            is AiConfigClient.Result.Ok -> {
                val changes = check(settings, schema, game, result.suggestion)
                if (changes.isEmpty()) {
                    AiState.Message(
                        buildString {
                            append(context.getString(R.string.ai_config_nothing_to_change))
                            if (result.suggestion.notes.isNotBlank()) append("\n\n").append(result.suggestion.notes)
                        }
                    )
                } else {
                    AiState.Suggest(result.suggestion.answerId, result.suggestion.notes, changes)
                }
            }
            is AiConfigClient.Result.ServerError -> AiState.Message(errorText(context, result.code))
            AiConfigClient.Result.NetworkError -> AiState.Message(context.getString(R.string.ai_config_network_error))
        }
    }

    val privacy = NoticeAction(stringResource(R.string.ai_config_privacy)) { openPrivacy(context) }
    val cancel = NoticeAction(stringResource(R.string.cancel), onClick = onClose)
    when (val current = state) {
        AiState.Asking -> NoticeDialog(
            title = stringResource(R.string.ai_config_title),
            icon = Icons.Outlined.AutoAwesome,
            actions = listOf(cancel, privacy),
            onBack = onClose,
        ) {
            Text(stringResource(R.string.ai_config_asking), style = MaterialTheme.typography.bodyMedium, color = Capy.TextSecondary)
            LinearProgressIndicator(modifier = Modifier.fillMaxWidth(), color = Capy.Accent)
        }
        is AiState.Suggest -> NoticeDialog(
            title = stringResource(R.string.ai_config_title),
            icon = Icons.Outlined.AutoAwesome,
            actions = listOf(
                NoticeAction(stringResource(R.string.ai_config_apply)) {
                    AiConfigStore.apply(
                        context, settings, schema, game, current.answerId,
                        current.changes.associate { it.entry.key to it.value },
                    )
                    Toast.makeText(context, R.string.ai_config_applied, Toast.LENGTH_LONG).show()
                    onClose()
                },
                cancel,
                privacy,
            ),
            onBack = onClose,
        ) {
            Column(
                Modifier.heightIn(max = 300.dp).verticalScroll(rememberScrollState()),
                verticalArrangement = Arrangement.spacedBy(10.dp),
            ) {
                if (current.notes.isNotBlank()) {
                    Text(current.notes, style = MaterialTheme.typography.bodyMedium, color = Capy.TextSecondary)
                }
                current.changes.forEach { change ->
                    Column {
                        val experimental = if (change.experimental) " · " + stringResource(R.string.ai_config_experimental) else ""
                        Text(
                            title(context, change.entry) + experimental,
                            style = MaterialTheme.typography.titleSmall.copy(fontWeight = FontWeight.Bold),
                            color = if (change.experimental) Capy.Warn else Capy.TextPrimary,
                        )
                        Text(
                            valueText(context, change.entry, change.current) + "  →  " + valueText(context, change.entry, change.value),
                            style = MaterialTheme.typography.bodyMedium,
                            color = Capy.Accent,
                        )
                        if (change.reason.isNotBlank()) {
                            Text(change.reason, style = MaterialTheme.typography.bodySmall, color = Capy.TextSecondary)
                        }
                    }
                }
                Text(stringResource(R.string.ai_config_undo_hint), style = MaterialTheme.typography.bodySmall, color = Capy.TextMuted)
            }
        }
        is AiState.Message -> NoticeDialog(
            title = stringResource(R.string.ai_config_title),
            icon = Icons.Outlined.AutoAwesome,
            actions = listOf(NoticeAction(stringResource(R.string.close), onClick = onClose)),
            onBack = onClose,
        ) {
            Text(current.text, style = MaterialTheme.typography.bodyMedium, color = Capy.TextSecondary)
        }
    }
}

/** Tells the player, once, which games had their recommended settings undone after a failed start. */
@Composable
fun AiRevertedDialog(notices: List<AiConfigStore.Notice>, onClose: () -> Unit) {
    NoticeDialog(
        title = stringResource(R.string.ai_config_reverted_title),
        icon = Icons.AutoMirrored.Outlined.Undo,
        actions = listOf(NoticeAction(stringResource(R.string.close), onClick = onClose)),
        onBack = onClose,
    ) {
        notices.forEach { notice ->
            val name = notice.titleName.ifBlank { notice.titleId }
            Text(
                stringResource(
                    if (notice.outcome == AiConfigClient.Outcome.NO_BOOT.wire) R.string.ai_config_reverted_no_boot
                    else R.string.ai_config_reverted_crash,
                    name,
                ),
                style = MaterialTheme.typography.bodyMedium,
                color = Capy.TextSecondary,
            )
        }
    }
}

/** The request: the game, the device and every recommendable value that differs from its default. */
private fun buildRequest(context: Context, settings: Settings, schema: AiSchema, game: GameItem): AiConfigClient.Request {
    val current = schema.settings.mapNotNull { entry ->
        val value = AiConfigStore.effectiveValue(settings, game, entry)
        if (value == entry.default) null else entry.key to schema.wire(entry, value)
    }.toMap()
    return AiConfigClient.Request(
        appVersion = BuildConfig.VERSION_NAME,
        schemaVersion = schema.schemaVersion,
        titleId = game.titleId,
        titleName = game.titleName.take(128),
        gameVersion = game.version.orEmpty().take(32),
        language = Locale.getDefault().language,
        device = AiConfigClient.device(context, settings, game.titleId),
        current = current,
    )
}

private fun check(settings: Settings, schema: AiSchema, game: GameItem, suggestion: AiConfigClient.Suggestion): List<Checked> =
    suggestion.changes.mapNotNull { change ->
        val entry = schema.entry(change.key) ?: return@mapNotNull null
        // A risky setting only when the server marks it experimental (a curated answer has a precedent).
        if (entry.isRisky && !change.experimental) return@mapNotNull null
        val value = schema.normalize(change.key, change.value) ?: return@mapNotNull null
        val now = AiConfigStore.effectiveValue(settings, game, entry)
        if (value == now) return@mapNotNull null
        Checked(entry, value, now, change.reason, change.experimental || entry.isRisky)
    }.distinctBy { it.entry.key }

/**
 * What the feature sends and keeps, in Spanish or English, on CapyPS5's website.
 * (The same server answers Lemon, which has its own page.)
 */
private fun openPrivacy(context: Context) {
    val url = if (Locale.getDefault().language == "es") {
        "https://capyps5.org/es/privacidad-ia.html"
    } else {
        "https://capyps5.org/privacy-ai.html"
    }
    runCatching {
        context.startActivity(Intent(Intent.ACTION_VIEW, Uri.parse(url)).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK))
    }.onFailure { Toast.makeText(context, url, Toast.LENGTH_LONG).show() }
}

/** The setting's name: the core option's title, else ai_setting_<key>, else the schema's title or key. */
@SuppressLint("DiscouragedApi")
private fun title(context: Context, entry: AiSchema.Entry): String {
    entry.coreOption?.let { return it.title(context) }
    val id = context.resources.getIdentifier("ai_setting_${entry.key}", "string", context.packageName)
    if (id != 0) return context.getString(id)
    return entry.uiTitle ?: entry.key
}

/** A value as a player reads it: the option's label, On/Off, an enum's name or the number. */
private fun valueText(context: Context, entry: AiSchema.Entry, value: Long): String {
    entry.coreOption?.let { return it.label(context, value) }
    if (entry.isBool) return context.getString(if (value != 0L) R.string.enabled else R.string.disabled)
    entry.enum.firstOrNull { it.second == value }?.let { return it.first }
    return value.toString()
}

private fun errorText(context: Context, code: String): String = when (code) {
    "rate_limited" -> context.getString(R.string.ai_config_rate_limited)
    "schema_unknown" -> context.getString(R.string.ai_config_update_app)
    "ai_unavailable" -> context.getString(R.string.ai_config_unavailable)
    else -> context.getString(R.string.ai_config_server_error, code)
}
