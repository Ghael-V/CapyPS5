package org.capyps5.emulator.ui.screens

import android.net.Uri
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.*
import androidx.compose.ui.res.painterResource
import androidx.compose.foundation.lazy.grid.GridCells
import androidx.compose.foundation.lazy.grid.LazyVerticalGrid
import androidx.compose.foundation.lazy.grid.items
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Add
import androidx.compose.material.icons.filled.PlayArrow
import androidx.compose.material.icons.filled.Settings
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import org.capyps5.emulator.NativeBridge
import org.capyps5.emulator.model.GameItem
import org.capyps5.emulator.ui.theme.CapyPrimary
import org.capyps5.emulator.ui.theme.CapySecondary
import org.capyps5.emulator.ui.theme.CapySurface
import org.capyps5.emulator.ui.theme.CapySurfaceVariant

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun HomeScreen(
    games: List<GameItem>,
    onGameSelected: (GameItem) -> Unit,
    onAddGameFile: (String) -> Unit
) {
    val context = LocalContext.current
    var versionText by remember { mutableStateOf("CapyPS5 v0.1.0-alpha") }

    LaunchedEffect(Unit) {
        try {
            versionText = NativeBridge.nativeGetVersion()
        } catch (_: Exception) {}
    }

    val filePickerLauncher = rememberLauncherForActivityResult(
        contract = ActivityResultContracts.OpenDocument()
    ) { uri: Uri? ->
        uri?.let {
            onAddGameFile(it.toString())
        }
    }

    Scaffold(
        topBar = {
            TopAppBar(
                title = {
                    Row(
                        verticalAlignment = Alignment.CenterVertically,
                        horizontalArrangement = Arrangement.spacedBy(12.dp)
                    ) {
                        Image(
                            painter = painterResource(id = org.capyps5.emulator.R.mipmap.ic_launcher),
                            contentDescription = "CapyPS5 Logo",
                            modifier = Modifier
                                .size(40.dp)
                                .clip(RoundedCornerShape(8.dp))
                        )
                        Text(
                            text = "CapyPS5",
                            fontWeight = FontWeight.ExtraBold,
                            fontSize = 24.sp,
                            color = CapySecondary
                        )
                        AssistChip(
                            onClick = {},
                            label = { Text(versionText, fontSize = 11.sp) },
                            colors = AssistChipDefaults.assistChipColors(
                                containerColor = CapySurfaceVariant,
                                labelColor = Color.White
                            )
                        )
                        AssistChip(
                            onClick = {},
                            label = { Text("Vulkan 1.3 • ARM64", fontSize = 11.sp) },
                            colors = AssistChipDefaults.assistChipColors(
                                containerColor = CapyPrimary.copy(alpha = 0.3f),
                                labelColor = CapySecondary
                            )
                        )
                    }
                },
                actions = {
                    IconButton(onClick = { /* TODO: Open Settings dialog */ }) {
                        Icon(Icons.Default.Settings, contentDescription = "Configuración", tint = Color.White)
                    }
                },
                colors = TopAppBarDefaults.topAppBarColors(
                    containerColor = MaterialTheme.colorScheme.background
                )
            )
        },
        floatingActionButton = {
            ExtendedFloatingActionButton(
                onClick = {
                    filePickerLauncher.launch(arrayOf("*/*"))
                },
                icon = { Icon(Icons.Default.Add, contentDescription = "Cargar juego") },
                text = { Text("Cargar ELF / EBOOT") },
                containerColor = CapyPrimary,
                contentColor = Color.White
            )
        }
    ) { padding ->
        Box(
            modifier = Modifier
                .fillMaxSize()
                .padding(padding)
                .padding(horizontal = 24.dp)
        ) {
            if (games.isEmpty()) {
                // Empty State
                Column(
                    modifier = Modifier.align(Alignment.Center),
                    horizontalAlignment = Alignment.CenterHorizontally,
                    verticalArrangement = Arrangement.spacedBy(16.dp)
                ) {
                    Text(
                        text = "Biblioteca vacía",
                        fontSize = 20.sp,
                        fontWeight = FontWeight.Bold,
                        color = Color.White
                    )
                    Text(
                        text = "Selecciona un archivo binario ELF o directorio de juego de PlayStation 5 para iniciar la emulación.",
                        fontSize = 14.sp,
                        color = MaterialTheme.colorScheme.onSurface.copy(alpha = 0.7f),
                        modifier = Modifier.widthIn(max = 480.dp),
                        textAlign = androidx.compose.ui.text.style.TextAlign.Center
                    )
                    Button(
                        onClick = { filePickerLauncher.launch(arrayOf("*/*")) },
                        colors = ButtonDefaults.buttonColors(containerColor = CapyPrimary)
                    ) {
                        Icon(Icons.Default.Add, contentDescription = null, modifier = Modifier.size(18.dp))
                        Spacer(modifier = Modifier.width(8.dp))
                        Text("Explorar Archivos")
                    }
                }
            } else {
                // Grid of games
                LazyVerticalGrid(
                    columns = GridCells.Adaptive(minSize = 220.dp),
                    horizontalArrangement = Arrangement.spacedBy(16.dp),
                    verticalArrangement = Arrangement.spacedBy(16.dp),
                    modifier = Modifier.fillMaxSize().padding(top = 16.dp)
                ) {
                    items(games) { game ->
                        GameCard(game = game, onClick = { onGameSelected(game) })
                    }
                }
            }
        }
    }
}

@Composable
fun GameCard(
    game: GameItem,
    onClick: () -> Unit
) {
    Card(
        modifier = Modifier
            .fillMaxWidth()
            .clickable(onClick = onClick),
        shape = RoundedCornerShape(12.dp),
        colors = CardDefaults.cardColors(containerColor = CapySurface)
    ) {
        Column(
            modifier = Modifier.padding(16.dp),
            verticalArrangement = Arrangement.spacedBy(8.dp)
        ) {
            Box(
                modifier = Modifier
                    .fillMaxWidth()
                    .height(110.dp)
                    .clip(RoundedCornerShape(8.dp))
                    .background(CapySurfaceVariant),
                contentAlignment = Alignment.Center
            ) {
                Icon(
                    Icons.Default.PlayArrow,
                    contentDescription = null,
                    tint = CapySecondary,
                    modifier = Modifier.size(48.dp)
                )
            }

            Text(
                text = game.titleName,
                fontWeight = FontWeight.Bold,
                fontSize = 16.sp,
                color = Color.White,
                maxLines = 1
            )
            Text(
                text = game.titleId,
                fontSize = 12.sp,
                color = MaterialTheme.colorScheme.onSurface.copy(alpha = 0.6f)
            )
        }
    }
}
