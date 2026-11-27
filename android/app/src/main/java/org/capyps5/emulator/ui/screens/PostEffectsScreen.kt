// SPDX-FileCopyrightText: Copyright 2026 CapyPS5 Project
// SPDX-License-Identifier: GPL-2.0-only

package org.capyps5.emulator.ui.screens

import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.itemsIndexed
import androidx.compose.foundation.lazy.rememberLazyListState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.unit.dp
import org.capyps5.emulator.EffectEntry
import org.capyps5.emulator.PostChain
import org.capyps5.emulator.PostEffect
import org.capyps5.emulator.PostPreset
import org.capyps5.emulator.R
import org.capyps5.emulator.ui.NavKey
import org.capyps5.emulator.ui.OnNavKey
import org.capyps5.emulator.ui.theme.Capy

/** A row of the effects editor: a value stepped with left/right, or an action on confirm. */
private class EditorRow(
    val title: String,
    val subtitle: String? = null,
    val value: String? = null,
    val header: Boolean = false,
    val indent: Boolean = false,
    val danger: Boolean = false,
    val onStep: ((Int) -> Unit)? = null,
    val onConfirm: (() -> Unit)? = null,
)

/**
 * Editor of a post-processing chain: a preset, then every effect in order with its switch, its
 * place in the order and its values, then adding effects. Every change goes to [onChange] at once
 * (in a game it applies live). [onUseGlobal] offers dropping a game's own chain.
 */
@Composable
fun PostEffectsEditor(
    title: String,
    chain: String,
    onChange: (String) -> Unit,
    onClose: () -> Unit,
    modifier: Modifier = Modifier,
    onUseGlobal: (() -> Unit)? = null,
) {
    val context = LocalContext.current
    var entries by remember { mutableStateOf(PostChain.decode(chain)) }
    var adding by remember { mutableStateOf(PostEffect.Bloom) }
    var row by remember { mutableIntStateOf(0) }

    fun update(list: List<EffectEntry>) {
        entries = list
        onChange(PostChain.encode(list))
    }

    val rows = buildList {
        val presets = PostPreset.entries
        val current = PostPreset.of(PostChain.encode(entries))
        add(
            EditorRow(
                context.getString(R.string.pp_preset),
                current?.let { context.getString(it.descriptionRes) },
                PostPreset.label(context, PostChain.encode(entries)),
                onStep = { delta ->
                    val index = current?.let(presets::indexOf) ?: -1
                    update(PostChain.decode(presets[(index + delta).mod(presets.size)].chain))
                },
            ),
        )
        entries.forEachIndexed { i, entry ->
            val effect = entry.effect
            add(
                EditorRow(
                    "${i + 1}. ${effect.title(context)}",
                    context.getString(effect.descriptionRes),
                    context.getString(if (entry.enabled) R.string.enabled else R.string.disabled),
                    header = true,
                    onStep = { update(entries.toMutableList().also { it[i] = entry.copy(enabled = !entry.enabled) }) },
                ),
            )
            add(
                EditorRow(
                    context.getString(R.string.pp_order), context.getString(R.string.pp_order_desc),
                    "${i + 1} / ${entries.size}", indent = true,
                    onStep = { delta ->
                        val target = (i + delta).coerceIn(0, entries.lastIndex)
                        if (target != i) {
                            val moved = entries.toMutableList().also { it.add(target, it.removeAt(i)) }
                            update(moved)
                            // Keep the focus on the order row of the moved effect.
                            row = 2 + moved.take(target).sumOf { 3 + it.effect.params.size }
                        }
                    },
                ),
            )
            effect.params.forEachIndexed { p, param ->
                add(
                    EditorRow(
                        context.getString(param.labelRes), null, param.format(entry.values[p]), indent = true,
                        onStep = { delta ->
                            val values = entry.values.toMutableList()
                            values[p] = param.step(values[p], delta)
                            update(entries.toMutableList().also { it[i] = entry.copy(values = values) })
                        },
                    ),
                )
            }
            add(
                EditorRow(
                    context.getString(R.string.pp_remove), null, indent = true, danger = true,
                    onConfirm = { update(entries.toMutableList().also { it.removeAt(i) }) },
                ),
            )
        }
        add(
            EditorRow(
                context.getString(R.string.pp_add), context.getString(R.string.pp_add_desc), adding.title(context),
                onStep = { delta -> adding = PostEffect.entries[(adding.ordinal + delta).mod(PostEffect.entries.size)] },
                onConfirm = { update(entries + EffectEntry(adding)) },
            ),
        )
        if (entries.isNotEmpty()) {
            add(EditorRow(context.getString(R.string.pp_remove_all), null, danger = true, onConfirm = { update(emptyList()) }))
        }
        if (onUseGlobal != null) {
            add(
                EditorRow(
                    context.getString(R.string.pp_use_global), context.getString(R.string.pp_use_global_desc),
                    onConfirm = onUseGlobal,
                ),
            )
        }
    }
    row = row.coerceIn(0, rows.lastIndex)

    val listState = rememberLazyListState()
    LaunchedEffect(row) { listState.animateScrollToItem((row - 2).coerceAtLeast(0)) }

    OnNavKey { key ->
        val item = rows.getOrNull(row)
        when (key) {
            NavKey.Up -> row = (row - 1).mod(rows.size)
            NavKey.Down -> row = (row + 1).mod(rows.size)
            NavKey.Left -> item?.onStep?.invoke(-1)
            NavKey.Right -> item?.onStep?.invoke(1)
            NavKey.Confirm -> item?.onConfirm?.invoke() ?: item?.onStep?.invoke(1)
            NavKey.Back, NavKey.Menu, NavKey.Secondary -> onClose()
            else -> Unit
        }
    }

    Column(modifier.background(Capy.Background).padding(vertical = 18.dp)) {
        ScreenHeader(title, onBack = onClose)
        Text(
            stringResource(R.string.pp_editor_hint),
            style = MaterialTheme.typography.bodySmall,
            color = Capy.TextSecondary,
            modifier = Modifier.padding(horizontal = 24.dp),
        )
        Spacer(Modifier.height(10.dp))
        LazyColumn(
            state = listState,
            modifier = Modifier.weight(1f).padding(horizontal = 20.dp),
            verticalArrangement = Arrangement.spacedBy(6.dp),
        ) {
            itemsIndexed(rows) { i, item ->
                EditorRowView(item, focused = i == row) { row = i }
            }
        }
        HintBar(
            listOf(
                "◀ ▶" to stringResource(R.string.hint_change),
                "Ⓐ" to stringResource(R.string.hint_choose),
                "Ⓑ" to stringResource(R.string.hint_back),
            ),
            Modifier.padding(horizontal = 24.dp, vertical = 6.dp),
        )
    }
}

@Composable
private fun EditorRowView(item: EditorRow, focused: Boolean, onFocus: () -> Unit) {
    val shape = RoundedCornerShape(10.dp)
    Row(
        modifier = Modifier
            .fillMaxWidth()
            .padding(start = if (item.indent) 22.dp else 0.dp)
            .background(if (focused) Capy.SurfaceHigh else if (item.header) Capy.Surface else Color.Transparent, shape)
            .border(1.dp, if (focused) Capy.Accent else Color.Transparent, shape)
            .clickable {
                onFocus()
                item.onConfirm?.invoke() ?: item.onStep?.invoke(1)
            }
            .padding(horizontal = 14.dp, vertical = if (item.header) 12.dp else 8.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Column(Modifier.weight(1f)) {
            Text(
                item.title,
                style = if (item.header) MaterialTheme.typography.titleMedium else MaterialTheme.typography.bodyLarge,
                color = if (item.danger) Capy.Bad else Capy.TextPrimary,
            )
            item.subtitle?.let { Text(it, style = MaterialTheme.typography.bodySmall, color = Capy.TextSecondary) }
        }
        if (item.onStep != null) {
            // Touch: the arrows step down and up; a tap on the row steps up.
            Text(
                "‹",
                style = MaterialTheme.typography.titleLarge,
                color = Capy.Accent,
                modifier = Modifier.clickable { onFocus(); item.onStep.invoke(-1) }.padding(horizontal = 10.dp),
            )
        }
        item.value?.let { Text(it, style = MaterialTheme.typography.titleMedium, color = Capy.Accent) }
        if (item.onStep != null) {
            Text(
                "›",
                style = MaterialTheme.typography.titleLarge,
                color = Capy.Accent,
                modifier = Modifier.clickable { onFocus(); item.onStep.invoke(1) }.padding(horizontal = 10.dp),
            )
        }
    }
}
