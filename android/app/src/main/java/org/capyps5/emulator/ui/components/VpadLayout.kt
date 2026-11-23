// SPDX-FileCopyrightText: Copyright 2026 CapyPS5 Project
// SPDX-License-Identifier: GPL-2.0-only

package org.capyps5.emulator.ui.components

import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.gestures.detectDragGestures
import androidx.compose.foundation.gestures.detectTapGestures
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.widthIn
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.ui.Alignment
import androidx.compose.ui.res.stringResource
import org.capyps5.emulator.R
import org.capyps5.emulator.ui.NavKey
import org.capyps5.emulator.ui.OnNavKey
import org.capyps5.emulator.ui.theme.Capy
import androidx.compose.foundation.layout.offset
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.graphicsLayer
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.unit.dp
import java.util.Locale

/** The parts of the on-screen pad that can be moved and resized on their own. */
enum class VpadElement(val label: String) {
    L2("L2"), L1("L1"), DPad("D-pad"), LeftStick("L3"),
    R1("R1"), R2("R2"), RightStick("R3"), FaceButtons("△ ○ ✕ □"),
    Create("CREATE"), Touchpad("TOUCHPAD"), Options("OPTIONS"), Menu("☰"),
}

/** Where an element sits relative to its default place (dp) and how large it is. */
data class VpadPlacement(val dx: Float = 0f, val dy: Float = 0f, val scale: Float = 1f) {
    val isDefault: Boolean get() = dx == 0f && dy == 0f && scale == 1f
}

/** A pad layout: the placement of every element that left its default place. */
class VpadLayout(private val placements: Map<VpadElement, VpadPlacement> = emptyMap()) {
    operator fun get(element: VpadElement): VpadPlacement = placements[element] ?: VpadPlacement()

    fun with(element: VpadElement, placement: VpadPlacement): VpadLayout =
        VpadLayout(if (placement.isDefault) placements - element else placements + (element to placement))

    val isDefault: Boolean get() = placements.isEmpty()

    /** "Element:dx,dy,scale;..." with only the moved elements. */
    fun encode(): String = placements.entries.joinToString(";") { (element, p) ->
        String.format(Locale.ROOT, "%s:%.1f,%.1f,%.2f", element.name, p.dx, p.dy, p.scale)
    }

    companion object {
        const val MinScale = 0.5f
        const val MaxScale = 2.5f

        /** Unknown elements or malformed entries are skipped (an older or newer app wrote them). */
        fun decode(text: String?): VpadLayout {
            if (text.isNullOrBlank()) return VpadLayout()
            val placements = text.split(';').mapNotNull { entry ->
                val (name, values) = entry.split(':', limit = 2).takeIf { it.size == 2 } ?: return@mapNotNull null
                val element = VpadElement.entries.firstOrNull { it.name == name } ?: return@mapNotNull null
                val numbers = values.split(',').mapNotNull { it.toFloatOrNull() }
                if (numbers.size != 3) return@mapNotNull null
                element to VpadPlacement(numbers[0], numbers[1], numbers[2].coerceIn(MinScale, MaxScale))
            }.toMap()
            return VpadLayout(placements)
        }
    }
}

/** Edit mode of the pad: which element is selected and how a drag moves one. */
class VpadEditor(
    val selected: VpadElement?,
    val onSelect: (VpadElement) -> Unit,
    val onMove: (VpadElement, Float, Float) -> Unit,
)

/**
 * One element of the pad at its placement. While editing, a layer over it takes every touch:
 * a tap selects it and a drag moves it, and nothing reaches the game.
 */
@Composable
fun PlacedElement(
    element: VpadElement,
    layout: VpadLayout,
    editor: VpadEditor?,
    modifier: Modifier = Modifier,
    content: @Composable () -> Unit,
) {
    val placement = layout[element]
    Box(
        modifier
            .offset(placement.dx.dp, placement.dy.dp)
            .graphicsLayer {
                scaleX = placement.scale
                scaleY = placement.scale
            },
    ) {
        content()
        if (editor != null) {
            val shape = RoundedCornerShape(10.dp)
            Box(
                Modifier
                    .matchParentSize()
                    .border(
                        if (editor.selected == element) 2.dp else 1.dp,
                        if (editor.selected == element) Color(0xFF00E5FF) else Color(0x99FFFFFF),
                        shape,
                    )
                    .pointerInput(element) { detectTapGestures { editor.onSelect(element) } }
                    .pointerInput(element) {
                        detectDragGestures(onDragStart = { editor.onSelect(element) }) { change, amount ->
                            change.consume()
                            // Touch positions here are in the scaled element's units.
                            editor.onMove(
                                element,
                                amount.x * placement.scale / density,
                                amount.y * placement.scale / density,
                            )
                        }
                    },
            )
        }
    }
}

/**
 * Tools of the pad editor: size of the selected element, resets and where to save the layout.
 * Back (B or the system back) leaves without saving.
 */
@Composable
fun VpadEditBar(
    selected: VpadElement?,
    onScale: (Float) -> Unit,
    onResetElement: () -> Unit,
    onResetAll: () -> Unit,
    onSaveGlobal: () -> Unit,
    onSaveGame: () -> Unit,
    onCancel: () -> Unit,
    modifier: Modifier = Modifier,
) {
    OnNavKey { key -> if (key == NavKey.Back || key == NavKey.Secondary || key == NavKey.Menu) onCancel() }
    Column(
        modifier
            .widthIn(max = 560.dp)
            .background(Capy.Surface.copy(alpha = 0.92f), RoundedCornerShape(14.dp))
            .padding(14.dp),
        verticalArrangement = Arrangement.spacedBy(10.dp),
        horizontalAlignment = Alignment.CenterHorizontally,
    ) {
        Text(
            selected?.label ?: stringResource(R.string.vpad_hint),
            style = MaterialTheme.typography.titleMedium,
            color = Capy.TextPrimary,
        )
        if (selected != null) {
            Row(horizontalArrangement = Arrangement.spacedBy(8.dp), verticalAlignment = Alignment.CenterVertically) {
                BarButton("−") { onScale(-0.1f) }
                BarButton("+") { onScale(0.1f) }
                BarButton(stringResource(R.string.vpad_reset_item), onClick = onResetElement)
            }
        }
        Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            BarButton(stringResource(R.string.vpad_reset_all), onClick = onResetAll)
            BarButton(stringResource(R.string.cancel), onClick = onCancel)
        }
        Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            BarButton(stringResource(R.string.vpad_save_global), primary = true, onClick = onSaveGlobal)
            BarButton(stringResource(R.string.vpad_save_game), primary = true, onClick = onSaveGame)
        }
    }
}

@Composable
private fun BarButton(label: String, primary: Boolean = false, onClick: () -> Unit) {
    Box(
        Modifier
            .background(if (primary) Capy.Accent else Capy.SurfaceHigh, RoundedCornerShape(10.dp))
            .clickable(onClick = onClick)
            .padding(horizontal = 14.dp, vertical = 9.dp),
        contentAlignment = Alignment.Center,
    ) {
        Text(label, style = MaterialTheme.typography.labelLarge, color = if (primary) Color.White else Capy.TextPrimary)
    }
}
