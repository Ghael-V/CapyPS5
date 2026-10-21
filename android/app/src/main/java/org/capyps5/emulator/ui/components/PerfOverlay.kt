// SPDX-FileCopyrightText: Copyright 2026 CapyPS5 Project
// SPDX-License-Identifier: GPL-2.0-only

package org.capyps5.emulator.ui.components

import androidx.compose.foundation.Canvas
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.Path
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.text.SpanStyle
import androidx.compose.ui.text.buildAnnotatedString
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.withStyle
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import org.capyps5.emulator.OverlayMode
import org.capyps5.emulator.telemetry.PerfSnapshot
import org.capyps5.emulator.ui.theme.Capy

private val OverlayBg = Color(0xB3000000)
private val Label = Color(0xFF8A93A6)

private fun fpsColor(fps: Float) = when {
    fps >= 55f -> Capy.Good
    fps >= 28f -> Capy.Warn
    else -> Capy.Bad
}

private fun tempColor(c: Float?) = when {
    c == null -> Capy.TextPrimary
    c >= 85f -> Capy.Bad
    c >= 70f -> Capy.Warn
    else -> Capy.TextPrimary
}

@Composable
fun PerfOverlay(mode: OverlayMode, snapshot: PerfSnapshot, fpsHistory: List<Float>, modifier: Modifier = Modifier) {
    if (mode == OverlayMode.Off) return
    val mono = FontFamily.Monospace
    Column(
        modifier = modifier
            .background(OverlayBg, RoundedCornerShape(8.dp))
            .padding(horizontal = 10.dp, vertical = 6.dp),
        verticalArrangement = Arrangement.spacedBy(3.dp),
    ) {
        Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(12.dp)) {
            Text(
                buildAnnotatedString {
                    withStyle(SpanStyle(color = fpsColor(snapshot.fps), fontWeight = FontWeight.Bold)) {
                        append("%.0f".format(snapshot.fps))
                    }
                    withStyle(SpanStyle(color = Label)) { append(" FPS  ") }
                    append("%.1f".format(snapshot.frameTimeMs))
                    withStyle(SpanStyle(color = Label)) { append(" ms") }
                    if (mode == OverlayMode.Full) {
                        withStyle(SpanStyle(color = Label)) { append("  max ") }
                        withStyle(SpanStyle(color = if (snapshot.maxFrameTimeMs > 50f) Capy.Warn else Capy.TextPrimary)) {
                            append("%.1f".format(snapshot.maxFrameTimeMs))
                        }
                    }
                },
                color = Capy.TextPrimary, fontFamily = mono, fontSize = 12.sp,
            )
            if (mode == OverlayMode.Full) {
                FpsGraph(fpsHistory, Modifier.width(96.dp).height(18.dp))
            }
        }
        if (mode == OverlayMode.Full) {
            Text(
                buildAnnotatedString {
                    metric("CPU", snapshot.cpuPercent?.let { "%.0f%%".format(it) })
                    if (snapshot.cpuClusterMhz.isNotEmpty()) {
                        append(" ")
                        append(snapshot.cpuClusterMhz.joinToString("/") { "%.1f".format(it / 1000f) })
                        withStyle(SpanStyle(color = Label)) { append("GHz") }
                    }
                    snapshot.cpuTempC?.let { temp(it) }
                    sep()
                    metric("GPU", snapshot.gpuPercent?.let { "%.0f%%".format(it) })
                    snapshot.gpuMhz?.let {
                        append(" $it")
                        withStyle(SpanStyle(color = Label)) { append("MHz") }
                    }
                    snapshot.gpuTempC?.let { temp(it) }
                    sep()
                    metric("RAM", "%.1f".format(snapshot.appRamMb / 1024f))
                    withStyle(SpanStyle(color = Label)) {
                        append(" / %.1f/%.1fGB".format(snapshot.deviceRamUsedMb / 1024f, snapshot.deviceRamTotalMb / 1024f))
                    }
                    if (snapshot.powerWatts != null || snapshot.batteryTempC != null) {
                        sep()
                        metric("BAT", snapshot.powerWatts?.let { "%.1fW".format(it) })
                        snapshot.batteryTempC?.let { temp(it) }
                    }
                },
                color = Capy.TextPrimary, fontFamily = mono, fontSize = 11.sp,
            )
            val issues = snapshot.syncTimeoutsPerSec + snapshot.unmappedWaitsPerSec + snapshot.nullBuffersPerSec
            if (issues > 0f) {
                Text(
                    "HACKS/s  sync-timeout %.1f  unmapped-wait %.1f  null-buffer %.1f".format(
                        snapshot.syncTimeoutsPerSec, snapshot.unmappedWaitsPerSec, snapshot.nullBuffersPerSec
                    ),
                    color = Capy.Warn, fontFamily = mono, fontSize = 10.sp,
                )
            }
        }
    }
}

private fun androidx.compose.ui.text.AnnotatedString.Builder.metric(label: String, value: String?) {
    withStyle(SpanStyle(color = Label)) { append("$label ") }
    append(value ?: "–")
}

private fun androidx.compose.ui.text.AnnotatedString.Builder.temp(c: Float) {
    append(" ")
    withStyle(SpanStyle(color = tempColor(c))) { append("%.0f°".format(c)) }
}

private fun androidx.compose.ui.text.AnnotatedString.Builder.sep() {
    withStyle(SpanStyle(color = Label)) { append("  │  ") }
}

@Composable
private fun FpsGraph(history: List<Float>, modifier: Modifier) {
    Canvas(modifier) {
        val target = 60f
        drawLine(Color(0x33FFFFFF), Offset(0f, 0f), Offset(size.width, 0f), 1f)
        if (history.size < 2) return@Canvas
        val step = size.width / (history.size - 1)
        val path = Path()
        history.forEachIndexed { i, v ->
            val y = size.height * (1f - (v / target).coerceIn(0f, 1f))
            if (i == 0) path.moveTo(0f, y) else path.lineTo(i * step, y)
        }
        drawPath(path, fpsColor(history.last()), style = Stroke(width = 1.5.dp.toPx()))
    }
}
