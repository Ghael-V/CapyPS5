package org.capyps5.emulator.ui.screens

import android.view.SurfaceHolder
import android.view.SurfaceView
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.*
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.unit.dp
import androidx.compose.ui.viewinterop.AndroidView
import org.capyps5.emulator.NativeBridge
import org.capyps5.emulator.model.GameItem
import org.capyps5.emulator.ui.components.VirtualGamepad

@Composable
fun EmulationScreen(
    game: GameItem,
    onExit: () -> Unit
) {
    var isPaused by remember { mutableStateOf(false) }
    var showPauseMenu by remember { mutableStateOf(false) }
    var loadError by remember { mutableStateOf<String?>(null) }

    Box(
        modifier = Modifier
            .fillMaxSize()
            .background(Color.Black)
    ) {
        // Native Vulkan SurfaceView
        AndroidView(
            modifier = Modifier.fillMaxSize(),
            factory = { context ->
                SurfaceView(context).apply {
                    holder.addCallback(object : SurfaceHolder.Callback {
                        override fun surfaceCreated(holder: SurfaceHolder) {
                            android.util.Log.i("CapyPS5-UI", "surfaceCreated called, valid=${holder.surface.isValid}")
                            NativeBridge.nativeSetSurface(holder.surface)
                            val file = java.io.File(game.elfPath)
                            if (!file.exists()) {
                                loadError = "No se encontró el ejecutable:\n${game.elfPath}\n\nPor favor, coloca un archivo ELF/EBOOT de PS5 en el dispositivo o usa '+ Cargar ELF / EBOOT'."
                                return
                            }
                            val success = NativeBridge.nativeStartGame(game.directoryPath, game.elfPath)
                            if (!success) {
                                loadError = "No se pudo iniciar el ejecutable.\nComprueba los registros de depuración (logcat)."
                            }
                        }

                        override fun surfaceChanged(
                            holder: SurfaceHolder,
                            format: Int,
                            width: Int,
                            height: Int
                        ) {
                            android.util.Log.i("CapyPS5-UI", "surfaceChanged: ${width}x${height}, format=$format, valid=${holder.surface.isValid}")
                            if (holder.surface.isValid) {
                                NativeBridge.nativeSetSurface(holder.surface)
                            }
                        }

                        override fun surfaceDestroyed(holder: SurfaceHolder) {
                            android.util.Log.i("CapyPS5-UI", "surfaceDestroyed called")
                            NativeBridge.nativeDestroySurface()
                        }
                    })
                }
            }
        )

        // Overlay touch controls
        VirtualGamepad(
            modifier = Modifier.fillMaxSize(),
            onMenuClick = {
                showPauseMenu = true
            }
        )

        // Pause Menu Dialog
        if (showPauseMenu) {
            AlertDialog(
                onDismissRequest = { showPauseMenu = false },
                title = { Text(text = game.titleName) },
                text = {
                    Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
                        Text(text = "ID: ${game.titleId}")
                        Text(text = "Ruta: ${game.elfPath}")
                    }
                },
                confirmButton = {
                    TextButton(onClick = { showPauseMenu = false }) {
                        Text("Reanudar")
                    }
                },
                dismissButton = {
                    Button(
                        colors = ButtonDefaults.buttonColors(containerColor = MaterialTheme.colorScheme.error),
                        onClick = {
                            showPauseMenu = false
                            NativeBridge.nativeStopGame()
                            onExit()
                        }
                    ) {
                        Text("Detener y Salir")
                    }
                }
            )
        }

        // Error Dialog
        if (loadError != null) {
            AlertDialog(
                onDismissRequest = onExit,
                title = { Text(text = "Aviso de Carga") },
                text = { Text(text = loadError!!) },
                confirmButton = {
                    Button(onClick = onExit) {
                        Text("Volver al Menú")
                    }
                }
            )
        }
    }
}
