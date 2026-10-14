package org.capyps5.emulator.ui.screens

import android.net.Uri
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.animation.AnimatedVisibility
import androidx.compose.animation.Crossfade
import androidx.compose.animation.core.animateDpAsState
import androidx.compose.animation.fadeIn
import androidx.compose.animation.fadeOut
import androidx.compose.animation.slideInHorizontally
import androidx.compose.animation.slideOutHorizontally
import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.lazy.LazyRow
import androidx.compose.foundation.lazy.itemsIndexed
import androidx.compose.foundation.lazy.rememberLazyListState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Add
import androidx.compose.material.icons.filled.PlayArrow
import androidx.compose.material.icons.filled.Refresh
import androidx.compose.material.icons.filled.Settings
import androidx.compose.material.icons.outlined.SportsEsports
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.produceState
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.res.painterResource
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import kotlinx.coroutines.delay
import org.capyps5.emulator.R
import org.capyps5.emulator.Settings
import org.capyps5.emulator.model.GameItem
import org.capyps5.emulator.ui.NavKey
import org.capyps5.emulator.ui.OnNavKey
import org.capyps5.emulator.ui.components.SettingsPanel
import org.capyps5.emulator.ui.rememberImage
import org.capyps5.emulator.ui.theme.Capy
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

@Composable
fun HomeScreen(
    games: List<GameItem>,
    libraryPath: String,
    settings: Settings,
    versionText: String,
    onGameSelected: (GameItem) -> Unit,
    onAddGameFile: (String) -> Unit,
    onRefresh: () -> Unit,
) {
    var selected by remember { mutableIntStateOf(0) }
    var showSettings by remember { mutableStateOf(false) }
    val listState = rememberLazyListState()
    val current = games.getOrNull(selected.coerceIn(0, (games.size - 1).coerceAtLeast(0)))

    val filePicker = rememberLauncherForActivityResult(ActivityResultContracts.OpenDocument()) { uri: Uri? ->
        uri?.let { onAddGameFile(it.toString()) }
    }

    LaunchedEffect(selected, games.size) {
        if (games.isNotEmpty()) listState.animateScrollToItem(selected.coerceIn(0, games.size - 1))
    }

    OnNavKey(enabled = !showSettings) { key ->
        when (key) {
            NavKey.Left -> if (games.isNotEmpty()) selected = (selected - 1).coerceAtLeast(0)
            NavKey.Right -> if (games.isNotEmpty()) selected = (selected + 1).coerceAtMost(games.size - 1)
            NavKey.Confirm -> current?.let(onGameSelected)
            NavKey.Menu, NavKey.Secondary -> showSettings = true
            else -> Unit
        }
    }

    Box(Modifier.fillMaxSize().background(Capy.Background)) {
        // Key art of the selected game, faded into the background like a console dashboard.
        val art by rememberImage(current?.backgroundPath, 1280)
        Crossfade(targetState = art, label = "background") { bitmap ->
            if (bitmap != null) {
                Image(bitmap, null, Modifier.fillMaxSize(), contentScale = ContentScale.Crop, alpha = 0.55f)
            } else {
                Image(
                    painterResource(R.drawable.capy_bg), null, Modifier.fillMaxSize(),
                    contentScale = ContentScale.Crop, alpha = 0.18f,
                )
            }
        }
        Box(
            Modifier.fillMaxSize().background(
                Brush.verticalGradient(0f to Color(0x6607090E), 0.45f to Color(0x3307090E), 1f to Capy.Background)
            )
        )
        Box(
            Modifier.fillMaxSize().background(
                Brush.horizontalGradient(0f to Color(0xCC07090E), 0.6f to Color.Transparent)
            )
        )

        Column(Modifier.fillMaxSize().padding(horizontal = 28.dp, vertical = 18.dp)) {
            TopBar(
                onSettings = { showSettings = true },
                onRefresh = onRefresh,
                onAdd = { filePicker.launch(arrayOf("*/*")) },
            )
            Spacer(Modifier.weight(1f))

            if (games.isEmpty()) {
                EmptyLibrary(libraryPath) { filePicker.launch(arrayOf("*/*")) }
            } else {
                LazyRow(
                    state = listState,
                    horizontalArrangement = Arrangement.spacedBy(14.dp),
                    verticalAlignment = Alignment.Bottom,
                    contentPadding = PaddingValues(end = 400.dp),
                    modifier = Modifier.fillMaxWidth().height(124.dp),
                ) {
                    itemsIndexed(games, key = { _, g -> g.elfPath }) { index, game ->
                        GameTile(game, index == selected) {
                            if (index == selected) onGameSelected(game) else selected = index
                        }
                    }
                }
                Spacer(Modifier.height(18.dp))
                current?.let { GameDetails(it) { onGameSelected(it) } }
            }
            Spacer(Modifier.height(14.dp))
            HintBar(if (games.isEmpty()) listOf("Ⓨ" to "Ajustes") else listOf("◀ ▶" to "Elegir", "Ⓐ" to "Jugar", "Ⓨ" to "Ajustes"))
        }

        AnimatedVisibility(
            visible = showSettings,
            enter = fadeIn(),
            exit = fadeOut(),
        ) {
            Box(Modifier.fillMaxSize().background(Capy.Scrim).clickable { showSettings = false })
        }
        AnimatedVisibility(
            visible = showSettings,
            enter = slideInHorizontally { it },
            exit = slideOutHorizontally { it },
            modifier = Modifier.align(Alignment.CenterEnd),
        ) {
            SettingsPanel(settings, versionText, onClose = { showSettings = false })
        }
    }
}

@Composable
private fun TopBar(onSettings: () -> Unit, onRefresh: () -> Unit, onAdd: () -> Unit) {
    val time by produceState(initialValue = "") {
        val format = SimpleDateFormat("HH:mm", Locale.getDefault())
        while (true) {
            value = format.format(Date())
            delay(10_000)
        }
    }
    Row(verticalAlignment = Alignment.CenterVertically) {
        Image(
            painterResource(R.mipmap.ic_launcher), "CapyPS5",
            Modifier.size(30.dp).clip(RoundedCornerShape(8.dp)),
        )
        Spacer(Modifier.width(10.dp))
        Text("CapyPS5", style = MaterialTheme.typography.titleLarge)
        Spacer(Modifier.weight(1f))
        IconPill(Icons.Default.Add, "Añadir", onAdd)
        Spacer(Modifier.width(8.dp))
        IconPill(Icons.Default.Refresh, null, onRefresh)
        Spacer(Modifier.width(8.dp))
        IconPill(Icons.Default.Settings, null, onSettings)
        Spacer(Modifier.width(16.dp))
        Text(time, style = MaterialTheme.typography.titleMedium, color = Capy.TextSecondary)
    }
}

@Composable
private fun IconPill(icon: ImageVector, label: String?, onClick: () -> Unit) {
    Row(
        modifier = Modifier
            .clip(RoundedCornerShape(50))
            .background(Color(0x33FFFFFF))
            .clickable(onClick = onClick)
            .padding(horizontal = if (label != null) 12.dp else 8.dp, vertical = 7.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Icon(icon, label, Modifier.size(18.dp), tint = Capy.TextPrimary)
        if (label != null) {
            Spacer(Modifier.width(6.dp))
            Text(label, style = MaterialTheme.typography.labelLarge)
        }
    }
}

@Composable
private fun GameTile(game: GameItem, selected: Boolean, onClick: () -> Unit) {
    val size by animateDpAsState(if (selected) 120.dp else 92.dp, label = "tileSize")
    val icon by rememberImage(game.iconPath, 256)
    Box(
        modifier = Modifier
            .size(size)
            .clip(RoundedCornerShape(14.dp))
            .background(Capy.SurfaceHigh)
            .border(
                if (selected) 3.dp else 1.dp,
                if (selected) Color.White else Capy.Outline,
                RoundedCornerShape(14.dp),
            )
            .clickable(onClick = onClick),
        contentAlignment = Alignment.Center,
    ) {
        val bitmap = icon
        if (bitmap != null) {
            Image(bitmap, game.titleName, Modifier.fillMaxSize(), contentScale = ContentScale.Crop)
        } else {
            Column(horizontalAlignment = Alignment.CenterHorizontally, modifier = Modifier.padding(8.dp)) {
                Icon(Icons.Outlined.SportsEsports, null, Modifier.size(32.dp), tint = Capy.TextSecondary)
                Spacer(Modifier.height(4.dp))
                Text(
                    game.titleName, style = MaterialTheme.typography.labelSmall, color = Capy.TextSecondary,
                    maxLines = 2, overflow = TextOverflow.Ellipsis,
                )
            }
        }
    }
}

@Composable
private fun GameDetails(game: GameItem, onPlay: () -> Unit) {
    Column(Modifier.widthIn(max = 560.dp)) {
        Text(game.titleName, style = MaterialTheme.typography.displaySmall, maxLines = 1, overflow = TextOverflow.Ellipsis)
        Spacer(Modifier.height(4.dp))
        Text(
            listOfNotNull(game.titleId, game.version?.let { "v$it" }).joinToString("  ·  "),
            style = MaterialTheme.typography.bodyMedium,
            color = Capy.TextSecondary,
        )
        Spacer(Modifier.height(14.dp))
        Row(
            modifier = Modifier
                .clip(RoundedCornerShape(50))
                .background(Color.White)
                .clickable(onClick = onPlay)
                .padding(horizontal = 26.dp, vertical = 10.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            Icon(Icons.Default.PlayArrow, null, Modifier.size(20.dp), tint = Color.Black)
            Spacer(Modifier.width(6.dp))
            Text("Jugar", style = MaterialTheme.typography.titleMedium, color = Color.Black)
        }
    }
}

@Composable
private fun EmptyLibrary(libraryPath: String, onBrowse: () -> Unit) {
    Column(Modifier.widthIn(max = 520.dp), verticalArrangement = Arrangement.spacedBy(10.dp)) {
        Text("Tu biblioteca está vacía", style = MaterialTheme.typography.displaySmall)
        Text(
            "Copia cada juego como carpeta (con eboot.bin y sce_sys/) dentro de\n$libraryPath\ny pulsa actualizar, o abre un ejecutable suelto.",
            style = MaterialTheme.typography.bodyMedium,
            color = Capy.TextSecondary,
        )
        Row(
            modifier = Modifier
                .clip(RoundedCornerShape(50))
                .background(Color.White)
                .clickable(onClick = onBrowse)
                .padding(horizontal = 22.dp, vertical = 10.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            Icon(Icons.Default.Add, null, Modifier.size(18.dp), tint = Color.Black)
            Spacer(Modifier.width(6.dp))
            Text("Abrir ELF / EBOOT", style = MaterialTheme.typography.titleMedium, color = Color.Black)
        }
    }
}

@Composable
fun HintBar(hints: List<Pair<String, String>>, modifier: Modifier = Modifier) {
    Row(modifier, horizontalArrangement = Arrangement.spacedBy(18.dp)) {
        hints.forEach { (button, action) ->
            Row(verticalAlignment = Alignment.CenterVertically) {
                Text(button, style = MaterialTheme.typography.labelLarge, color = Capy.TextPrimary)
                Spacer(Modifier.width(6.dp))
                Text(action, style = MaterialTheme.typography.labelLarge, color = Capy.TextSecondary)
            }
        }
    }
}
