// SPDX-FileCopyrightText: Copyright 2026 CapyPS5 Project
// SPDX-License-Identifier: GPL-2.0-only

package org.capyps5.emulator.ui.screens

import android.view.InputDevice
import android.view.SurfaceHolder
import android.view.SurfaceView
import androidx.compose.animation.AnimatedVisibility
import androidx.core.view.ViewCompat
import androidx.core.view.WindowInsetsCompat
import androidx.compose.animation.fadeIn
import androidx.compose.animation.fadeOut
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxHeight
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.text.KeyboardActions
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.outlined.Logout
import androidx.compose.material.icons.filled.PlayArrow
import androidx.compose.material.icons.outlined.Keyboard
import androidx.compose.material.icons.outlined.Layers
import androidx.compose.material.icons.outlined.Tune
import androidx.compose.material3.Button
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.size
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateListOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.focus.FocusRequester
import androidx.compose.ui.focus.focusRequester
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.LocalFocusManager
import androidx.compose.ui.platform.LocalSoftwareKeyboardController
import androidx.compose.ui.platform.LocalView
import androidx.compose.ui.text.input.ImeAction
import androidx.compose.ui.unit.dp
import androidx.compose.ui.viewinterop.AndroidView
import androidx.compose.foundation.gestures.detectHorizontalDragGestures
import androidx.compose.foundation.gestures.detectTapGestures
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.layout.boundsInWindow
import androidx.compose.ui.layout.onGloballyPositioned
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.delay
import kotlinx.coroutines.isActive
import kotlinx.coroutines.withContext
import org.capyps5.emulator.InputRouter
import org.capyps5.emulator.NativeBridge
import org.capyps5.emulator.OverlayMode
import org.capyps5.emulator.Settings
import org.capyps5.emulator.TouchControlsMode
import org.capyps5.emulator.model.GameItem
import org.capyps5.emulator.telemetry.PerfMonitor
import org.capyps5.emulator.telemetry.PerfSnapshot
import org.capyps5.emulator.ui.NavKey
import org.capyps5.emulator.ui.OnNavKey
import org.capyps5.emulator.ui.components.LayersPanel
import org.capyps5.emulator.ui.components.PerfOverlay
import org.capyps5.emulator.ui.components.SettingsPanel
import org.capyps5.emulator.ui.components.TouchpadOverlay
import org.capyps5.emulator.ui.components.VirtualGamepad
import org.capyps5.emulator.ui.theme.Capy
import org.capyps5.emulator.R
import androidx.compose.ui.res.stringResource

private fun hasPhysicalGamepad(): Boolean = InputDevice.getDeviceIds().any { id ->
    val device = InputDevice.getDevice(id) ?: return@any false
    !device.isVirtual && (device.sources and InputDevice.SOURCE_GAMEPAD) == InputDevice.SOURCE_GAMEPAD
}

@Composable
fun EmulationScreen(
    game: GameItem,
    settings: Settings,
    versionText: String,
    onExit: () -> Unit,
) {
    val context = LocalContext.current
    var showPauseMenu by remember { mutableStateOf(false) }
    var showSettings by remember { mutableStateOf(false) }
    var showLayers by remember { mutableStateOf(false) }
    var loadError by remember { mutableStateOf<String?>(null) }
    var gamepadConnected by remember { mutableStateOf(hasPhysicalGamepad()) }
    var snapshot by remember { mutableStateOf(PerfSnapshot()) }
    val fpsHistory = remember { mutableStateListOf<Float>() }

    // The core draws PS5 system dialogs (IME keyboard, error dialogs) over the game frame.
    // While one is visible, screen touches go to that dialog and a text bar backed by the
    // Android soft keyboard stays in sync with the dialog text.
    var overlayActive by remember { mutableStateOf(false) }
    var overlayTitle by remember { mutableStateOf("") }
    var overlayText by remember { mutableStateOf("") }
    val overlayFocus = remember { FocusRequester() }
    val keyboard = LocalSoftwareKeyboardController.current
    val focusManager = LocalFocusManager.current
    val inputView = LocalView.current

    LaunchedEffect(Unit) {
        while (isActive) {
            val active = NativeBridge.nativeOverlayActive()
            if (active && !overlayActive) {
                overlayTitle = NativeBridge.nativeOverlayTitle()
                overlayText = NativeBridge.nativeOverlayText()
            }
            val wasActive = overlayActive
            overlayActive = active
            if (active && !wasActive) {
                // The text bar above composes once overlayActive flips; give it a beat, then
                // focus it so a physical keyboard types straight into the dialog, and bring
                // the Android soft keyboard up with it. The toolbar button below hides it
                // again at any time.
                delay(250)
                runCatching { overlayFocus.requestFocus() }
                delay(100)
                keyboard?.show()
            } else if (!active && wasActive) {
                focusManager.clearFocus(force = true)
                keyboard?.hide()
            }
            delay(250)
        }
    }

    // While any menu is open, controller input drives the UI instead of the game.
    val uiHasInput = showPauseMenu || showSettings || showLayers || loadError != null
    LaunchedEffect(uiHasInput) { InputRouter.setRouteToUi(uiHasInput) }
    DisposableEffect(Unit) {
        InputRouter.onPauseRequested = { showPauseMenu = true }
        // While a system dialog is up, the system back key closes the dialog (as Circle does
        // on a pad) instead of pausing the game.
        InputRouter.onSystemBack = {
            if (overlayActive) {
                NativeBridge.nativeOverlayCancel()
                true
            } else {
                false
            }
        }
        onDispose {
            InputRouter.onPauseRequested = null
            InputRouter.onSystemBack = null
            InputRouter.setRouteToUi(true)
        }
    }

    LaunchedEffect(Unit) {
        while (isActive) {
            gamepadConnected = hasPhysicalGamepad()
            delay(2000)
        }
    }

    LaunchedEffect(settings.overlayMode) {
        if (settings.overlayMode == OverlayMode.Off) return@LaunchedEffect
        val monitor = withContext(Dispatchers.IO) { PerfMonitor(context) }
        while (isActive) {
            val sample = withContext(Dispatchers.IO) { monitor.sample() }
            snapshot = sample
            fpsHistory.add(sample.fps)
            if (fpsHistory.size > 60) fpsHistory.removeAt(0)
            delay(500)
        }
    }

    val showTouch = when (settings.touchControls) {
        TouchControlsMode.Always -> true
        TouchControlsMode.Never -> false
        TouchControlsMode.Auto -> !gamepadConnected
    }

    Box(Modifier.fillMaxSize().background(Color.Black)) {
        AndroidView(
            modifier = Modifier.fillMaxSize(),
            factory = { ctx ->
                SurfaceView(ctx).apply {
                    holder.addCallback(object : SurfaceHolder.Callback {
                        private var started = false

                        override fun surfaceCreated(holder: SurfaceHolder) {
                            NativeBridge.nativeSetSurface(holder.surface)
                            if (started) return
                            started = true
                            if (!java.io.File(game.elfPath).exists()) {
                                loadError = context.getString(R.string.error_elf_missing, game.elfPath)
                                return
                            }
                            if (!NativeBridge.nativeStartGame(game.directoryPath, game.elfPath)) {
                                loadError = context.getString(R.string.error_elf_start)
                            }
                        }

                        override fun surfaceChanged(holder: SurfaceHolder, format: Int, width: Int, height: Int) {
                            if (holder.surface.isValid) NativeBridge.nativeSetSurface(holder.surface)
                        }

                        override fun surfaceDestroyed(holder: SurfaceHolder) {
                            NativeBridge.nativeDestroySurface()
                        }
                    })
                }
            },
        )

        if (overlayActive) {
            // Taps land on the dialog the core drew (its on-screen keyboard included).
            Box(
                Modifier
                    .fillMaxSize()
                    .pointerInput(Unit) {
                        detectTapGestures(
                            onPress = { offset ->
                                NativeBridge.nativeOverlayMouse(offset.x, offset.y, true)
                                try {
                                    awaitRelease()
                                } finally {
                                    NativeBridge.nativeOverlayMouse(offset.x, offset.y, false)
                                }
                            },
                        )
                    },
            )
            // Text entry via the Android soft keyboard, synced with the dialog text.
            Column(
                modifier = Modifier
                    .align(Alignment.TopCenter)
                    .padding(top = 64.dp)
                    .fillMaxWidth(0.94f)
                    .background(Capy.Surface, RoundedCornerShape(14.dp))
                    .padding(horizontal = 12.dp, vertical = 10.dp),
            ) {
                if (overlayTitle.isNotBlank()) {
                    Text(
                        overlayTitle,
                        style = MaterialTheme.typography.labelMedium,
                        color = Capy.TextSecondary,
                    )
                    Spacer(Modifier.height(6.dp))
                }
                Row(
                    verticalAlignment = Alignment.CenterVertically,
                    horizontalArrangement = Arrangement.spacedBy(8.dp),
                ) {
                    OutlinedTextField(
                        value = overlayText,
                        onValueChange = { value ->
                            overlayText = value
                            NativeBridge.nativeOverlaySetText(value)
                        },
                        modifier = Modifier.weight(1f).focusRequester(overlayFocus),
                        singleLine = true,
                        keyboardOptions = KeyboardOptions(imeAction = ImeAction.Done),
                        // Enter on a physical keyboard and "Done" on the soft keyboard both
                        // confirm the dialog.
                        keyboardActions = KeyboardActions(onDone = { NativeBridge.nativeOverlayAccept() }),
                    )
                    // Toggles the Android soft keyboard; typing is synced live with the dialog.
                    IconButton(
                        onClick = {
                            val imeVisible = ViewCompat.getRootWindowInsets(inputView)
                                ?.isVisible(WindowInsetsCompat.Type.ime()) == true
                            if (imeVisible) {
                                keyboard?.hide()
                            } else {
                                runCatching { overlayFocus.requestFocus() }
                                keyboard?.show()
                            }
                        },
                    ) {
                        Icon(
                            Icons.Outlined.Keyboard,
                            contentDescription = stringResource(R.string.ime_keyboard_toggle),
                        )
                    }
                    Button(onClick = { NativeBridge.nativeOverlayAccept() }) {
                        Text("OK")
                    }
                    TextButton(onClick = { NativeBridge.nativeOverlayCancel() }) {
                        Text(stringResource(android.R.string.cancel))
                    }
                }
            }
        }

        // Swiping in from the left edge opens the pause menu (as in other Android emulators). The
        // strip is excluded from the system back gesture so Android does not take the swipe.
        val density = androidx.compose.ui.platform.LocalDensity.current
        val view = androidx.compose.ui.platform.LocalView.current
        Box(
            Modifier
                .align(Alignment.CenterStart)
                .width(28.dp)
                .fillMaxHeight()
                .onGloballyPositioned { coordinates ->
                    val bounds = coordinates.boundsInWindow()
                    view.systemGestureExclusionRects = listOf(
                        android.graphics.Rect(
                            bounds.left.toInt(), bounds.top.toInt(), bounds.right.toInt(), bounds.bottom.toInt()
                        )
                    )
                }
                .pointerInput(Unit) {
                    var dragged = 0f
                    detectHorizontalDragGestures(
                        onDragStart = { dragged = 0f },
                        onHorizontalDrag = { change, amount ->
                            dragged += amount
                            if (dragged > with(density) { 72.dp.toPx() }) {
                                change.consume()
                                showPauseMenu = true
                            }
                        },
                    )
                },
        )

        if (showTouch && !overlayActive) {
            VirtualGamepad(
                modifier = Modifier.fillMaxSize(),
                alpha = settings.controlsOpacity,
                showTouchpad = settings.touchpadOverlay != TouchControlsMode.Never,
                onMenuClick = { showPauseMenu = true },
            )
        } else if (settings.touchpadOverlay == TouchControlsMode.Always && !overlayActive) {
            TouchpadOverlay(Modifier.fillMaxSize(), alpha = settings.controlsOpacity)
        }

        PerfOverlay(
            mode = settings.overlayMode,
            snapshot = snapshot,
            fpsHistory = fpsHistory,
            modifier = Modifier.align(Alignment.TopStart).padding(10.dp),
        )

        AnimatedVisibility(visible = showPauseMenu && !showSettings && !showLayers, enter = fadeIn(), exit = fadeOut()) {
            PauseMenu(
                game = game,
                onResume = { showPauseMenu = false },
                onSettings = { showSettings = true },
                onLayers = {
                    showPauseMenu = false
                    showLayers = true
                },
                onExit = onExit,
            )
        }

        if (showSettings) {
            Box(Modifier.fillMaxSize().background(Capy.Scrim).clickable { showSettings = false })
            SettingsPanel(
                settings, versionText,
                onClose = { showSettings = false },
                modifier = Modifier.align(Alignment.CenterEnd),
            )
        }

        if (showLayers) {
            LayersPanel(onClose = { showLayers = false }, modifier = Modifier.align(Alignment.CenterEnd))
        }

        loadError?.let { message ->
            ErrorCard(message, onExit)
        }
    }
}

@Composable
private fun PauseMenu(
    game: GameItem,
    onResume: () -> Unit,
    onSettings: () -> Unit,
    onLayers: () -> Unit,
    onExit: () -> Unit,
) {
    val entries = listOf(
        Triple(stringResource(R.string.resume), Icons.Filled.PlayArrow, onResume),
        Triple(stringResource(R.string.settings), Icons.Outlined.Tune, onSettings),
        Triple(stringResource(R.string.layers), Icons.Outlined.Layers, onLayers),
        Triple(stringResource(R.string.exit), Icons.AutoMirrored.Outlined.Logout, onExit),
    )
    var focused by remember { mutableIntStateOf(0) }
    OnNavKey { key ->
        when (key) {
            NavKey.Left -> focused = (focused - 1).mod(entries.size)
            NavKey.Right -> focused = (focused + 1).mod(entries.size)
            NavKey.Confirm -> entries[focused].third()
            NavKey.Back, NavKey.Menu -> onResume()
            else -> Unit
        }
    }
    Box(
        Modifier.fillMaxSize().background(Capy.Scrim).clickable(onClick = onResume),
        contentAlignment = Alignment.BottomStart,
    ) {
        Column(
            modifier = Modifier
                .fillMaxWidth()
                .background(Capy.Surface)
                .clickable(enabled = false) {}
                .padding(horizontal = 28.dp, vertical = 18.dp),
            verticalArrangement = Arrangement.spacedBy(12.dp),
        ) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Text(game.titleName, style = MaterialTheme.typography.titleLarge, maxLines = 1)
                Spacer(Modifier.width(12.dp))
                Text(game.titleId, style = MaterialTheme.typography.bodySmall, color = Capy.TextSecondary)
            }
            Row(horizontalArrangement = Arrangement.spacedBy(12.dp)) {
                entries.forEachIndexed { i, (label, icon, action) ->
                    val isFocused = i == focused
                    val danger = i == entries.lastIndex
                    Column(
                        modifier = Modifier
                            .width(112.dp)
                            .background(
                                when {
                                    isFocused && danger -> Capy.Bad
                                    isFocused -> Color.White
                                    else -> Capy.SurfaceHigh
                                },
                                RoundedCornerShape(14.dp),
                            )
                            .clickable { focused = i; action() }
                            .padding(vertical = 14.dp),
                        horizontalAlignment = Alignment.CenterHorizontally,
                        verticalArrangement = Arrangement.spacedBy(6.dp),
                    ) {
                        val tint = when {
                            isFocused && danger -> Color.White
                            isFocused -> Color.Black
                            danger -> Capy.Bad
                            else -> Capy.TextPrimary
                        }
                        Icon(icon, null, Modifier.size(24.dp), tint = tint)
                        Text(label, style = MaterialTheme.typography.labelLarge, color = tint)
                    }
                }
            }
            HintBar(
                listOf(
                    "◀ ▶" to stringResource(R.string.hint_choose),
                    "Ⓐ" to stringResource(R.string.hint_accept),
                    "Ⓑ" to stringResource(R.string.hint_back_to_game),
                ),
            )
        }
    }
}

@Composable
private fun ErrorCard(message: String, onExit: () -> Unit) {
    OnNavKey { key -> if (key == NavKey.Confirm || key == NavKey.Back) onExit() }
    Box(Modifier.fillMaxSize().background(Capy.Scrim), contentAlignment = Alignment.Center) {
        Column(
            modifier = Modifier
                .width(420.dp)
                .background(Capy.Surface, RoundedCornerShape(16.dp))
                .padding(22.dp),
            verticalArrangement = Arrangement.spacedBy(12.dp),
        ) {
            Text(stringResource(R.string.error_start_title), style = MaterialTheme.typography.titleLarge, color = Capy.Bad)
            Text(message, style = MaterialTheme.typography.bodyMedium, color = Capy.TextSecondary)
            Text(
                stringResource(R.string.back_to_home_a),
                style = MaterialTheme.typography.labelLarge,
                color = Color.White,
                modifier = Modifier
                    .background(Capy.Accent, RoundedCornerShape(50))
                    .clickable(onClick = onExit)
                    .padding(horizontal = 18.dp, vertical = 9.dp),
            )
        }
    }
}
