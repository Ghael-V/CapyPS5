// SPDX-FileCopyrightText: Copyright 2026 CapyPS5 Project
// SPDX-License-Identifier: GPL-2.0-only

package org.capyps5.emulator

import android.os.Bundle
import android.view.InputDevice
import android.view.KeyEvent
import android.view.MotionEvent
import androidx.activity.ComponentActivity
import androidx.core.view.WindowCompat
import androidx.core.view.WindowInsetsCompat
import androidx.core.view.WindowInsetsControllerCompat
import org.capyps5.emulator.ui.NavKey
import org.capyps5.emulator.ui.UiInput

/** Decides whether physical controller input goes to the frontend UI or to the emulated pad. */
object InputRouter {
    @Volatile
    var routeToUi = true
        private set
    var onPauseRequested: (() -> Unit)? = null
    var onRouteChanged: ((Boolean) -> Unit)? = null

    fun setRouteToUi(value: Boolean) {
        if (routeToUi == value) return
        routeToUi = value
        onRouteChanged?.invoke(value)
    }
}

/** Full-screen activity whose physical controller drives the UI or, in game, the emulated pad. */
abstract class CapyActivity : ComponentActivity() {

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        window.addFlags(android.view.WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        WindowCompat.setDecorFitsSystemWindows(window, false)
        WindowCompat.getInsetsController(window, window.decorView).apply {
            systemBarsBehavior = WindowInsetsControllerCompat.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE
            hide(WindowInsetsCompat.Type.systemBars())
        }
    }

    // Buttons from key events and from axes (hat, analog triggers) are tracked separately so
    // a stick movement never clears a d-pad press that arrived as a key event, and vice versa.
    private var keyButtons = 0
    private var axisButtons = 0
    private var physicalLx = 0f
    private var physicalLy = 0f
    private var physicalRx = 0f
    private var physicalRy = 0f
    private var physicalL2 = 0f
    private var physicalR2 = 0f

    private fun sendPad() {
        NativeBridge.nativeSendControllerInput(
            keyButtons or axisButtons, physicalLx, physicalLy, physicalRx, physicalRy, physicalL2, physicalR2
        )
    }

    protected fun releaseEmulatedPad() {
        keyButtons = 0
        axisButtons = 0
        physicalLx = 0f; physicalLy = 0f; physicalRx = 0f; physicalRy = 0f
        physicalL2 = 0f; physicalR2 = 0f
        sendPad()
    }

    /** Swaps A<->B and X<->Y of physical pads (ButtonLayout.Nintendo); games only. */
    protected var swapFaceButtons = false

    private fun faceSwapped(keyCode: Int): Int = if (!swapFaceButtons) keyCode else when (keyCode) {
        KeyEvent.KEYCODE_BUTTON_A -> KeyEvent.KEYCODE_BUTTON_B
        KeyEvent.KEYCODE_BUTTON_B -> KeyEvent.KEYCODE_BUTTON_A
        KeyEvent.KEYCODE_BUTTON_X -> KeyEvent.KEYCODE_BUTTON_Y
        KeyEvent.KEYCODE_BUTTON_Y -> KeyEvent.KEYCODE_BUTTON_X
        else -> keyCode
    }

    private fun padMask(keyCode: Int): Int = when (faceSwapped(keyCode)) {
        KeyEvent.KEYCODE_BUTTON_A -> NativeBridge.BTN_CROSS
        KeyEvent.KEYCODE_BUTTON_B -> NativeBridge.BTN_CIRCLE
        // Pads that report B as Back; the system back key and gesture open the menu instead.
        KeyEvent.KEYCODE_BACK -> NativeBridge.BTN_CIRCLE
        KeyEvent.KEYCODE_BUTTON_X -> NativeBridge.BTN_SQUARE
        KeyEvent.KEYCODE_BUTTON_Y -> NativeBridge.BTN_TRIANGLE
        KeyEvent.KEYCODE_BUTTON_L1 -> NativeBridge.BTN_L1
        KeyEvent.KEYCODE_BUTTON_R1 -> NativeBridge.BTN_R1
        KeyEvent.KEYCODE_BUTTON_L2 -> NativeBridge.BTN_L2
        KeyEvent.KEYCODE_BUTTON_R2 -> NativeBridge.BTN_R2
        KeyEvent.KEYCODE_BUTTON_THUMBL -> NativeBridge.BTN_L3
        KeyEvent.KEYCODE_BUTTON_THUMBR -> NativeBridge.BTN_R3
        KeyEvent.KEYCODE_BUTTON_START -> NativeBridge.BTN_START
        KeyEvent.KEYCODE_BUTTON_SELECT -> NativeBridge.BTN_SELECT
        KeyEvent.KEYCODE_DPAD_UP -> NativeBridge.BTN_UP
        KeyEvent.KEYCODE_DPAD_DOWN -> NativeBridge.BTN_DOWN
        KeyEvent.KEYCODE_DPAD_LEFT -> NativeBridge.BTN_LEFT
        KeyEvent.KEYCODE_DPAD_RIGHT -> NativeBridge.BTN_RIGHT
        else -> 0
    }

    private val pauseCombo = NativeBridge.BTN_SELECT or NativeBridge.BTN_START

    override fun dispatchKeyEvent(event: KeyEvent): Boolean {
        val down = event.action == KeyEvent.ACTION_DOWN

        if (InputRouter.routeToUi) {
            val nav = UiInput.fromKeyCode(event.keyCode)
            if (nav != null) {
                if (down && event.repeatCount % 3 == 0) UiInput.emit(nav)
                return true
            }
            return super.dispatchKeyEvent(event)
        }

        // In game: the guide button, the system back key or gesture, or Select+Start together,
        // opens the pause menu.
        val systemBack = event.keyCode == KeyEvent.KEYCODE_BACK &&
            !event.isFromSource(InputDevice.SOURCE_GAMEPAD) && !event.isFromSource(InputDevice.SOURCE_JOYSTICK)
        if (event.keyCode == KeyEvent.KEYCODE_BUTTON_MODE || systemBack) {
            if (down) InputRouter.onPauseRequested?.invoke()
            return true
        }
        val mask = padMask(event.keyCode)
        if (mask == 0) return super.dispatchKeyEvent(event)
        if (event.repeatCount > 0) return true
        keyButtons = if (down) keyButtons or mask else keyButtons and mask.inv()
        if (down && keyButtons and pauseCombo == pauseCombo) {
            InputRouter.onPauseRequested?.invoke()
            return true
        }
        sendPad()
        return true
    }

    private var lastStickNav: NavKey? = null

    override fun dispatchGenericMotionEvent(event: MotionEvent): Boolean {
        if (!event.isFromSource(InputDevice.SOURCE_CLASS_JOYSTICK)) return super.dispatchGenericMotionEvent(event)

        val hatX = event.getAxisValue(MotionEvent.AXIS_HAT_X)
        val hatY = event.getAxisValue(MotionEvent.AXIS_HAT_Y)
        val lx = event.getAxisValue(MotionEvent.AXIS_X)
        val ly = event.getAxisValue(MotionEvent.AXIS_Y)

        if (InputRouter.routeToUi) {
            // Treat the hat and left stick as a d-pad, emitting once per deflection.
            val x = if (kotlin.math.abs(hatX) > 0.5f) hatX else lx
            val y = if (kotlin.math.abs(hatY) > 0.5f) hatY else ly
            val nav = when {
                x < -0.6f -> NavKey.Left
                x > 0.6f -> NavKey.Right
                y < -0.6f -> NavKey.Up
                y > 0.6f -> NavKey.Down
                else -> null
            }
            if (nav != null && nav != lastStickNav) UiInput.emit(nav)
            lastStickNav = nav
            return true
        }

        physicalLx = lx
        physicalLy = ly
        physicalRx = event.getAxisValue(MotionEvent.AXIS_Z)
        physicalRy = event.getAxisValue(MotionEvent.AXIS_RZ)
        physicalL2 = maxOf(event.getAxisValue(MotionEvent.AXIS_LTRIGGER), event.getAxisValue(MotionEvent.AXIS_BRAKE))
        physicalR2 = maxOf(event.getAxisValue(MotionEvent.AXIS_RTRIGGER), event.getAxisValue(MotionEvent.AXIS_GAS))

        fun set(mask: Int, on: Boolean) {
            axisButtons = if (on) axisButtons or mask else axisButtons and mask.inv()
        }
        set(NativeBridge.BTN_L2, physicalL2 > 0.1f)
        set(NativeBridge.BTN_R2, physicalR2 > 0.1f)
        set(NativeBridge.BTN_LEFT, hatX < -0.5f)
        set(NativeBridge.BTN_RIGHT, hatX > 0.5f)
        set(NativeBridge.BTN_UP, hatY < -0.5f)
        set(NativeBridge.BTN_DOWN, hatY > 0.5f)
        sendPad()
        return true
    }
}
