// SPDX-FileCopyrightText: Copyright 2026 CapyPS5 Project
// SPDX-License-Identifier: GPL-2.0-only

package org.capyps5.emulator.ui

import android.graphics.BitmapFactory
import android.view.KeyEvent
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.State
import androidx.compose.runtime.produceState
import androidx.compose.runtime.rememberUpdatedState
import androidx.compose.ui.graphics.ImageBitmap
import androidx.compose.ui.graphics.asImageBitmap
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.flow.MutableSharedFlow
import kotlinx.coroutines.flow.SharedFlow
import kotlinx.coroutines.withContext

/** Controller/keyboard actions used to drive the frontend UI without touch. */
enum class NavKey { Up, Down, Left, Right, Confirm, Back, Menu, Secondary, TabLeft, TabRight }

object UiInput {
    private val _keys = MutableSharedFlow<NavKey>(extraBufferCapacity = 16)
    val keys: SharedFlow<NavKey> = _keys

    fun fromKeyCode(keyCode: Int): NavKey? = when (keyCode) {
        KeyEvent.KEYCODE_DPAD_UP -> NavKey.Up
        KeyEvent.KEYCODE_DPAD_DOWN -> NavKey.Down
        KeyEvent.KEYCODE_DPAD_LEFT -> NavKey.Left
        KeyEvent.KEYCODE_DPAD_RIGHT -> NavKey.Right
        KeyEvent.KEYCODE_BUTTON_A, KeyEvent.KEYCODE_ENTER, KeyEvent.KEYCODE_DPAD_CENTER -> NavKey.Confirm
        KeyEvent.KEYCODE_BUTTON_B, KeyEvent.KEYCODE_BACK, KeyEvent.KEYCODE_ESCAPE -> NavKey.Back
        KeyEvent.KEYCODE_BUTTON_START, KeyEvent.KEYCODE_MENU, KeyEvent.KEYCODE_BUTTON_MODE -> NavKey.Menu
        KeyEvent.KEYCODE_BUTTON_Y -> NavKey.Secondary
        KeyEvent.KEYCODE_BUTTON_L1 -> NavKey.TabLeft
        KeyEvent.KEYCODE_BUTTON_R1 -> NavKey.TabRight
        else -> null
    }

    fun emit(key: NavKey): Boolean = _keys.tryEmit(key)
}

/** Collects controller navigation while this composable is in the composition. */
@Composable
fun OnNavKey(enabled: Boolean = true, handler: (NavKey) -> Unit) {
    val current = rememberUpdatedState(handler)
    LaunchedEffect(enabled) {
        if (enabled) UiInput.keys.collect { current.value(it) }
    }
}

/** Decodes an image off the main thread, downsampled to roughly [maxSize] pixels. */
@Composable
fun rememberImage(path: String?, maxSize: Int): State<ImageBitmap?> =
    produceState<ImageBitmap?>(initialValue = null, path) {
        value = if (path == null) null else withContext(Dispatchers.IO) {
            runCatching {
                val bounds = BitmapFactory.Options().apply { inJustDecodeBounds = true }
                BitmapFactory.decodeFile(path, bounds)
                var sample = 1
                while (bounds.outWidth / (sample * 2) >= maxSize && bounds.outHeight / (sample * 2) >= maxSize) sample *= 2
                BitmapFactory.decodeFile(path, BitmapFactory.Options().apply { inSampleSize = sample })?.asImageBitmap()
            }.getOrNull()
        }
    }
