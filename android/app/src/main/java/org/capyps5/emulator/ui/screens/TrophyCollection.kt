// SPDX-FileCopyrightText: Copyright 2026 CapyPS5 Project
// SPDX-License-Identifier: GPL-2.0-only

package org.capyps5.emulator.ui.screens

import android.content.Context
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
import androidx.compose.material.icons.outlined.SportsEsports
import androidx.compose.material3.Icon
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.produceState
import androidx.compose.runtime.remember
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.ImageBitmap
import androidx.compose.ui.graphics.asImageBitmap
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import org.capyps5.emulator.Grade
import org.capyps5.emulator.R
import org.capyps5.emulator.Trophies
import org.capyps5.emulator.Trophy
import org.capyps5.emulator.TrophySet
import org.capyps5.emulator.model.GameItem
import org.capyps5.emulator.ui.rememberImage
import org.capyps5.emulator.ui.theme.Capy

/** One library game with its trophy list. */
data class GameTrophies(val game: GameItem, val set: TrophySet) {
    /** Epoch ms of the newest unlock, 0 when nothing (or nothing timed) has been earned. */
    val lastUnlock: Long = set.trophies.maxOfOrNull { it.unlockedAt ?: 0L } ?: 0L
}

/**
 * The trophy lists of every library game that ships one, most recent unlock first and then by
 * name. Each title id counts once, since unlocks are kept per title id.
 */
fun loadTrophyCollection(context: Context, games: List<GameItem>): List<GameTrophies> =
    games.distinctBy { it.titleId }
        .filter { Trophies.hasTrophies(it.directoryPath) }
        .mapNotNull { game ->
            runCatching { Trophies.load(context, game.directoryPath, game.titleId) }.getOrNull()
                ?.takeIf { it.trophies.isNotEmpty() }
                ?.let { GameTrophies(game, it) }
        }
        .sortedWith(compareByDescending<GameTrophies> { it.lastUnlock }.thenBy { it.game.titleName.lowercase() })

private const val RecentCount = 4

/** All trophies of the library: totals by grade, the latest earned, and every game's progress. */
@Composable
fun TrophyCollection(
    collection: List<GameTrophies>?,
    focused: Int,
    onFocus: (Int) -> Unit,
    onOpen: (GameItem) -> Unit,
    modifier: Modifier = Modifier,
) {
    when {
        collection == null -> Text(stringResource(R.string.trophies_loading), color = Capy.TextSecondary, modifier = modifier)
        collection.isEmpty() -> Text(stringResource(R.string.trophies_collection_none), color = Capy.TextSecondary, modifier = modifier)
        else -> Row(modifier, horizontalArrangement = Arrangement.spacedBy(20.dp)) {
            CollectionSummary(collection, Modifier.width(320.dp).fillMaxHeight())
            val listState = rememberLazyListState()
            val row = focused.coerceIn(0, collection.size - 1)
            LaunchedEffect(row) { listState.animateScrollToItem((row - 1).coerceAtLeast(0)) }
            LazyColumn(Modifier.weight(1f).fillMaxHeight(), state = listState, verticalArrangement = Arrangement.spacedBy(8.dp)) {
                itemsIndexed(collection, key = { _, g -> g.game.titleId }) { i, entry ->
                    GameTrophyRow(entry, focused = i == row) { if (i == row) onOpen(entry.game) else onFocus(i) }
                }
            }
        }
    }
}

@Composable
private fun CollectionSummary(collection: List<GameTrophies>, modifier: Modifier) {
    val all = remember(collection) { collection.flatMap { it.set.trophies } }
    val earned = all.count { it.unlocked }
    val percent = if (all.isEmpty()) 0 else earned * 100 / all.size
    // The newest timed unlocks of any game, each with the list its icon lives in.
    val recent = remember(collection) {
        collection.flatMap { entry -> entry.set.trophies.filter { (it.unlockedAt ?: 0L) > 0 }.map { entry to it } }
            .sortedByDescending { it.second.unlockedAt }
            .take(RecentCount)
    }
    Column(
        modifier.background(Capy.Surface.copy(alpha = 0.92f), RoundedCornerShape(14.dp)).padding(20.dp),
        verticalArrangement = Arrangement.spacedBy(10.dp),
    ) {
        Text(stringResource(R.string.trophies_collection), style = MaterialTheme.typography.titleLarge)
        Text(
            stringResource(R.string.trophies_progress, earned, all.size, percent),
            style = MaterialTheme.typography.bodyMedium,
            color = Capy.TextSecondary,
        )
        LinearProgressIndicator(
            progress = { percent / 100f },
            modifier = Modifier.fillMaxWidth().height(6.dp).clip(RoundedCornerShape(3.dp)),
            color = Capy.Accent,
            trackColor = Capy.SurfaceHigh,
        )
        Grade.entries.forEach { grade ->
            val ofGrade = all.count { it.grade == grade }
            if (ofGrade == 0) return@forEach
            Row(verticalAlignment = Alignment.CenterVertically) {
                Box(Modifier.size(10.dp).background(grade.color(), CircleShape))
                Spacer(Modifier.width(10.dp))
                Text(grade.label(), style = MaterialTheme.typography.bodyMedium, modifier = Modifier.weight(1f))
                Text(
                    "${all.count { it.grade == grade && it.unlocked }} / $ofGrade",
                    style = MaterialTheme.typography.bodyMedium,
                    color = Capy.TextSecondary,
                )
            }
        }
        if (recent.isNotEmpty()) {
            Spacer(Modifier.height(4.dp))
            Text(stringResource(R.string.trophies_card_recent), style = MaterialTheme.typography.labelLarge, color = Capy.TextSecondary)
            recent.forEach { (entry, trophy) -> RecentTrophy(entry, trophy) }
        }
    }
}

@Composable
private fun RecentTrophy(entry: GameTrophies, trophy: Trophy) {
    val icon by produceState<ImageBitmap?>(null, entry.set.packagePath, trophy.id) {
        value = withContext(Dispatchers.IO) {
            Trophies.readIcon(entry.set, trophy)?.let { BitmapFactory.decodeByteArray(it, 0, it.size)?.asImageBitmap() }
        }
    }
    Row(verticalAlignment = Alignment.CenterVertically) {
        Box(Modifier.size(40.dp).clip(RoundedCornerShape(6.dp)).background(Capy.SurfaceHigh)) {
            icon?.let { Image(it, null, Modifier.fillMaxSize()) }
        }
        Spacer(Modifier.width(10.dp))
        Column(Modifier.weight(1f)) {
            Text(trophy.name, style = MaterialTheme.typography.bodyMedium, maxLines = 1, overflow = TextOverflow.Ellipsis)
            Text(
                entry.game.titleName,
                style = MaterialTheme.typography.bodySmall,
                color = Capy.TextMuted,
                maxLines = 1,
                overflow = TextOverflow.Ellipsis,
            )
        }
        Box(Modifier.size(10.dp).background(trophy.grade.color(), CircleShape))
    }
}

@Composable
private fun GameTrophyRow(entry: GameTrophies, focused: Boolean, onClick: () -> Unit) {
    val set = entry.set
    val gameIcon by rememberImage(entry.game.iconPath, 160)
    Row(
        Modifier
            .fillMaxWidth()
            .background(if (focused) Capy.AccentSoft else Capy.Surface.copy(alpha = 0.92f), RoundedCornerShape(12.dp))
            .clickable(onClick = onClick)
            .padding(horizontal = 14.dp, vertical = 10.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Box(
            Modifier.size(64.dp).clip(RoundedCornerShape(10.dp)).background(Capy.SurfaceHigh),
            contentAlignment = Alignment.Center,
        ) {
            gameIcon?.let { Image(it, null, Modifier.fillMaxSize(), contentScale = ContentScale.Crop) }
                ?: Icon(Icons.Outlined.SportsEsports, null, Modifier.size(26.dp), tint = Capy.TextSecondary)
        }
        Spacer(Modifier.width(14.dp))
        Column(Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(4.dp)) {
            Text(entry.game.titleName, style = MaterialTheme.typography.titleSmall, maxLines = 1, overflow = TextOverflow.Ellipsis)
            Text(
                stringResource(R.string.trophies_progress, set.earned, set.trophies.size, set.percent),
                style = MaterialTheme.typography.bodySmall,
                color = Capy.TextSecondary,
            )
            LinearProgressIndicator(
                progress = { set.percent / 100f },
                modifier = Modifier.fillMaxWidth().height(4.dp).clip(RoundedCornerShape(2.dp)),
                color = Capy.Accent,
                trackColor = Capy.SurfaceHigh,
            )
        }
        Spacer(Modifier.width(16.dp))
        // Earned per grade, as on the console's trophy list.
        Row(horizontalArrangement = Arrangement.spacedBy(12.dp), verticalAlignment = Alignment.CenterVertically) {
            Grade.entries.forEach { grade ->
                if (set.trophies.none { it.grade == grade }) return@forEach
                Row(verticalAlignment = Alignment.CenterVertically) {
                    Box(Modifier.size(10.dp).background(grade.color(), CircleShape))
                    Spacer(Modifier.width(5.dp))
                    Text(
                        set.trophies.count { it.grade == grade && it.unlocked }.toString(),
                        style = MaterialTheme.typography.labelLarge,
                        color = Capy.TextSecondary,
                    )
                }
            }
        }
    }
}
