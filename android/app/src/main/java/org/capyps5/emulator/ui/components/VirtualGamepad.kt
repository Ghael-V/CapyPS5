// SPDX-FileCopyrightText: Copyright 2026 CapyPS5 Project
// SPDX-License-Identifier: GPL-2.0-only

package org.capyps5.emulator.ui.components

import androidx.compose.foundation.Canvas
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberUpdatedState
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.alpha
import androidx.compose.ui.draw.clip
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.input.pointer.PointerEventType
import androidx.compose.ui.input.pointer.PointerId
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch
import org.capyps5.emulator.NativeBridge
import kotlin.math.hypot

/** On-screen DualSense: shoulders, d-pad, both sticks, face buttons, Create/Options and touchpad. */
@Composable
fun VirtualGamepad(
    modifier: Modifier = Modifier,
    alpha: Float = 0.65f,
    showTouchpad: Boolean = true,
    onMenuClick: () -> Unit = {},
) {
    var buttons by remember { mutableIntStateOf(0) }
    val axes = remember { FloatArray(6) } // lx, ly, rx, ry, l2, r2

    fun send() = NativeBridge.nativeSendControllerInput(buttons, axes[0], axes[1], axes[2], axes[3], axes[4], axes[5])
    fun button(mask: Int, pressed: Boolean) {
        buttons = if (pressed) buttons or mask else buttons and mask.inv()
        send()
    }

    Box(modifier.fillMaxSize().alpha(alpha).padding(16.dp)) {
        // Left: L2/L1, d-pad and left stick.
        Column(
            Modifier.align(Alignment.BottomStart).padding(start = 8.dp, bottom = 8.dp),
            verticalArrangement = Arrangement.spacedBy(10.dp),
        ) {
            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                GamepadButton("L2", 56.dp, 36.dp) { axes[4] = if (it) 1f else 0f; button(NativeBridge.BTN_L2, it) }
                GamepadButton("L1", 56.dp, 36.dp) { button(NativeBridge.BTN_L1, it) }
            }
            Row(horizontalArrangement = Arrangement.spacedBy(14.dp), verticalAlignment = Alignment.Bottom) {
                DPad(onButton = ::button)
                AnalogStick { x, y -> axes[0] = x; axes[1] = y; send() }
            }
        }

        // Right: R1/R2, right stick and face buttons.
        Column(
            Modifier.align(Alignment.BottomEnd).padding(end = 8.dp, bottom = 8.dp),
            verticalArrangement = Arrangement.spacedBy(10.dp),
            horizontalAlignment = Alignment.End,
        ) {
            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                GamepadButton("R1", 56.dp, 36.dp) { button(NativeBridge.BTN_R1, it) }
                GamepadButton("R2", 56.dp, 36.dp) { axes[5] = if (it) 1f else 0f; button(NativeBridge.BTN_R2, it) }
            }
            Row(horizontalArrangement = Arrangement.spacedBy(14.dp), verticalAlignment = Alignment.Bottom) {
                AnalogStick { x, y -> axes[2] = x; axes[3] = y; send() }
                FaceButtons(onButton = ::button)
            }
        }

        // Top centre: Create, touchpad, Options and the emulator menu.
        Row(
            Modifier.align(Alignment.TopCenter),
            horizontalArrangement = Arrangement.spacedBy(12.dp),
            verticalAlignment = Alignment.Top,
        ) {
            GamepadButton("CREATE", 70.dp, 30.dp, textSize = 10) { button(NativeBridge.BTN_SELECT, it) }
            if (showTouchpad) Touchpad(Modifier.size(width = 220.dp, height = 96.dp))
            GamepadButton("OPTIONS", 70.dp, 30.dp, textSize = 10) { button(NativeBridge.BTN_START, it) }
            GamepadButton("☰", 36.dp, 30.dp, textSize = 14) { if (it) onMenuClick() }
        }
    }
}

/** Touchpad on its own, for devices with a physical controller but no touchpad. */
@Composable
fun TouchpadOverlay(modifier: Modifier = Modifier, alpha: Float = 0.65f) {
    Box(modifier.fillMaxSize().alpha(alpha).padding(top = 16.dp)) {
        Touchpad(Modifier.align(Alignment.TopCenter).size(width = 240.dp, height = 104.dp))
    }
}

/**
 * DualSense touchpad: up to two fingers are reported to the game with coordinates relative to the
 * pad; a short tap without movement also clicks it.
 */
@Composable
fun Touchpad(modifier: Modifier = Modifier) {
    var touching by remember { mutableStateOf(false) }
    var clicked by remember { mutableStateOf(false) }
    val scope = rememberCoroutineScope()
    val shape = RoundedCornerShape(16.dp)
    Box(
        modifier
            .clip(shape)
            .background(if (touching) Color(0x664466AA) else Color(0x44222A3A))
            .border(1.dp, if (clicked) Color(0xFF00E5FF) else Color(0x44FFFFFF), shape)
            .pointerInput(Unit) {
                awaitPointerEventScope {
                    // Pointer id -> (finger slot, down time, start position).
                    val fingers = HashMap<Long, Triple<Int, Long, Offset>>()
                    var moved = false
                    while (true) {
                        val event = awaitPointerEvent()
                        for (change in event.changes) {
                            val id = change.id.value
                            val x = (change.position.x / size.width).coerceIn(0f, 1f)
                            val y = (change.position.y / size.height).coerceIn(0f, 1f)
                            if (change.pressed && id !in fingers) {
                                val slot = (0..1).firstOrNull { s -> fingers.values.none { it.first == s } } ?: continue
                                fingers[id] = Triple(slot, change.uptimeMillis, change.position)
                                if (fingers.size == 1) moved = false
                                NativeBridge.nativeSendTouch(slot, true, x, y)
                            } else if (change.pressed) {
                                val finger = fingers[id] ?: continue
                                if ((change.position - finger.third).getDistance() > 12f) moved = true
                                NativeBridge.nativeSendTouch(finger.first, true, x, y)
                            } else {
                                val finger = fingers.remove(id) ?: continue
                                NativeBridge.nativeSendTouch(finger.first, false, x, y)
                                // A short tap without movement is a click of the pad.
                                if (!moved && fingers.isEmpty() && change.uptimeMillis - finger.second < 250) {
                                    clicked = true
                                    NativeBridge.nativeSetTouchpadClick(true)
                                    scope.launch {
                                        delay(90)
                                        NativeBridge.nativeSetTouchpadClick(false)
                                        clicked = false
                                    }
                                }
                            }
                            change.consume()
                        }
                        touching = fingers.isNotEmpty()
                        if (event.type == PointerEventType.Exit && fingers.isEmpty()) touching = false
                    }
                }
            },
        contentAlignment = Alignment.Center,
    ) {
        Text("TOUCHPAD", color = Color(0x88FFFFFF), fontSize = 10.sp, fontWeight = FontWeight.Bold)
    }
}

/** Analog stick: the knob follows the finger inside the base; release recentres it. */
@Composable
fun AnalogStick(size: Dp = 116.dp, onMove: (Float, Float) -> Unit) {
    var knob by remember { mutableStateOf(Offset.Zero) }
    Box(
        Modifier
            .size(size)
            .pointerInput(Unit) {
                val radius = this.size.width / 2f
                val centre = Offset(radius, radius)
                var finger: PointerId? = null
                awaitPointerEventScope {
                    while (true) {
                        val event = awaitPointerEvent()
                        // Follow the finger that grabbed the stick; a second one landing on it
                        // takes over only after the first lifts.
                        val change = event.changes.firstOrNull { it.id == finger }
                            ?: event.changes.firstOrNull { it.pressed }
                            ?: event.changes.firstOrNull()
                            ?: continue
                        finger = if (change.pressed) change.id else null
                        if (change.pressed) {
                            var delta = change.position - centre
                            val length = hypot(delta.x, delta.y)
                            if (length > radius) delta *= radius / length
                            knob = delta
                            onMove(delta.x / radius, delta.y / radius)
                        } else {
                            knob = Offset.Zero
                            onMove(0f, 0f)
                        }
                        event.changes.forEach { it.consume() }
                    }
                }
            },
    ) {
        Canvas(Modifier.fillMaxSize()) {
            val radius = this.size.minDimension / 2f
            drawCircle(Color(0x44222A3A), radius)
            drawCircle(Color(0x44FFFFFF), radius, style = Stroke(width = 2f))
            drawCircle(Color(0x99AABBDD), radius * 0.42f, center = center + knob)
        }
    }
}

@Composable
private fun DPad(onButton: (Int, Boolean) -> Unit) {
    Box(Modifier.size(124.dp), contentAlignment = Alignment.Center) {
        Box(Modifier.align(Alignment.TopCenter)) { GamepadButton("▲", 42.dp, 42.dp) { onButton(NativeBridge.BTN_UP, it) } }
        Box(Modifier.align(Alignment.BottomCenter)) { GamepadButton("▼", 42.dp, 42.dp) { onButton(NativeBridge.BTN_DOWN, it) } }
        Box(Modifier.align(Alignment.CenterStart)) { GamepadButton("◀", 42.dp, 42.dp) { onButton(NativeBridge.BTN_LEFT, it) } }
        Box(Modifier.align(Alignment.CenterEnd)) { GamepadButton("▶", 42.dp, 42.dp) { onButton(NativeBridge.BTN_RIGHT, it) } }
    }
}

@Composable
private fun FaceButtons(onButton: (Int, Boolean) -> Unit) {
    Box(Modifier.size(124.dp), contentAlignment = Alignment.Center) {
        Box(Modifier.align(Alignment.TopCenter)) {
            GamepadButton("△", 44.dp, 44.dp, color = Color(0xFF00FF7F)) { onButton(NativeBridge.BTN_TRIANGLE, it) }
        }
        Box(Modifier.align(Alignment.BottomCenter)) {
            GamepadButton("✕", 44.dp, 44.dp, color = Color(0xFF4169E1)) { onButton(NativeBridge.BTN_CROSS, it) }
        }
        Box(Modifier.align(Alignment.CenterStart)) {
            GamepadButton("□", 44.dp, 44.dp, color = Color(0xFFFF69B4)) { onButton(NativeBridge.BTN_SQUARE, it) }
        }
        Box(Modifier.align(Alignment.CenterEnd)) {
            GamepadButton("○", 44.dp, 44.dp, color = Color(0xFFFF4500)) { onButton(NativeBridge.BTN_CIRCLE, it) }
        }
    }
}

/**
 * Pressed while any finger that went down on it stays down. Compose hands each pointer to the
 * node it landed on, so several buttons (and the sticks) can be held at the same time.
 */
@Composable
fun GamepadButton(
    label: String,
    width: Dp,
    height: Dp,
    color: Color = Color.White,
    textSize: Int = 16,
    onPressChanged: (Boolean) -> Unit,
) {
    var isPressed by remember { mutableStateOf(false) }
    val onChange by rememberUpdatedState(onPressChanged)
    DisposableEffect(Unit) {
        onDispose { if (isPressed) onChange(false) }
    }

    Box(
        modifier = Modifier
            .size(width = width, height = height)
            .clip(CircleShape)
            .background(if (isPressed) Color(0x994466AA) else Color(0x66222A3A))
            .border(1.dp, if (isPressed) Color(0xFF00E5FF) else Color(0x44FFFFFF), CircleShape)
            .pointerInput(Unit) {
                awaitPointerEventScope {
                    while (true) {
                        val event = awaitPointerEvent()
                        val down = event.changes.any { it.pressed }
                        if (down != isPressed) {
                            isPressed = down
                            onChange(down)
                        }
                        event.changes.forEach { it.consume() }
                    }
                }
            },
        contentAlignment = Alignment.Center,
    ) {
        Text(
            text = label,
            color = if (isPressed) Color(0xFF00E5FF) else color,
            fontWeight = FontWeight.Bold,
            fontSize = textSize.sp,
        )
    }
}
