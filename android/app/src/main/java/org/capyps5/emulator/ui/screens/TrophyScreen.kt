// SPDX-FileCopyrightText: Copyright 2026 CapyPS5 Project
// SPDX-License-Identifier: GPL-2.0-only

package org.capyps5.emulator.ui.screens

import android.graphics.BitmapFactory
import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxHeight
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.itemsIndexed
import androidx.compose.foundation.lazy.rememberLazyListState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Share
import androidx.compose.material3.Icon
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.produceState
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.alpha
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.ColorFilter
import androidx.compose.ui.graphics.ColorMatrix
import androidx.compose.ui.graphics.ImageBitmap
import androidx.compose.ui.graphics.asImageBitmap
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import org.capyps5.emulator.Grade
import org.capyps5.emulator.R
import org.capyps5.emulator.Trophies
import org.capyps5.emulator.Trophy
import org.capyps5.emulator.TrophyCard
import org.capyps5.emulator.TrophySet
import org.capyps5.emulator.model.GameItem
import org.capyps5.emulator.ui.NavKey
import org.capyps5.emulator.ui.OnNavKey
import org.capyps5.emulator.ui.theme.Capy
import java.text.DateFormat
import java.util.Date

private fun Grade.color(): Color = when (this) {
    Grade.Platinum -> Color(0xFFB8D4E8)
    Grade.Gold -> Color(0xFFE3B54A)
    Grade.Silver -> Color(0xFFB4BAC4)
    Grade.Bronze -> Color(0xFFC08457)
}

@Composable
private fun Grade.label(): String = stringResource(
    when (this) {
        Grade.Platinum -> R.string.trophy_platinum
        Grade.Gold -> R.string.trophy_gold
        Grade.Silver -> R.string.trophy_silver
        Grade.Bronze -> R.string.trophy_bronze
    },
)

/** The trophy list a game ships, with what has been earned on this device. */
@Composable
fun TrophyScreen(game: GameItem, onClose: () -> Unit) {
    val context = LocalContext.current
    val set by produceState<Result<TrophySet?>?>(null, game.directoryPath) {
        value = withContext(Dispatchers.IO) { runCatching { Trophies.load(context, game.directoryPath, game.titleId) } }
    }
    var row by remember { mutableIntStateOf(0) }
    val listState = rememberLazyListState()
    // Earned first, newest on top; then the rest in the game's order.
    val trophies = remember(set) {
        set?.getOrNull()?.trophies.orEmpty()
            .sortedWith(compareBy<Trophy>({ !it.unlocked }, { -(it.unlockedAt ?: 0L) }, { it.id }))
    }
    LaunchedEffect(row) { if (trophies.isNotEmpty()) listState.animateScrollToItem(row) }

    // Rendering the card reads the trophy icons; keep it off the main thread.
    val scope = rememberCoroutineScope()
    var sharing by remember { mutableStateOf(false) }
    fun share() {
        val loaded = set?.getOrNull() ?: return
        if (sharing) return
        sharing = true
        scope.launch {
            withContext(Dispatchers.IO) { runCatching { TrophyCard.share(context, game, loaded) } }
            sharing = false
        }
    }

    OnNavKey { key ->
        when (key) {
            NavKey.Up -> row = (row - 1).coerceAtLeast(0)
            NavKey.Down -> row = (row + 1).coerceAtMost((trophies.size - 1).coerceAtLeast(0))
            NavKey.TabLeft -> row = (row - 8).coerceAtLeast(0)
            NavKey.TabRight -> row = (row + 8).coerceAtMost((trophies.size - 1).coerceAtLeast(0))
            NavKey.Secondary -> share()
            NavKey.Back, NavKey.Menu -> onClose()
            else -> Unit
        }
    }

    Row(Modifier.fillMaxSize().background(Capy.Background).clickable(enabled = false) {}) {
        Column(
            Modifier.width(320.dp).fillMaxHeight().background(Capy.Surface).padding(vertical = 22.dp),
        ) {
            ScreenHeader(stringResource(R.string.trophies), onBack = onClose)
            Column(Modifier.padding(horizontal = 24.dp), verticalArrangement = Arrangement.spacedBy(10.dp)) {
                Text(
                    set?.getOrNull()?.title?.ifEmpty { null } ?: game.titleName,
                    style = MaterialTheme.typography.titleMedium,
                    maxLines = 2,
                    overflow = TextOverflow.Ellipsis,
                )
                set?.getOrNull()?.let { loaded ->
                    Text(
                        stringResource(R.string.trophies_progress, loaded.earned, loaded.trophies.size, loaded.percent),
                        style = MaterialTheme.typography.bodyMedium,
                        color = Capy.TextSecondary,
                    )
                    LinearProgressIndicator(
                        progress = { loaded.percent / 100f },
                        modifier = Modifier.fillMaxWidth().height(6.dp).clip(RoundedCornerShape(3.dp)),
                        color = Capy.Accent,
                        trackColor = Capy.SurfaceHigh,
                    )
                    Spacer(Modifier.height(6.dp))
                    Grade.entries.forEach { grade ->
                        val all = loaded.trophies.count { it.grade == grade }
                        if (all == 0) return@forEach
                        val earned = loaded.trophies.count { it.grade == grade && it.unlocked }
                        Row(verticalAlignment = Alignment.CenterVertically) {
                            Box(Modifier.size(10.dp).background(grade.color(), CircleShape))
                            Spacer(Modifier.width(10.dp))
                            Text(grade.label(), style = MaterialTheme.typography.bodyMedium, modifier = Modifier.weight(1f))
                            Text("$earned / $all", style = MaterialTheme.typography.bodyMedium, color = Capy.TextSecondary)
                        }
                    }
                    Spacer(Modifier.height(10.dp))
                    Row(
                        Modifier
                            .clip(RoundedCornerShape(50))
                            .background(Color.White)
                            .clickable { share() }
                            .padding(horizontal = 20.dp, vertical = 9.dp),
                        verticalAlignment = Alignment.CenterVertically,
                    ) {
                        Icon(Icons.Filled.Share, null, Modifier.size(18.dp), tint = Color.Black)
                        Spacer(Modifier.width(8.dp))
                        Text(stringResource(R.string.trophies_share), style = MaterialTheme.typography.titleSmall, color = Color.Black)
                    }
                }
            }
            Spacer(Modifier.weight(1f))
            HintBar(
                listOf(
                    "Ⓨ" to stringResource(R.string.trophies_share),
                    "Ⓑ" to stringResource(R.string.hint_back),
                ),
                Modifier.padding(horizontal = 24.dp),
            )
        }
        Column(Modifier.weight(1f).fillMaxHeight().padding(horizontal = 28.dp, vertical = 22.dp)) {
            when {
                set == null -> Text(stringResource(R.string.trophies_loading), color = Capy.TextSecondary)
                set?.getOrNull() == null -> Text(stringResource(R.string.trophies_none), color = Capy.TextSecondary)
                else -> LazyColumn(state = listState, verticalArrangement = Arrangement.spacedBy(6.dp)) {
                    itemsIndexed(trophies, key = { _, t -> t.id }) { i, trophy ->
                        TrophyRow(set!!.getOrNull()!!, trophy, focused = i == row) { row = i }
                    }
                }
            }
        }
    }
}

@Composable
private fun TrophyRow(set: TrophySet, trophy: Trophy, focused: Boolean, onClick: () -> Unit) {
    val secret = trophy.hidden && !trophy.unlocked
    val icon by produceState<ImageBitmap?>(null, trophy.id, secret) {
        if (!secret) value = withContext(Dispatchers.IO) {
            Trophies.readIcon(set, trophy)?.let { BitmapFactory.decodeByteArray(it, 0, it.size)?.asImageBitmap() }
        }
    }
    Row(
        Modifier
            .fillMaxWidth()
            .background(if (focused) Capy.AccentSoft else Capy.Surface, RoundedCornerShape(10.dp))
            .clickable(onClick = onClick)
            .padding(horizontal = 14.dp, vertical = 10.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Box(Modifier.size(56.dp).clip(RoundedCornerShape(8.dp)).background(Capy.SurfaceHigh), contentAlignment = Alignment.Center) {
            icon?.let {
                Image(
                    it, null, Modifier.fillMaxSize().alpha(if (trophy.unlocked) 1f else 0.45f),
                    colorFilter = if (trophy.unlocked) null else ColorFilter.colorMatrix(ColorMatrix().apply { setToSaturation(0f) }),
                )
            } ?: Text("?", style = MaterialTheme.typography.titleLarge, color = Capy.TextMuted)
        }
        Spacer(Modifier.width(14.dp))
        Column(Modifier.weight(1f)) {
            Text(
                if (secret) stringResource(R.string.trophy_hidden) else trophy.name,
                style = MaterialTheme.typography.titleSmall,
                color = if (trophy.unlocked) Capy.TextPrimary else Capy.TextSecondary,
                maxLines = 1,
                overflow = TextOverflow.Ellipsis,
            )
            Text(
                if (secret) stringResource(R.string.trophy_hidden_desc) else trophy.detail,
                style = MaterialTheme.typography.bodySmall,
                color = Capy.TextSecondary,
                maxLines = 2,
                overflow = TextOverflow.Ellipsis,
            )
            trophy.unlockedAt?.takeIf { it > 0 }?.let {
                Text(
                    stringResource(R.string.trophy_unlocked_on, DateFormat.getDateTimeInstance(DateFormat.MEDIUM, DateFormat.SHORT).format(Date(it))),
                    style = MaterialTheme.typography.bodySmall,
                    color = Capy.Good,
                )
            }
        }
        Spacer(Modifier.width(12.dp))
        Column(horizontalAlignment = Alignment.End) {
            Box(Modifier.size(12.dp).background(trophy.grade.color(), CircleShape))
            Spacer(Modifier.height(4.dp))
            Text(trophy.grade.label(), style = MaterialTheme.typography.labelSmall, color = Capy.TextMuted)
        }
    }
}
