// SPDX-FileCopyrightText: Copyright 2026 CapyPS5 Project
// SPDX-License-Identifier: GPL-2.0-only

package org.capyps5.emulator.ui.components

import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxHeight
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.itemsIndexed
import androidx.compose.foundation.lazy.rememberLazyListState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateListOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.unit.dp
import kotlinx.coroutines.delay
import kotlinx.coroutines.isActive
import org.capyps5.emulator.NativeBridge
import org.capyps5.emulator.ui.NavKey
import org.capyps5.emulator.ui.OnNavKey
import org.capyps5.emulator.ui.screens.HintBar
import org.capyps5.emulator.ui.theme.Capy
import org.capyps5.emulator.R
import androidx.compose.ui.res.stringResource

/** One pixel shader seen in recent frames (see core/src/common/passInspector.h). */
private data class RenderPass(
    val shader: Long,
    val width: Int,
    val height: Int,
    val draws: Int,
    val targets: Int,
    val skipped: Boolean,
    val seen: Boolean,
)

private fun readPasses(): List<RenderPass> {
    val raw = NativeBridge.nativeGetPasses()
    return (0 until raw.size / 5).map { i ->
        val size = raw[i * 5 + 1]
        RenderPass(
            shader = raw[i * 5],
            width = (size ushr 32).toInt(),
            height = (size and 0xffffffffL).toInt(),
            draws = raw[i * 5 + 2].toInt(),
            targets = raw[i * 5 + 3].toInt(),
            skipped = raw[i * 5 + 4] != 0L,
            seen = raw[i * 5 + 2] != 0L,
        )
    }
}

/**
 * Debug panel listing the render passes (pixel shaders) of the last frame. Each one can be
 * switched off live to find which pass produces a visual artifact. The game keeps rendering
 * behind the panel, so the effect is visible immediately.
 */
@Composable
fun LayersPanel(onClose: () -> Unit, modifier: Modifier = Modifier) {
    // Stable order: passes keep their first-seen position so the focus does not jump around.
    val passes = remember { mutableStateListOf<RenderPass>() }
    var focused by remember { mutableIntStateOf(0) }
    val listState = rememberLazyListState()

    fun refresh() {
        val latest = readPasses().associateBy { it.shader }
        for (i in passes.indices) {
            val known = passes[i]
            passes[i] = latest[known.shader] ?: known.copy(draws = 0, seen = false)
        }
        latest.values.filter { p -> passes.none { it.shader == p.shader } }.forEach { passes.add(it) }
    }

    fun toggle(index: Int) {
        val pass = passes.getOrNull(index) ?: return
        NativeBridge.nativeSetPassSkipped(pass.shader, !pass.skipped)
        passes[index] = pass.copy(skipped = !pass.skipped)
    }

    LaunchedEffect(Unit) {
        while (isActive) {
            refresh()
            delay(500)
        }
    }
    LaunchedEffect(focused) {
        if (passes.isNotEmpty()) listState.animateScrollToItem((focused - 3).coerceAtLeast(0))
    }

    OnNavKey { key ->
        when (key) {
            NavKey.Up -> if (passes.isNotEmpty()) focused = (focused - 1).mod(passes.size)
            NavKey.Down -> if (passes.isNotEmpty()) focused = (focused + 1).mod(passes.size)
            NavKey.Confirm, NavKey.Left, NavKey.Right -> toggle(focused)
            NavKey.Secondary -> {
                NativeBridge.nativeClearSkippedPasses()
                for (i in passes.indices) passes[i] = passes[i].copy(skipped = false)
            }
            NavKey.Back, NavKey.Menu -> onClose()
            else -> Unit
        }
    }

    Column(
        modifier = modifier
            .fillMaxHeight()
            .width(330.dp)
            .background(Capy.Surface.copy(alpha = 0.92f))
            .padding(16.dp),
        verticalArrangement = Arrangement.spacedBy(10.dp),
    ) {
        Row(verticalAlignment = Alignment.CenterVertically) {
            Text(stringResource(R.string.layers), style = MaterialTheme.typography.titleLarge, modifier = Modifier.weight(1f))
            Text(
                stringResource(R.string.close_b),
                style = MaterialTheme.typography.labelLarge,
                color = Capy.TextSecondary,
                modifier = Modifier.clickable(onClick = onClose).padding(6.dp),
            )
        }
        Text(
            stringResource(R.string.layers_desc),
            style = MaterialTheme.typography.bodySmall,
            color = Capy.TextSecondary,
        )
        LazyColumn(
            state = listState,
            modifier = Modifier.weight(1f),
            verticalArrangement = Arrangement.spacedBy(6.dp),
        ) {
            itemsIndexed(passes, key = { _, p -> p.shader }) { i, pass ->
                PassRow(i, pass, i == focused) {
                    focused = i
                    toggle(i)
                }
            }
        }
        HintBar(
            listOf(
                "Ⓐ" to stringResource(R.string.hint_toggle),
                "Ⓨ" to stringResource(R.string.hint_enable_all),
                "Ⓑ" to stringResource(R.string.close),
            ),
        )
    }
}

@Composable
private fun PassRow(index: Int, pass: RenderPass, focused: Boolean, onClick: () -> Unit) {
    Row(
        modifier = Modifier
            .fillMaxWidth()
            .background(if (focused) Capy.SurfaceHigh else Color.Transparent, RoundedCornerShape(10.dp))
            .border(1.dp, if (focused) Capy.Accent.copy(alpha = 0.7f) else Color.Transparent, RoundedCornerShape(10.dp))
            .clickable(onClick = onClick)
            .padding(horizontal = 12.dp, vertical = 8.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Column(Modifier.weight(1f)) {
            Text(
                stringResource(R.string.layer_pass, index + 1) +
                    if (pass.width > 0) "  ·  ${pass.width}×${pass.height}" else "",
                style = MaterialTheme.typography.titleSmall,
                color = if (pass.seen) Capy.TextPrimary else Capy.TextMuted,
            )
            val drawsText = if (pass.seen) stringResource(R.string.layer_draws, pass.draws)
            else stringResource(R.string.layer_not_drawn)
            val targetsText = stringResource(R.string.layer_targets, pass.targets)
            Text(
                buildString {
                    append(drawsText)
                    if (pass.targets > 1) append(" · $targetsText")
                    append(" · ps 0x%x".format(pass.shader))
                },
                style = MaterialTheme.typography.bodySmall.copy(fontFamily = FontFamily.Monospace),
                color = Capy.TextSecondary,
            )
        }
        Text(
            if (pass.skipped) "OFF" else "ON",
            style = MaterialTheme.typography.labelLarge,
            color = Color.White,
            modifier = Modifier
                .background(if (pass.skipped) Capy.Bad else Capy.Good.copy(alpha = 0.55f), RoundedCornerShape(50))
                .padding(horizontal = 12.dp, vertical = 4.dp),
        )
    }
}
