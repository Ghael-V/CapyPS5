// SPDX-FileCopyrightText: Copyright 2026 CapyPS5 Project
// SPDX-License-Identifier: GPL-2.0-only

package org.capyps5.emulator

import android.util.Log
import android.view.Surface

object NativeBridge {
    private const val TAG = "CapyPS5-Native"

    init {
        try {
            System.loadLibrary("c++_shared")
            System.loadLibrary("capyps5")
            Log.i(TAG, "Native libraries loaded successfully")
        } catch (e: UnsatisfiedLinkError) {
            Log.e(TAG, "Failed to load CapyPS5 native library", e)
        }
    }

    external fun nativeInit(filesDir: String, cacheDir: String)
    external fun nativeSetSurface(surface: Surface)
    external fun nativeDestroySurface()
    external fun nativeStartGame(gamePath: String, elfPath: String): Boolean
    external fun nativeStopGame()
    /** Sets a core option (a debug.capy.* name) for this session; call before nativeStartGame. */
    external fun nativeSetOption(name: String, value: Long)
    /** Vulkan driver for this session; an empty library keeps the system driver. Call before nativeStartGame. */
    external fun nativeSetGpuDriver(nativeLibraryDir: String, driverDir: String, library: String)
    external fun nativeSendControllerInput(
        buttons: Int,
        lx: Float,
        ly: Float,
        rx: Float,
        ry: Float,
        l2: Float,
        r2: Float
    )
    external fun nativeGetVersion(): String
    /** On-screen touchpad: [finger] 0 or 1, [x]/[y] normalized to the pad (0..1). */
    external fun nativeSendTouch(finger: Int, down: Boolean, x: Float, y: Float)
    /** Touchpad click, merged into whatever the controllers send. */
    external fun nativeSetTouchpadClick(down: Boolean)

    /** Render passes of the last frame, 5 longs each; see LayersPanel. */
    external fun nativeGetPasses(): LongArray
    external fun nativeSetPassSkipped(pixelShader: Long, skipped: Boolean)
    external fun nativeClearSkippedPasses()

    /** Cumulative counters; layout matches nativeGetStats() in capyps5_jni.cpp. */
    external fun nativeGetStats(): LongArray

    const val STAT_GAME_FRAMES = 0
    const val STAT_HOST_PRESENTS = 1
    const val STAT_MAX_FRAME_NS = 2 // reset to 0 on every read
    const val STAT_WAIT_TIMEOUTS = 3
    const val STAT_WAIT_UNMAPPED = 4
    const val STAT_NULL_BUFFERS = 5
    const val STAT_RUNNING = 6

    // Button bitmasks corresponding to PS5 DualSense
    const val BTN_SELECT = 1 shl 0
    const val BTN_L3 = 1 shl 1
    const val BTN_R3 = 1 shl 2
    const val BTN_START = 1 shl 3
    const val BTN_UP = 1 shl 4
    const val BTN_RIGHT = 1 shl 5
    const val BTN_DOWN = 1 shl 6
    const val BTN_LEFT = 1 shl 7
    const val BTN_L2 = 1 shl 8
    const val BTN_R2 = 1 shl 9
    const val BTN_L1 = 1 shl 10
    const val BTN_R1 = 1 shl 11
    const val BTN_TRIANGLE = 1 shl 12
    const val BTN_CIRCLE = 1 shl 13
    const val BTN_CROSS = 1 shl 14
    const val BTN_SQUARE = 1 shl 15
    const val BTN_TOUCHPAD = 1 shl 16
}
