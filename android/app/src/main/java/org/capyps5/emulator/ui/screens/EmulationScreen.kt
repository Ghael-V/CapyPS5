package org.capyps5.emulator.ui.screens

import android.view.InputDevice
import android.view.SurfaceHolder
import android.view.SurfaceView
import androidx.compose.animation.AnimatedVisibility
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
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
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
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.unit.dp
import androidx.compose.ui.viewinterop.AndroidView
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
import org.capyps5.emulator.ui.components.PerfOverlay
import org.capyps5.emulator.ui.components.SettingsPanel
import org.capyps5.emulator.ui.components.VirtualGamepad
import org.capyps5.emulator.ui.theme.Capy

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
    var loadError by remember { mutableStateOf<String?>(null) }
    var gamepadConnected by remember { mutableStateOf(hasPhysicalGamepad()) }
    var snapshot by remember { mutableStateOf(PerfSnapshot()) }
    val fpsHistory = remember { mutableStateListOf<Float>() }

    // While any menu is open, controller input drives the UI instead of the game.
    val uiHasInput = showPauseMenu || showSettings || loadError != null
    LaunchedEffect(uiHasInput) { InputRouter.setRouteToUi(uiHasInput) }
    DisposableEffect(Unit) {
        InputRouter.onPauseRequested = { showPauseMenu = true }
        onDispose {
            InputRouter.onPauseRequested = null
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
                                loadError = "No se encontró el ejecutable:\n${game.elfPath}"
                                return
                            }
                            if (!NativeBridge.nativeStartGame(game.directoryPath, game.elfPath)) {
                                loadError = "No se pudo iniciar el ejecutable. Revisa logcat (CapyPS5-*)."
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

        if (showTouch) {
            VirtualGamepad(
                modifier = Modifier.fillMaxSize(),
                alpha = settings.controlsOpacity,
                onMenuClick = { showPauseMenu = true },
            )
        }

        PerfOverlay(
            mode = settings.overlayMode,
            snapshot = snapshot,
            fpsHistory = fpsHistory,
            modifier = Modifier.align(Alignment.TopStart).padding(10.dp),
        )

        AnimatedVisibility(visible = showPauseMenu && !showSettings, enter = fadeIn(), exit = fadeOut()) {
            PauseMenu(
                game = game,
                onResume = { showPauseMenu = false },
                onSettings = { showSettings = true },
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

        loadError?.let { message ->
            ErrorCard(message, onExit)
        }
    }
}

@Composable
private fun PauseMenu(game: GameItem, onResume: () -> Unit, onSettings: () -> Unit, onExit: () -> Unit) {
    val entries = listOf("Reanudar" to onResume, "Ajustes" to onSettings, "Salir del juego" to onExit)
    var focused by remember { mutableIntStateOf(0) }
    OnNavKey { key ->
        when (key) {
            NavKey.Up -> focused = (focused - 1).mod(entries.size)
            NavKey.Down -> focused = (focused + 1).mod(entries.size)
            NavKey.Confirm -> entries[focused].second()
            NavKey.Back, NavKey.Menu -> onResume()
            else -> Unit
        }
    }
    Box(
        Modifier.fillMaxSize().background(Capy.Scrim).clickable(onClick = onResume),
        contentAlignment = Alignment.Center,
    ) {
        Column(
            modifier = Modifier
                .width(320.dp)
                .background(Capy.Surface, RoundedCornerShape(16.dp))
                .border(1.dp, Capy.Outline, RoundedCornerShape(16.dp))
                .clickable(enabled = false) {}
                .padding(20.dp),
            verticalArrangement = Arrangement.spacedBy(6.dp),
        ) {
            Text(game.titleName, style = MaterialTheme.typography.titleLarge, maxLines = 1)
            Text(game.titleId, style = MaterialTheme.typography.bodySmall, color = Capy.TextSecondary)
            Spacer(Modifier.height(8.dp))
            entries.forEachIndexed { i, (label, action) ->
                val isFocused = i == focused
                val danger = i == entries.lastIndex
                Row(
                    modifier = Modifier
                        .background(
                            when {
                                isFocused && danger -> Capy.Bad
                                isFocused -> Capy.Accent
                                else -> Capy.SurfaceHigh
                            },
                            RoundedCornerShape(10.dp),
                        )
                        .clickable(onClick = action)
                        .padding(horizontal = 16.dp, vertical = 11.dp),
                ) {
                    Text(
                        label,
                        style = MaterialTheme.typography.titleMedium,
                        color = if (isFocused) Color.White else if (danger) Capy.Bad else Capy.TextPrimary,
                        modifier = Modifier.weight(1f),
                    )
                }
            }
            Spacer(Modifier.height(6.dp))
            HintBar(listOf("Ⓐ" to "Elegir", "Ⓑ" to "Volver"))
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
            Text("No se pudo arrancar", style = MaterialTheme.typography.titleLarge, color = Capy.Bad)
            Text(message, style = MaterialTheme.typography.bodyMedium, color = Capy.TextSecondary)
            Text(
                "Volver al inicio  Ⓐ",
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
