// SPDX-FileCopyrightText: Copyright 2026 CapyPS5 Project
// SPDX-License-Identifier: GPL-2.0-only

package org.capyps5.emulator.aiconfig

import android.content.Context
import org.capyps5.emulator.CoreOption
import org.capyps5.emulator.R
import org.json.JSONObject

/**
 * The settings the recommended settings may change, from res/raw/ai_settings_schema.json (made by
 * android/ai-schema/gen_schema.py). Every suggestion from the server is checked here again before
 * it is written. Keys are debug.capy.* property names without the prefix: a core option of the
 * settings screens ("app") or a hidden switch the core reads ("hidden").
 */
class AiSchema(val app: String, val schemaVersion: Int, val settings: List<Entry>) {
    class Entry(
        val key: String,
        val tier: String,
        val type: String,
        val default: Long,
        val min: Long?,
        val max: Long?,
        val enum: List<Pair<String, Long>>,
        val uiTitle: String?,
        val source: String,
    ) {
        val isRisky get() = tier == "risky"
        val isBool get() = type == "bool"

        /** The core option of the settings screens this key is, or null for a hidden switch. */
        val coreOption: CoreOption? get() = CoreOption.entries.firstOrNull { it.property == PREFIX + key }
    }

    private val byKey = settings.associateBy { it.key }

    fun entry(key: String): Entry? = byKey[key]

    /**
     * The value to store for [value] (a JSON value from the server) on [key], or null when the key
     * is unknown or the value does not fit it (wrong type, not one of the enum values, out of range).
     */
    fun normalize(key: String, value: Any?): Long? {
        val entry = byKey[key] ?: return null
        if (entry.isBool) return (value as? Boolean)?.let { if (it) 1L else 0L }
        val number = when (value) {
            is Int -> value.toLong()
            is Long -> value
            is Double -> value.toLong().takeIf { it.toDouble() == value }
            else -> null
        } ?: return null
        if (entry.enum.isNotEmpty()) return number.takeIf { n -> entry.enum.any { it.second == n } }
        if (entry.min != null && number < entry.min) return null
        if (entry.max != null && number > entry.max) return null
        if (entry.min == null || entry.max == null) return null
        return number
    }

    /** A stored value as the server reads it: "true"/"false" or the number. */
    fun wire(entry: Entry, value: Long): String = if (entry.isBool) (value != 0L).toString() else value.toString()

    companion object {
        const val PREFIX = "debug.capy."

        @Volatile
        private var cached: AiSchema? = null

        fun load(context: Context): AiSchema = cached ?: synchronized(this) {
            cached ?: parse(
                context.resources.openRawResource(R.raw.ai_settings_schema).bufferedReader().use { it.readText() }
            ).also { cached = it }
        }

        private fun parse(text: String): AiSchema {
            val root = JSONObject(text)
            val list = root.getJSONArray("settings")
            val entries = (0 until list.length()).map { i ->
                val o = list.getJSONObject(i)
                val type = o.getString("type")
                val enumArray = o.optJSONArray("enum")
                val enum = if (enumArray == null) emptyList() else (0 until enumArray.length()).map { j ->
                    val v = enumArray.getJSONObject(j)
                    v.getString("name") to v.getLong("value")
                }
                val default = o.optString("default")
                Entry(
                    key = o.getString("key"),
                    tier = o.getString("tier"),
                    type = type,
                    default = if (type == "bool") (if (default == "true") 1L else 0L) else default.toLongOrNull() ?: 0L,
                    min = o.optString("min").toLongOrNull(),
                    max = o.optString("max").toLongOrNull(),
                    enum = enum,
                    uiTitle = o.optString("ui_title").ifEmpty { null },
                    source = o.optString("source"),
                )
            }
            return AiSchema(root.getString("app"), root.getInt("schema_version"), entries)
        }
    }
}
