package org.capyps5.emulator.ui.components

import android.view.MotionEvent
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Text
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.ExperimentalComposeUiApi
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.alpha
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.input.pointer.pointerInteropFilter
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import org.capyps5.emulator.NativeBridge

@Composable
fun VirtualGamepad(
    modifier: Modifier = Modifier,
    alpha: Float = 0.65f,
    onMenuClick: () -> Unit = {}
) {
    var buttonsState by remember { mutableStateOf(0) }
    var leftStickX by remember { mutableStateOf(0f) }
    var leftStickY by remember { mutableStateOf(0f) }
    var rightStickX by remember { mutableStateOf(0f) }
    var rightStickY by remember { mutableStateOf(0f) }
    var l2Pressure by remember { mutableStateOf(0f) }
    var r2Pressure by remember { mutableStateOf(0f) }

    fun updateButton(mask: Int, pressed: Boolean) {
        buttonsState = if (pressed) {
            buttonsState or mask
        } else {
            buttonsState and mask.inv()
        }
        NativeBridge.nativeSendControllerInput(
            buttonsState,
            leftStickX,
            leftStickY,
            rightStickX,
            rightStickY,
            l2Pressure,
            r2Pressure
        )
    }

    Box(
        modifier = modifier
            .fillMaxSize()
            .alpha(alpha)
            .padding(16.dp)
    ) {
        // Left Side: L1, L2 and D-Pad
        Column(
            modifier = Modifier
                .align(Alignment.BottomStart)
                .padding(bottom = 12.dp, start = 8.dp),
            verticalArrangement = Arrangement.spacedBy(12.dp)
        ) {
            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                GamepadButton("L2", 52.dp, 36.dp) { pressed ->
                    l2Pressure = if (pressed) 1.0f else 0.0f
                    updateButton(NativeBridge.BTN_L2, pressed)
                }
                GamepadButton("L1", 52.dp, 36.dp) { pressed ->
                    updateButton(NativeBridge.BTN_L1, pressed)
                }
            }

            // D-Pad
            Box(
                modifier = Modifier
                    .size(130.dp),
                contentAlignment = Alignment.Center
            ) {
                // Up
                Box(modifier = Modifier.align(Alignment.TopCenter)) {
                    GamepadButton("▲", 42.dp, 42.dp) { pressed ->
                        updateButton(NativeBridge.BTN_UP, pressed)
                    }
                }
                // Down
                Box(modifier = Modifier.align(Alignment.BottomCenter)) {
                    GamepadButton("▼", 42.dp, 42.dp) { pressed ->
                        updateButton(NativeBridge.BTN_DOWN, pressed)
                    }
                }
                // Left
                Box(modifier = Modifier.align(Alignment.CenterStart)) {
                    GamepadButton("◀", 42.dp, 42.dp) { pressed ->
                        updateButton(NativeBridge.BTN_LEFT, pressed)
                    }
                }
                // Right
                Box(modifier = Modifier.align(Alignment.CenterEnd)) {
                    GamepadButton("▶", 42.dp, 42.dp) { pressed ->
                        updateButton(NativeBridge.BTN_RIGHT, pressed)
                    }
                }
            }
        }

        // Center: Menu / Touchpad / Options
        Row(
            modifier = Modifier
                .align(Alignment.TopCenter)
                .padding(top = 8.dp),
            horizontalArrangement = Arrangement.spacedBy(16.dp)
        ) {
            GamepadButton("CREATE", 68.dp, 30.dp, textSize = 10) { pressed ->
                updateButton(NativeBridge.BTN_SELECT, pressed)
            }
            GamepadButton("TOUCHPAD", 84.dp, 30.dp, textSize = 10) { pressed ->
                updateButton(NativeBridge.BTN_TOUCHPAD, pressed)
            }
            GamepadButton("MENU", 64.dp, 30.dp, textSize = 10) { pressed ->
                if (pressed) onMenuClick()
            }
            GamepadButton("OPTIONS", 68.dp, 30.dp, textSize = 10) { pressed ->
                updateButton(NativeBridge.BTN_START, pressed)
            }
        }

        // Right Side: R1, R2 and Action Buttons (Square, Triangle, Cross, Circle)
        Column(
            modifier = Modifier
                .align(Alignment.BottomEnd)
                .padding(bottom = 12.dp, end = 8.dp),
            verticalArrangement = Arrangement.spacedBy(12.dp),
            horizontalAlignment = Alignment.End
        ) {
            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                GamepadButton("R1", 52.dp, 36.dp) { pressed ->
                    updateButton(NativeBridge.BTN_R1, pressed)
                }
                GamepadButton("R2", 52.dp, 36.dp) { pressed ->
                    r2Pressure = if (pressed) 1.0f else 0.0f
                    updateButton(NativeBridge.BTN_R2, pressed)
                }
            }

            // Action Buttons Cluster
            Box(
                modifier = Modifier
                    .size(130.dp),
                contentAlignment = Alignment.Center
            ) {
                // Triangle (Top)
                Box(modifier = Modifier.align(Alignment.TopCenter)) {
                    GamepadButton("△", 42.dp, 42.dp, color = Color(0xFF00FF7F)) { pressed ->
                        updateButton(NativeBridge.BTN_TRIANGLE, pressed)
                    }
                }
                // Cross (Bottom)
                Box(modifier = Modifier.align(Alignment.BottomCenter)) {
                    GamepadButton("✕", 42.dp, 42.dp, color = Color(0xFF4169E1)) { pressed ->
                        updateButton(NativeBridge.BTN_CROSS, pressed)
                    }
                }
                // Square (Left)
                Box(modifier = Modifier.align(Alignment.CenterStart)) {
                    GamepadButton("□", 42.dp, 42.dp, color = Color(0xFFFF69B4)) { pressed ->
                        updateButton(NativeBridge.BTN_SQUARE, pressed)
                    }
                }
                // Circle (Right)
                Box(modifier = Modifier.align(Alignment.CenterEnd)) {
                    GamepadButton("○", 42.dp, 42.dp, color = Color(0xFFFF4500)) { pressed ->
                        updateButton(NativeBridge.BTN_CIRCLE, pressed)
                    }
                }
            }
        }
    }
}

@OptIn(ExperimentalComposeUiApi::class)
@Composable
fun GamepadButton(
    label: String,
    width: androidx.compose.ui.unit.Dp,
    height: androidx.compose.ui.unit.Dp,
    color: Color = Color.White,
    textSize: Int = 16,
    onPressChanged: (Boolean) -> Unit
) {
    var isPressed by remember { mutableStateOf(false) }

    Box(
        modifier = Modifier
            .size(width = width, height = height)
            .clip(CircleShape)
            .background(if (isPressed) Color(0x994466AA) else Color(0x66222A3A))
            .border(1.dp, if (isPressed) Color(0xFF00E5FF) else Color(0x44FFFFFF), CircleShape)
            .pointerInteropFilter { event ->
                when (event.action) {
                    MotionEvent.ACTION_DOWN -> {
                        isPressed = true
                        onPressChanged(true)
                        true
                    }
                    MotionEvent.ACTION_MOVE -> {
                        true
                    }
                    MotionEvent.ACTION_UP, MotionEvent.ACTION_CANCEL -> {
                        isPressed = false
                        onPressChanged(false)
                        true
                    }
                    else -> false
                }
            },
        contentAlignment = Alignment.Center
    ) {
        Text(
            text = label,
            color = if (isPressed) Color(0xFF00E5FF) else color,
            fontWeight = FontWeight.Bold,
            fontSize = textSize.sp
        )
    }
}
