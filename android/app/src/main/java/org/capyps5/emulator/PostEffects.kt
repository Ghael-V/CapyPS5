// SPDX-FileCopyrightText: Copyright 2026 CapyPS5 Project
// SPDX-License-Identifier: GPL-2.0-only

package org.capyps5.emulator

import android.content.Context
import androidx.annotation.StringRes
import java.util.Locale
import kotlin.math.roundToInt

/** Core text property holding the post-processing effects chain (PostProcessor::SetEffects). */
const val POST_CHAIN_PROPERTY = "debug.capy.pp_chain"

/** One adjustable value of an effect: its range, the step of a controller press and its default. */
class EffectParam(@StringRes val labelRes: Int, val min: Float, val max: Float, val step: Float, val default: Float) {
    fun clamp(value: Float) = value.coerceIn(min, max)

    /** [value] moved by [steps] steps and snapped to the step grid. */
    fun step(value: Float, steps: Int): Float = clamp(((value - min) / step).roundToInt().plus(steps) * step + min)

    fun format(value: Float): String = when {
        step >= 1f -> value.roundToInt().toString()
        step >= 0.1f -> String.format(Locale.ROOT, "%.1f", value)
        step >= 0.01f -> String.format(Locale.ROOT, "%.2f", value)
        else -> String.format(Locale.ROOT, "%.3f", value)
    }
}

private fun p(@StringRes label: Int, min: Float, max: Float, step: Float, default: Float) =
    EffectParam(label, min, max, step, default)

/**
 * The effects of the post-processing chain, each one fullscreen pass in the core. The ids and the
 * parameter order are those of the core table (postProcess.cpp): keep both in step.
 */
enum class PostEffect(val id: String, @StringRes val titleRes: Int, @StringRes val descriptionRes: Int, val params: List<EffectParam>) {
    Sharpen("sharpen", R.string.fx_sharpen, R.string.fx_sharpen_desc, listOf(p(R.string.fxp_amount, 0f, 1f, 0.05f, 0.5f))),
    Bloom(
        "bloom", R.string.fx_bloom, R.string.fx_bloom_desc,
        listOf(
            p(R.string.fxp_threshold, 0f, 1f, 0.05f, 0.6f),
            p(R.string.fxp_radius, 0.5f, 4f, 0.1f, 1.5f),
            p(R.string.fxp_amount, 0f, 2f, 0.05f, 0.4f),
        ),
    ),
    Blur(
        "blur", R.string.fx_blur, R.string.fx_blur_desc,
        listOf(
            p(R.string.fxp_radius, 0.5f, 8f, 0.5f, 2f),
            p(R.string.fxp_strength, 0f, 1f, 0.05f, 1f),
            p(R.string.fxp_focus, 0f, 1f, 0.05f, 0f),
            p(R.string.fxp_focus_falloff, 0.05f, 1f, 0.05f, 0.4f),
        ),
    ),
    Crt(
        "crt", R.string.fx_crt, R.string.fx_crt_desc,
        listOf(
            p(R.string.fxp_scanlines, 0f, 1f, 0.05f, 0.5f),
            p(R.string.fxp_mask, 0f, 1f, 0.05f, 0.3f),
            p(R.string.fxp_curvature, 0f, 0.25f, 0.01f, 0.08f),
            p(R.string.fxp_lines, 120f, 1080f, 20f, 480f),
            p(R.string.fxp_bleed, 0f, 3f, 0.1f, 1f),
            p(R.string.fxp_roll, 0f, 2f, 0.1f, 0f),
        ),
    ),
    Scanlines(
        "scanlines", R.string.fx_scanlines, R.string.fx_scanlines_desc,
        listOf(
            p(R.string.fxp_strength, 0f, 1f, 0.05f, 0.35f),
            p(R.string.fxp_lines, 120f, 1080f, 20f, 540f),
            p(R.string.fxp_thickness, 0.1f, 0.9f, 0.05f, 0.5f),
        ),
    ),
    Vignette(
        "vignette", R.string.fx_vignette, R.string.fx_vignette_desc,
        listOf(
            p(R.string.fxp_strength, 0f, 1.5f, 0.05f, 0.5f),
            p(R.string.fxp_radius, 0.2f, 1.5f, 0.05f, 0.8f),
            p(R.string.fxp_softness, 0.05f, 1f, 0.05f, 0.45f),
        ),
    ),
    Chroma(
        "chroma", R.string.fx_chroma, R.string.fx_chroma_desc,
        listOf(p(R.string.fxp_strength, 0f, 5f, 0.1f, 1.5f), p(R.string.fxp_falloff, 0.1f, 3f, 0.1f, 1.5f)),
    ),
    Grain(
        "grain", R.string.fx_grain, R.string.fx_grain_desc,
        listOf(
            p(R.string.fxp_intensity, 0f, 0.2f, 0.005f, 0.04f),
            p(R.string.fxp_size, 1f, 4f, 0.25f, 1.5f),
            p(R.string.fxp_colour, 0f, 1f, 0.1f, 0f),
        ),
    ),
    Grade(
        "grade", R.string.fx_grade, R.string.fx_grade_desc,
        listOf(
            p(R.string.fxp_brightness, -0.5f, 0.5f, 0.01f, 0f),
            p(R.string.fxp_contrast, 0f, 2f, 0.05f, 1f),
            p(R.string.fxp_saturation, 0f, 2f, 0.05f, 1f),
            p(R.string.fxp_vibrance, -1f, 1f, 0.05f, 0f),
            p(R.string.fxp_gamma, 0.5f, 2f, 0.05f, 1f),
            p(R.string.fxp_temperature, -1f, 1f, 0.05f, 0f),
            p(R.string.fxp_tint, -1f, 1f, 0.05f, 0f),
        ),
    ),
    Filmic(
        "filmic", R.string.fx_filmic, R.string.fx_filmic_desc,
        listOf(
            p(R.string.fxp_exposure, -2f, 2f, 0.1f, 0f),
            p(R.string.fxp_toe, 0f, 1f, 0.05f, 0.4f),
            p(R.string.fxp_shoulder, 0f, 1f, 0.05f, 0.5f),
            p(R.string.fxp_amount, 0f, 1f, 0.05f, 1f),
        ),
    ),
    Levels(
        "levels", R.string.fx_levels, R.string.fx_levels_desc,
        listOf(
            p(R.string.fxp_input_black, 0f, 0.5f, 0.01f, 0f),
            p(R.string.fxp_input_white, 0.5f, 1f, 0.01f, 1f),
            p(R.string.fxp_gamma, 0.25f, 4f, 0.05f, 1f),
            p(R.string.fxp_output_black, 0f, 0.5f, 0.01f, 0f),
            p(R.string.fxp_output_white, 0.5f, 1f, 0.01f, 1f),
        ),
    ),
    Split(
        "split", R.string.fx_split, R.string.fx_split_desc,
        listOf(
            p(R.string.fxp_shadow_hue, 0f, 360f, 10f, 210f),
            p(R.string.fxp_shadow_strength, 0f, 1f, 0.05f, 0.2f),
            p(R.string.fxp_highlight_hue, 0f, 360f, 10f, 40f),
            p(R.string.fxp_highlight_strength, 0f, 1f, 0.05f, 0.2f),
            p(R.string.fxp_balance, -1f, 1f, 0.05f, 0f),
        ),
    ),
    Deband(
        "deband", R.string.fx_deband, R.string.fx_deband_desc,
        listOf(
            p(R.string.fxp_threshold, 0.002f, 0.05f, 0.002f, 0.012f),
            p(R.string.fxp_radius, 2f, 32f, 1f, 12f),
            p(R.string.fxp_dither, 0f, 1f, 0.1f, 0.5f),
        ),
    ),
    Denoise(
        "denoise", R.string.fx_denoise, R.string.fx_denoise_desc,
        listOf(p(R.string.fxp_strength, 0f, 1f, 0.05f, 0.25f), p(R.string.fxp_radius, 0.5f, 3f, 0.25f, 1f)),
    ),
    Cartoon(
        "cartoon", R.string.fx_cartoon, R.string.fx_cartoon_desc,
        listOf(
            p(R.string.fxp_outline, 0f, 2f, 0.1f, 1f),
            p(R.string.fxp_outline_threshold, 0.02f, 1f, 0.02f, 0.2f),
            p(R.string.fxp_colour_levels, 2f, 16f, 1f, 6f),
            p(R.string.fxp_saturation, 0f, 2f, 0.05f, 1.15f),
        ),
    ),
    Cel(
        "cel", R.string.fx_cel, R.string.fx_cel_desc,
        listOf(
            p(R.string.fxp_bands, 2f, 10f, 1f, 4f),
            p(R.string.fxp_band_softness, 0f, 1f, 0.05f, 0.2f),
            p(R.string.fxp_outline, 0f, 2f, 0.1f, 0.8f),
            p(R.string.fxp_outline_threshold, 0.02f, 1f, 0.02f, 0.25f),
            p(R.string.fxp_saturation, 0f, 2f, 0.05f, 1.2f),
        ),
    ),
    Lens(
        "lens", R.string.fx_lens, R.string.fx_lens_desc,
        listOf(p(R.string.fxp_distortion, -0.5f, 0.5f, 0.01f, 0.15f), p(R.string.fxp_zoom, 0.5f, 1.5f, 0.01f, 1f)),
    ),
    Pixelate("pixelate", R.string.fx_pixelate, R.string.fx_pixelate_desc, listOf(p(R.string.fxp_pixel_size, 1f, 32f, 1f, 4f))),
    Mono(
        "mono", R.string.fx_mono, R.string.fx_mono_desc,
        listOf(p(R.string.fxp_amount, 0f, 1f, 0.05f, 1f), p(R.string.fxp_sepia, 0f, 1f, 0.05f, 0f)),
    ),
    ;

    fun title(context: Context) = context.getString(titleRes)

    fun defaults(): List<Float> = params.map { it.default }

    companion object {
        fun byId(id: String): PostEffect? = entries.firstOrNull { it.id == id }
    }
}

/** One effect in the chain, with its values; disabled ones stay in the chain but are skipped. */
data class EffectEntry(val effect: PostEffect, val enabled: Boolean = true, val values: List<Float> = effect.defaults())

/** The chain as the core reads it: `id:v1,v2;...`, `!` before the id of a disabled effect. */
object PostChain {
    fun decode(text: String): List<EffectEntry> = text.split(';').mapNotNull { raw ->
        val entry = raw.trim()
        if (entry.isEmpty()) return@mapNotNull null
        val enabled = !entry.startsWith('!')
        val body = entry.removePrefix("!")
        val effect = PostEffect.byId(body.substringBefore(':')) ?: return@mapNotNull null
        val given = body.substringAfter(':', "").split(',').map { it.trim().toFloatOrNull() }
        EffectEntry(effect, enabled, effect.params.mapIndexed { i, param -> given.getOrNull(i)?.let(param::clamp) ?: param.default })
    }

    fun encode(entries: List<EffectEntry>): String = entries.joinToString(";") { entry ->
        (if (entry.enabled) "" else "!") + entry.effect.id + ":" +
            entry.values.joinToString(",") { value ->
                String.format(Locale.ROOT, "%.4f", value).trimEnd('0').trimEnd('.').let { if (it == "-0") "0" else it }
            }
    }

    /** The same chain in its canonical spelling, so equal chains compare equal. */
    fun normalize(text: String): String = encode(decode(text))

    fun summary(context: Context, text: String): String {
        val entries = decode(text).filter { it.enabled }
        return if (entries.isEmpty()) context.getString(R.string.pp_no_effects)
        else entries.joinToString(", ") { it.effect.title(context) }
    }
}

private fun chain(vararg entries: Pair<PostEffect, List<Float>>): String =
    PostChain.encode(entries.map { (effect, values) -> EffectEntry(effect, true, values) })

/** Ready-made chains. Off is the empty chain: the frame is presented unchanged. */
enum class PostPreset(@StringRes val titleRes: Int, @StringRes val descriptionRes: Int, val chain: String) {
    Off(R.string.pp_preset_off, R.string.pp_preset_off_desc, ""),
    Clean(
        R.string.pp_preset_clean, R.string.pp_preset_clean_desc,
        chain(
            PostEffect.Denoise to listOf(0.15f, 1f),
            PostEffect.Deband to listOf(0.012f, 12f, 0.5f),
            PostEffect.Sharpen to listOf(0.5f),
        ),
    ),
    Vivid(
        R.string.pp_preset_vivid, R.string.pp_preset_vivid_desc,
        chain(
            PostEffect.Grade to listOf(0.02f, 1.08f, 1.25f, 0.3f, 1f, 0.1f, 0f),
            PostEffect.Levels to listOf(0.02f, 0.98f, 1f, 0f, 1f),
            PostEffect.Sharpen to listOf(0.35f),
        ),
    ),
    Cinematic(
        R.string.pp_preset_cinematic, R.string.pp_preset_cinematic_desc,
        chain(
            PostEffect.Bloom to listOf(0.65f, 1.6f, 0.35f),
            PostEffect.Filmic to listOf(0f, 0.45f, 0.5f, 0.85f),
            PostEffect.Split to listOf(210f, 0.25f, 40f, 0.25f, 0f),
            PostEffect.Grain to listOf(0.025f, 1.5f, 0f),
            PostEffect.Vignette to listOf(0.5f, 0.8f, 0.45f),
        ),
    ),
    Retro(
        R.string.pp_preset_retro, R.string.pp_preset_retro_desc,
        chain(
            PostEffect.Chroma to listOf(1.2f, 1.5f),
            PostEffect.Crt to listOf(0.55f, 0.35f, 0.08f, 480f, 1.2f, 0f),
            PostEffect.Vignette to listOf(0.6f, 0.75f, 0.5f),
        ),
    ),
    Anime(
        R.string.pp_preset_anime, R.string.pp_preset_anime_desc,
        chain(
            PostEffect.Denoise to listOf(0.2f, 1.25f),
            PostEffect.Cel to listOf(5f, 0.2f, 0.7f, 0.2f, 1.3f),
            PostEffect.Bloom to listOf(0.7f, 1.2f, 0.25f),
        ),
    ),
    Dreamy(
        R.string.pp_preset_dreamy, R.string.pp_preset_dreamy_desc,
        chain(
            PostEffect.Blur to listOf(5f, 0.85f, 0.3f, 0.5f),
            PostEffect.Bloom to listOf(0.5f, 2f, 0.55f),
        ),
    ),
    ;

    fun title(context: Context) = context.getString(titleRes)

    companion object {
        /** The preset whose chain this is, or null for a chain of the user's own. */
        fun of(text: String): PostPreset? {
            val normalized = PostChain.normalize(text)
            return entries.firstOrNull { it.chain == normalized }
        }

        /** Preset name, or "Custom" for a chain of the user's own. */
        fun label(context: Context, text: String): String =
            of(text)?.title(context) ?: context.getString(R.string.pp_preset_custom)
    }
}
