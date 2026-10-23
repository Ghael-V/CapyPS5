// SPDX-FileCopyrightText: Copyright 2026 CapyPS5 Project
// SPDX-License-Identifier: GPL-2.0-only

package org.capyps5.emulator.ui.screens

import android.net.Uri
import android.os.Environment
import android.provider.DocumentsContract
import android.view.InputDevice
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.animation.AnimatedVisibility
import androidx.compose.animation.Crossfade
import androidx.compose.animation.core.animateDpAsState
import androidx.compose.animation.fadeIn
import androidx.compose.animation.fadeOut
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
import androidx.compose.foundation.layout.aspectRatio
import androidx.compose.foundation.layout.fillMaxHeight
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.lazy.LazyRow
import androidx.compose.foundation.lazy.grid.GridCells
import androidx.compose.foundation.lazy.grid.LazyVerticalGrid
import androidx.compose.foundation.lazy.grid.itemsIndexed
import androidx.compose.foundation.lazy.grid.rememberLazyGridState
import androidx.compose.foundation.lazy.itemsIndexed
import androidx.compose.foundation.lazy.rememberLazyListState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Add
import androidx.compose.material.icons.filled.EmojiEvents
import androidx.compose.material.icons.filled.MoreHoriz
import androidx.compose.material.icons.filled.PlayArrow
import androidx.compose.material.icons.filled.Refresh
import androidx.compose.material.icons.outlined.Settings
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
import androidx.compose.ui.text.SpanStyle
import androidx.compose.ui.text.buildAnnotatedString
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.withStyle
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import kotlinx.coroutines.delay
import org.capyps5.emulator.CoreOption
import org.capyps5.emulator.R
import org.capyps5.emulator.Settings
import org.capyps5.emulator.model.GameItem
import org.capyps5.emulator.ui.NavKey
import org.capyps5.emulator.ui.OnNavKey
import org.capyps5.emulator.ui.rememberImage
import org.capyps5.emulator.Trophies
import org.capyps5.emulator.ui.theme.Capy
import androidx.compose.ui.res.stringResource
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

private const val GridColumns = 6

private fun hasGamepad(): Boolean = InputDevice.getDeviceIds().any { id ->
    val device = InputDevice.getDevice(id) ?: return@any false
    !device.isVirtual && (device.sources and InputDevice.SOURCE_GAMEPAD) == InputDevice.SOURCE_GAMEPAD
}

/** Launcher home: a row of games ("Juegos") or the whole library as a grid ("Biblioteca"). */
@Composable
fun HomeScreen(
    games: List<GameItem>,
    libraryPath: String,
    settings: Settings,
    versionText: String,
    onGameSelected: (GameItem) -> Unit,
    onAddGameFile: (String) -> Unit,
    onRefresh: () -> Unit,
    onLibraryPicked: (String) -> Unit,
    onClearShaderCache: (titleId: String?) -> String,
    onSaveLog: () -> String?,
    overlayActive: Boolean = false,
) {
    var tab by remember { mutableIntStateOf(0) }
    var selected by remember { mutableIntStateOf(0) }
    var showSettings by remember { mutableStateOf(false) }
    var optionsFor by remember { mutableStateOf<GameItem?>(null) }
    var trophiesFor by remember { mutableStateOf<GameItem?>(null) }
    val current = games.getOrNull(selected.coerceIn(0, (games.size - 1).coerceAtLeast(0)))

    val filePicker = rememberLauncherForActivityResult(ActivityResultContracts.OpenDocument()) { uri: Uri? ->
        uri?.let { onAddGameFile(it.toString()) }
    }
    val context = androidx.compose.ui.platform.LocalContext.current
    var driverMessage by remember { mutableStateOf<String?>(null) }
    val driverPicker = rememberLauncherForActivityResult(ActivityResultContracts.OpenDocument()) { uri: Uri? ->
        uri?.let {
            driverMessage = org.capyps5.emulator.GpuDrivers.import(context, it).fold(
                onSuccess = { driver ->
                    settings.updateGpuDriver(driver.id)
                    context.getString(R.string.gpu_driver_installed, driver.label)
                },
                onFailure = { error -> context.getString(R.string.gpu_driver_import_failed, error.message.orEmpty()) },
            )
        }
    }
    val folderPicker = rememberLauncherForActivityResult(ActivityResultContracts.OpenDocumentTree()) { uri: Uri? ->
        uri?.let(::treeUriToPath)?.let {
            selected = 0
            onLibraryPicked(it)
        }
    }

    OnNavKey(enabled = !showSettings && optionsFor == null && trophiesFor == null && !overlayActive) { key ->
        val count = games.size
        when (key) {
            NavKey.TabLeft -> tab = 0
            NavKey.TabRight -> tab = 1
            NavKey.Left -> if (count > 0) selected = (selected - 1).coerceAtLeast(0)
            NavKey.Right -> if (count > 0) selected = (selected + 1).coerceAtMost(count - 1)
            NavKey.Up -> if (tab == 1 && count > 0) selected = (selected - GridColumns).coerceAtLeast(0)
            NavKey.Down -> if (tab == 1 && count > 0) selected = (selected + GridColumns).coerceAtMost(count - 1)
            NavKey.Confirm -> current?.let(onGameSelected)
            NavKey.Secondary -> optionsFor = current
            NavKey.Menu -> showSettings = true
            NavKey.Back -> if (tab == 1) tab = 0
        }
    }

    Box(Modifier.fillMaxSize().background(Capy.Background)) {
        // Key art of the selected game, faded towards the left so text stays readable.
        val art by rememberImage(current?.backgroundPath, 1280)
        Crossfade(targetState = art, label = "background") { bitmap ->
            if (bitmap != null) {
                Image(bitmap, null, Modifier.fillMaxSize(), contentScale = ContentScale.Crop, alpha = 0.6f)
            } else {
                Image(
                    painterResource(R.drawable.capy_bg), null, Modifier.fillMaxSize(),
                    contentScale = ContentScale.Crop, alpha = 0.35f,
                )
            }
        }
        Box(Modifier.fillMaxSize().background(Brush.horizontalGradient(0f to Color(0xEE07090E), 0.65f to Color(0x2207090E))))
        Box(Modifier.fillMaxSize().background(Brush.verticalGradient(0.55f to Color.Transparent, 1f to Capy.Background)))

        Column(Modifier.fillMaxSize().padding(horizontal = 32.dp, vertical = 18.dp)) {
            TopBar(
                tab = tab,
                onTab = { tab = it },
                onAdd = { filePicker.launch(arrayOf("*/*")) },
                onRefresh = onRefresh,
                onSettings = { showSettings = true },
            )
            Spacer(Modifier.height(18.dp))
            when {
                games.isEmpty() -> {
                    Spacer(Modifier.weight(1f))
                    EmptyLibrary(libraryPath, onAddFolder = { folderPicker.launch(null) }, onBrowse = { filePicker.launch(arrayOf("*/*")) })
                    Spacer(Modifier.weight(1f))
                }
                tab == 0 -> {
                    GameRow(games, selected, onSelect = { i -> if (i == selected) onGameSelected(games[i]) else selected = i })
                    Spacer(Modifier.weight(1f))
                    current?.let {
                        GameDetails(it, onPlay = { onGameSelected(it) }, onOptions = { optionsFor = it }, onTrophies = { trophiesFor = it })
                    }
                }
                else -> {
                    GameGrid(
                        games, selected,
                        onSelect = { i -> if (i == selected) onGameSelected(games[i]) else selected = i },
                        modifier = Modifier.weight(1f),
                    )
                }
            }
            Spacer(Modifier.height(12.dp))
            HintBar(
                if (games.isEmpty()) listOf("Start" to stringResource(R.string.settings))
                else listOf(
                    "Ⓐ" to stringResource(R.string.play),
                    "Ⓨ" to stringResource(R.string.game_options),
                    "Start" to stringResource(R.string.settings),
                    "L1 / R1" to stringResource(R.string.tabs),
                )
            )
        }

        optionsFor?.let { game ->
            GameOptionsPanel(
                game = game,
                settings = settings,
                onPlay = { optionsFor = null; onGameSelected(game) },
                onClearShaderCache = { onClearShaderCache(game.titleId) },
                onTrophies = { optionsFor = null; trophiesFor = game },
                onClose = { optionsFor = null },
            )
        }

        trophiesFor?.let { game -> TrophyScreen(game, onClose = { trophiesFor = null }) }

        AnimatedVisibility(visible = showSettings, enter = fadeIn(), exit = fadeOut()) {
            SettingsScreen(
                settings = settings,
                versionText = versionText,
                onAddFolder = { folderPicker.launch(null) },
                onImportDriver = { driverPicker.launch(arrayOf("application/zip", "application/octet-stream", "*/*")) },
                driverMessage = driverMessage,
                onClearShaderCache = { onClearShaderCache(null) },
                onSaveLog = onSaveLog,
                onClose = { showSettings = false },
            )
        }
    }
}

/**
 * The core opens games by file path (the app holds all-files access), so a folder picked through
 * the system picker is turned back into its path: "primary:Games" is internal storage, any other
 * volume id ("1234-ABCD:Games") is an SD card or USB drive under /storage.
 */
private fun treeUriToPath(uri: Uri): String? {
    val docId = runCatching { DocumentsContract.getTreeDocumentId(uri) }.getOrNull() ?: return null
    val volume = docId.substringBefore(':')
    val relative = docId.substringAfter(':', "")
    val root = if (volume.equals("primary", ignoreCase = true)) {
        Environment.getExternalStorageDirectory().absolutePath
    } else {
        "/storage/$volume"
    }
    return if (relative.isEmpty()) root else "$root/$relative"
}

@Composable
private fun TopBar(tab: Int, onTab: (Int) -> Unit, onAdd: () -> Unit, onRefresh: () -> Unit, onSettings: () -> Unit) {
    val time by produceState(initialValue = "") {
        val format = SimpleDateFormat("HH:mm", Locale.getDefault())
        while (true) {
            value = format.format(Date())
            delay(10_000)
        }
    }
    val gamepad by produceState(initialValue = hasGamepad()) {
        while (true) {
            value = hasGamepad()
            delay(2_000)
        }
    }
    Row(verticalAlignment = Alignment.CenterVertically) {
        // Brand first: screenshots and videos of the launcher should say what they show.
        Image(painterResource(R.drawable.capy_logo), null, Modifier.size(36.dp))
        Text(
            buildAnnotatedString {
                withStyle(SpanStyle(color = Capy.TextPrimary)) { append("Capy") }
                withStyle(SpanStyle(color = Capy.Accent)) { append("PS5") }
            },
            style = MaterialTheme.typography.titleLarge.copy(fontWeight = FontWeight.Bold),
            modifier = Modifier.padding(start = 8.dp, end = 30.dp),
        )
        listOf(stringResource(R.string.tab_games), stringResource(R.string.tab_library)).forEachIndexed { i, label ->
            Text(
                label,
                style = MaterialTheme.typography.titleLarge,
                color = if (i == tab) Capy.TextPrimary else Capy.TextMuted,
                modifier = Modifier.clickable { onTab(i) }.padding(end = 26.dp),
            )
        }
        Spacer(Modifier.weight(1f))
        TopIcon(Icons.Filled.Add, onAdd)
        TopIcon(Icons.Filled.Refresh, onRefresh)
        TopIcon(Icons.Outlined.Settings, onSettings)
        Icon(
            Icons.Outlined.SportsEsports, null, Modifier.padding(horizontal = 10.dp).size(22.dp),
            tint = if (gamepad) Capy.TextPrimary else Capy.TextMuted,
        )
        Text(time, style = MaterialTheme.typography.titleMedium, color = Capy.TextSecondary, modifier = Modifier.padding(start = 6.dp))
    }
}

@Composable
private fun TopIcon(icon: ImageVector, onClick: () -> Unit) {
    Box(
        Modifier.clip(RoundedCornerShape(50)).clickable(onClick = onClick).padding(10.dp),
        contentAlignment = Alignment.Center,
    ) {
        Icon(icon, null, Modifier.size(22.dp), tint = Capy.TextPrimary)
    }
}

@Composable
private fun GameRow(games: List<GameItem>, selected: Int, onSelect: (Int) -> Unit) {
    val state = rememberLazyListState()
    // The selection moves along the row up to the third slot; only then does the row scroll, so
    // the games before and after the selected one stay in view.
    LaunchedEffect(selected) {
        if (games.isNotEmpty()) state.animateScrollToItem((selected - 2).coerceIn(0, games.size - 1))
    }
    Column {
        LazyRow(
            state = state,
            horizontalArrangement = Arrangement.spacedBy(14.dp),
            verticalAlignment = Alignment.Top,
            contentPadding = PaddingValues(end = 480.dp),
            modifier = Modifier.fillMaxWidth().height(132.dp),
        ) {
            itemsIndexed(games, key = { _, g -> g.elfPath }) { index, game ->
                val size by animateDpAsState(if (index == selected) 128.dp else 88.dp, label = "tileSize")
                GameTile(game, index == selected, Modifier.size(size)) { onSelect(index) }
            }
        }
        games.getOrNull(selected)?.let {
            Text(it.titleName, style = MaterialTheme.typography.titleMedium, maxLines = 1, overflow = TextOverflow.Ellipsis)
        }
    }
}

@Composable
private fun GameGrid(games: List<GameItem>, selected: Int, onSelect: (Int) -> Unit, modifier: Modifier) {
    val state = rememberLazyGridState()
    LaunchedEffect(selected) { if (games.isNotEmpty()) state.animateScrollToItem(selected.coerceIn(0, games.size - 1)) }
    LazyVerticalGrid(
        columns = GridCells.Fixed(GridColumns),
        state = state,
        horizontalArrangement = Arrangement.spacedBy(16.dp),
        verticalArrangement = Arrangement.spacedBy(16.dp),
        modifier = modifier,
    ) {
        itemsIndexed(games, key = { _, g -> g.elfPath }) { index, game ->
            Column {
                GameTile(game, index == selected, Modifier.fillMaxWidth().aspectRatio(1f)) { onSelect(index) }
                Text(
                    game.titleName,
                    style = MaterialTheme.typography.bodySmall,
                    color = if (index == selected) Capy.TextPrimary else Capy.TextSecondary,
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis,
                    modifier = Modifier.padding(top = 6.dp),
                )
            }
        }
    }
}

@Composable
private fun GameTile(game: GameItem, selected: Boolean, modifier: Modifier, onClick: () -> Unit) {
    val icon by rememberImage(game.iconPath, 256)
    val shape = RoundedCornerShape(14.dp)
    Box(
        modifier = modifier
            .clip(shape)
            .background(Capy.SurfaceHigh)
            .border(if (selected) 3.dp else 1.dp, if (selected) Color.White else Capy.Outline, shape)
            .clickable(onClick = onClick),
        contentAlignment = Alignment.Center,
    ) {
        val bitmap = icon
        if (bitmap != null) {
            Image(bitmap, game.titleName, Modifier.fillMaxSize(), contentScale = ContentScale.Crop)
        } else {
            Column(horizontalAlignment = Alignment.CenterHorizontally, modifier = Modifier.padding(8.dp)) {
                Icon(Icons.Outlined.SportsEsports, null, Modifier.size(30.dp), tint = Capy.TextSecondary)
                Text(
                    game.titleName, style = MaterialTheme.typography.labelSmall, color = Capy.TextSecondary,
                    maxLines = 2, overflow = TextOverflow.Ellipsis,
                )
            }
        }
    }
}

@Composable
private fun GameDetails(game: GameItem, onPlay: () -> Unit, onOptions: () -> Unit, onTrophies: () -> Unit) {
    Column(Modifier.widthIn(max = 560.dp)) {
        Text(game.titleName, style = MaterialTheme.typography.displaySmall, maxLines = 1, overflow = TextOverflow.Ellipsis)
        Spacer(Modifier.height(4.dp))
        Text(
            listOfNotNull(game.titleId, game.version?.let { "v$it" }, "PS4".takeIf { game.isPs4 }).joinToString("  ·  "),
            style = MaterialTheme.typography.bodyMedium,
            color = Capy.TextSecondary,
        )
        Spacer(Modifier.height(14.dp))
        Row(horizontalArrangement = Arrangement.spacedBy(10.dp), verticalAlignment = Alignment.CenterVertically) {
            Row(
                modifier = Modifier
                    .clip(RoundedCornerShape(50))
                    .background(Color.White)
                    .clickable(onClick = onPlay)
                    .padding(horizontal = 30.dp, vertical = 11.dp),
                verticalAlignment = Alignment.CenterVertically,
            ) {
                Icon(Icons.Filled.PlayArrow, null, Modifier.size(20.dp), tint = Color.Black)
                Spacer(Modifier.width(6.dp))
                Text(stringResource(R.string.play), style = MaterialTheme.typography.titleMedium, color = Color.Black)
            }
            Box(
                Modifier.clip(RoundedCornerShape(50)).background(Color(0x33FFFFFF)).clickable(onClick = onOptions).padding(11.dp),
            ) {
                Icon(Icons.Filled.MoreHoriz, stringResource(R.string.game_options), Modifier.size(20.dp), tint = Capy.TextPrimary)
            }
            if (remember(game.directoryPath) { Trophies.hasTrophies(game.directoryPath) }) {
                Box(
                    Modifier.clip(RoundedCornerShape(50)).background(Color(0x33FFFFFF)).clickable(onClick = onTrophies).padding(11.dp),
                ) {
                    Icon(Icons.Filled.EmojiEvents, stringResource(R.string.trophies), Modifier.size(20.dp), tint = Capy.TextPrimary)
                }
            }
        }
    }
}

/** Per-game overrides of the core options, plus game actions. */
@Composable
private fun GameOptionsPanel(
    game: GameItem,
    settings: Settings,
    onPlay: () -> Unit,
    onClearShaderCache: () -> String,
    onTrophies: () -> Unit,
    onClose: () -> Unit,
) {
    var message by remember { mutableStateOf<String?>(null) }
    var row by remember { mutableIntStateOf(0) }
    // Read so the rows recompose when a value changes.
    val optionsVersion = settings.coreOptionsVersion
    val context = androidx.compose.ui.platform.LocalContext.current
    val items = remember(optionsVersion, message) {
        val drivers = org.capyps5.emulator.GpuDrivers.list(context)
        // null = follow the global driver, "" = system driver.
        val driverChoices: List<String?> = listOf<String?>(null, "") + drivers.map { it.id }
        val driverOverride = settings.gameGpuDriver(game.titleId)?.takeIf { id -> id in driverChoices }
        val driverName = { id: String -> drivers.firstOrNull { it.id == id }?.label ?: context.getString(R.string.gpu_driver_system) }
        val driverItem = SettingItem(
            context.getString(R.string.gpu_driver),
            context.getString(R.string.gpu_driver_game_desc),
            driverOverride?.let(driverName) ?: context.getString(R.string.global_value, driverName(settings.gpuDriver())),
            onStep = { settings.updateGameGpuDriver(game.titleId, cycle(driverChoices, driverOverride, it)) },
        )
        listOf(driverItem) + CoreOption.entries.map { option ->
            // null = follow the global value.
            val choices: List<Long?> = listOf<Long?>(null) + option.choices.map { it.first }
            val override = settings.gameOption(game.titleId, option)
            SettingItem(
                option.title(context),
                option.description(context),
                override?.let { option.label(context, it) }
                    ?: if (option == CoreOption.ResolutionScale && game.isUnreal) {
                        context.getString(R.string.ue_value, option.label(context, settings.ueResolution()))
                    } else {
                        context.getString(R.string.global_value, option.label(context, settings.coreOption(option)))
                    },
                onStep = { settings.updateGameOption(game.titleId, option, cycle(choices, override, it)) },
            )
        } + listOfNotNull(
            if (Trophies.hasTrophies(game.directoryPath)) {
                SettingItem(context.getString(R.string.trophies), context.getString(R.string.trophies_desc), onConfirm = onTrophies)
            } else null,
            SettingItem(
                context.getString(R.string.clear_game_shader_cache), context.getString(R.string.clear_shader_cache_desc),
                danger = true,
                onConfirm = { message = onClearShaderCache() },
            ),
            SettingItem(context.getString(R.string.play), null, onConfirm = onPlay),
        ) + listOfNotNull(message?.let { SettingItem(it) })
    }
    if (row >= items.size) row = items.size - 1

    OnNavKey { key ->
        val item = items.getOrNull(row)
        when (key) {
            NavKey.Up -> row = (row - 1).mod(items.size)
            NavKey.Down -> row = (row + 1).mod(items.size)
            NavKey.Left -> item?.onStep?.invoke(-1)
            NavKey.Right -> item?.onStep?.invoke(1)
            NavKey.Confirm -> item?.onConfirm?.invoke() ?: item?.onStep?.invoke(1)
            NavKey.Back, NavKey.Secondary, NavKey.Menu -> onClose()
            else -> Unit
        }
    }
    Box(Modifier.fillMaxSize().background(Capy.Scrim).clickable(onClick = onClose), contentAlignment = Alignment.CenterEnd) {
        Column(
            Modifier
                .width(460.dp)
                .fillMaxHeight()
                .background(Capy.Surface)
                .clickable(enabled = false) {}
                .padding(24.dp),
            verticalArrangement = Arrangement.spacedBy(8.dp),
        ) {
            Text(game.titleName, style = MaterialTheme.typography.titleLarge, maxLines = 1, overflow = TextOverflow.Ellipsis)
            Text(
                stringResource(R.string.game_options_of, game.titleId),
                style = MaterialTheme.typography.bodySmall,
                color = Capy.TextSecondary,
            )
            Spacer(Modifier.height(8.dp))
            items.forEachIndexed { i, item -> SettingRowView(item, focused = i == row) { row = i } }
            Spacer(Modifier.weight(1f))
            HintBar(
                listOf(
                    "◀ ▶" to stringResource(R.string.hint_change),
                    "Ⓐ" to stringResource(R.string.hint_choose),
                    "Ⓑ" to stringResource(R.string.hint_back),
                ),
            )
        }
    }
}

@Composable
private fun EmptyLibrary(libraryPath: String, onAddFolder: () -> Unit, onBrowse: () -> Unit) {
    Column(Modifier.widthIn(max = 560.dp), verticalArrangement = Arrangement.spacedBy(12.dp)) {
        Text(stringResource(R.string.library_empty), style = MaterialTheme.typography.displaySmall)
        Text(
            stringResource(R.string.library_empty_desc, libraryPath),
            style = MaterialTheme.typography.bodyMedium,
            color = Capy.TextSecondary,
        )
        Row(horizontalArrangement = Arrangement.spacedBy(10.dp)) {
            PillButton(stringResource(R.string.folder_add), Icons.Filled.Add, primary = true, onClick = onAddFolder)
            PillButton(stringResource(R.string.open_executable), null, primary = false, onClick = onBrowse)
        }
    }
}

@Composable
private fun PillButton(label: String, icon: ImageVector?, primary: Boolean, onClick: () -> Unit) {
    Row(
        modifier = Modifier
            .clip(RoundedCornerShape(50))
            .background(if (primary) Color.White else Color(0x33FFFFFF))
            .clickable(onClick = onClick)
            .padding(horizontal = 22.dp, vertical = 10.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        icon?.let { Icon(it, null, Modifier.size(18.dp), tint = if (primary) Color.Black else Capy.TextPrimary) }
        if (icon != null) Spacer(Modifier.width(6.dp))
        Text(label, style = MaterialTheme.typography.titleMedium, color = if (primary) Color.Black else Capy.TextPrimary)
    }
}

@Composable
fun HintBar(hints: List<Pair<String, String>>, modifier: Modifier = Modifier) {
    Row(modifier, horizontalArrangement = Arrangement.spacedBy(20.dp)) {
        hints.forEach { (button, action) ->
            Row(verticalAlignment = Alignment.CenterVertically) {
                Text(button, style = MaterialTheme.typography.labelLarge, color = Capy.TextPrimary)
                Spacer(Modifier.width(6.dp))
                Text(action, style = MaterialTheme.typography.labelLarge, color = Capy.TextSecondary)
            }
        }
    }
}
