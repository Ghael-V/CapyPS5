// SPDX-FileCopyrightText: Copyright 2026 CapyPS5 Project
// SPDX-License-Identifier: GPL-2.0-only

package org.capyps5.emulator

import android.content.ClipData
import android.content.Context
import android.content.Intent
import android.graphics.Bitmap
import android.graphics.BitmapFactory
import android.graphics.BitmapShader
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.LinearGradient
import android.graphics.Matrix
import android.graphics.Paint
import android.graphics.RectF
import android.graphics.Shader
import android.graphics.Typeface
import android.text.TextPaint
import android.text.TextUtils
import androidx.core.content.FileProvider
import org.capyps5.emulator.model.GameItem
import java.io.File

/** A 1200x630 image of a game's trophy progress, branded with the CapyPS5 logo and name. */
object TrophyCard {
    private const val WIDTH = 1200
    private const val HEIGHT = 630
    private const val MARGIN = 60f

    private val BACKGROUND = Color.rgb(0x07, 0x09, 0x0e)
    private val SURFACE = Color.rgb(0x1a, 0x1f, 0x2b)
    private val TEXT = Color.rgb(0xf1, 0xf3, 0xf7)
    private val TEXT_SECONDARY = Color.rgb(0x9a, 0xa3, 0xb5)
    private val TEXT_MUTED = Color.rgb(0x5e, 0x67, 0x78)
    private val ACCENT = Color.rgb(0x3d, 0x8b, 0xff)

    fun gradeColor(grade: Grade): Int = when (grade) {
        Grade.Platinum -> Color.rgb(0xb8, 0xd4, 0xe8)
        Grade.Gold -> Color.rgb(0xe3, 0xb5, 0x4a)
        Grade.Silver -> Color.rgb(0xb4, 0xba, 0xc4)
        Grade.Bronze -> Color.rgb(0xc0, 0x84, 0x57)
    }

    private fun gradeLabel(context: Context, grade: Grade): String = context.getString(
        when (grade) {
            Grade.Platinum -> R.string.trophy_platinum
            Grade.Gold -> R.string.trophy_gold
            Grade.Silver -> R.string.trophy_silver
            Grade.Bronze -> R.string.trophy_bronze
        },
    )

    fun render(context: Context, game: GameItem, set: TrophySet): Bitmap {
        val bitmap = Bitmap.createBitmap(WIDTH, HEIGHT, Bitmap.Config.ARGB_8888)
        val canvas = Canvas(bitmap)
        canvas.drawColor(BACKGROUND)

        // Key art on the right, faded into the background so the text stays readable.
        decode(game.backgroundPath, WIDTH)?.let { art ->
            val paint = Paint(Paint.ANTI_ALIAS_FLAG or Paint.FILTER_BITMAP_FLAG).apply { alpha = 110 }
            canvas.drawBitmap(art, null, cropRect(art, RectF(0f, 0f, WIDTH.toFloat(), HEIGHT.toFloat())), paint)
            val fade = Paint().apply {
                shader = LinearGradient(
                    0f, 0f, WIDTH.toFloat(), 0f,
                    intArrayOf(BACKGROUND, (BACKGROUND and 0x00ffffff) or (0xee shl 24), (BACKGROUND and 0x00ffffff) or (0x55 shl 24)),
                    floatArrayOf(0f, 0.55f, 1f), Shader.TileMode.CLAMP,
                )
            }
            canvas.drawRect(0f, 0f, WIDTH.toFloat(), HEIGHT.toFloat(), fade)
        }

        // Game icon.
        val iconRect = RectF(MARGIN, MARGIN, MARGIN + 220f, MARGIN + 220f)
        val icon = decode(game.iconPath, 440)
        if (icon != null) drawRounded(canvas, icon, iconRect, 28f) else {
            canvas.drawRoundRect(iconRect, 28f, 28f, Paint(Paint.ANTI_ALIAS_FLAG).apply { color = SURFACE })
        }

        val left = iconRect.right + 44f
        val textWidth = WIDTH - left - MARGIN
        val title = TextPaint(Paint.ANTI_ALIAS_FLAG).apply {
            color = TEXT
            textSize = 50f
            typeface = Typeface.create(Typeface.DEFAULT, Typeface.BOLD)
        }
        val name = set.title.ifEmpty { game.titleName }
        canvas.drawText(TextUtils.ellipsize(name, title, textWidth, TextUtils.TruncateAt.END).toString(), left, MARGIN + 52f, title)

        val body = TextPaint(Paint.ANTI_ALIAS_FLAG).apply { color = TEXT_SECONDARY; textSize = 30f }
        canvas.drawText(
            context.getString(R.string.trophies_progress, set.earned, set.trophies.size, set.percent),
            left, MARGIN + 104f, body,
        )

        // Progress bar.
        val barTop = MARGIN + 132f
        val bar = RectF(left, barTop, left + textWidth, barTop + 14f)
        canvas.drawRoundRect(bar, 7f, 7f, Paint(Paint.ANTI_ALIAS_FLAG).apply { color = SURFACE })
        if (set.percent > 0) {
            val fill = RectF(bar.left, bar.top, bar.left + bar.width() * set.percent / 100f, bar.bottom)
            canvas.drawRoundRect(fill, 7f, 7f, Paint(Paint.ANTI_ALIAS_FLAG).apply { color = ACCENT })
        }

        // Earned / total per grade.
        var x = left
        val countPaint = TextPaint(Paint.ANTI_ALIAS_FLAG).apply { color = TEXT; textSize = 28f }
        val dot = Paint(Paint.ANTI_ALIAS_FLAG)
        Grade.entries.forEach { grade ->
            val all = set.trophies.count { it.grade == grade }
            if (all == 0) return@forEach
            val earned = set.trophies.count { it.grade == grade && it.unlocked }
            dot.color = gradeColor(grade)
            canvas.drawCircle(x + 9f, MARGIN + 204f, 9f, dot)
            val label = "${gradeLabel(context, grade)} $earned/$all"
            canvas.drawText(label, x + 28f, MARGIN + 214f, countPaint)
            x += 28f + countPaint.measureText(label) + 36f
        }

        // The most recent trophies earned, newest first.
        val recent = set.trophies.filter { it.unlocked }.sortedByDescending { it.unlockedAt ?: 0L }.take(8)
        val rowTop = 340f
        if (recent.isEmpty()) {
            canvas.drawText(context.getString(R.string.trophies_card_none), MARGIN, rowTop + 60f, body)
        } else {
            val size = 112f
            recent.forEachIndexed { i, trophy ->
                val rect = RectF(MARGIN + i * (size + 22f), rowTop, MARGIN + i * (size + 22f) + size, rowTop + size)
                val bytes = Trophies.readIcon(set, trophy)
                val bitmapIcon = bytes?.let { BitmapFactory.decodeByteArray(it, 0, it.size) }
                if (bitmapIcon != null) drawRounded(canvas, bitmapIcon, rect, 16f)
                else canvas.drawRoundRect(rect, 16f, 16f, Paint(Paint.ANTI_ALIAS_FLAG).apply { color = SURFACE })
                canvas.drawRoundRect(
                    rect, 16f, 16f,
                    Paint(Paint.ANTI_ALIAS_FLAG).apply { style = Paint.Style.STROKE; strokeWidth = 4f; color = gradeColor(trophy.grade) },
                )
            }
        }

        // Footer: the emulator's logo and name.
        val footerY = HEIGHT - 70f
        canvas.drawLine(MARGIN, footerY - 34f, WIDTH - MARGIN, footerY - 34f, Paint().apply { color = SURFACE; strokeWidth = 2f })
        val logo = BitmapFactory.decodeResource(context.resources, R.drawable.capy_logo)
        val logoRect = RectF(MARGIN, footerY - 20f, MARGIN + 56f, footerY + 36f)
        logo?.let { canvas.drawBitmap(it, null, logoRect, Paint(Paint.FILTER_BITMAP_FLAG)) }
        val brand = TextPaint(Paint.ANTI_ALIAS_FLAG).apply {
            textSize = 38f
            typeface = Typeface.create(Typeface.DEFAULT, Typeface.BOLD)
        }
        val brandX = logoRect.right + 16f
        brand.color = TEXT
        canvas.drawText("Capy", brandX, footerY + 21f, brand)
        brand.color = ACCENT
        canvas.drawText("PS5", brandX + brand.measureText("Capy"), footerY + 21f, brand)
        val site = TextPaint(Paint.ANTI_ALIAS_FLAG).apply { color = TEXT_MUTED; textSize = 28f; textAlign = Paint.Align.RIGHT }
        canvas.drawText("capyps5.org", WIDTH - MARGIN, footerY + 18f, site)
        return bitmap
    }

    /** Renders the card into the cache and opens the system share sheet with it. */
    fun share(context: Context, game: GameItem, set: TrophySet) {
        val dir = File(context.cacheDir, "share").apply { mkdirs() }
        val file = File(dir, "trophies-${game.titleId.filter { it.isLetterOrDigit() }}.png")
        file.outputStream().use { render(context, game, set).compress(Bitmap.CompressFormat.PNG, 100, it) }
        val uri = FileProvider.getUriForFile(context, "${context.packageName}.updates", file)
        val send = Intent(Intent.ACTION_SEND).apply {
            type = "image/png"
            putExtra(Intent.EXTRA_STREAM, uri)
            putExtra(
                Intent.EXTRA_TEXT,
                context.getString(R.string.trophies_share_text, set.title.ifEmpty { game.titleName }, set.earned, set.trophies.size),
            )
            clipData = ClipData.newRawUri(null, uri)
            addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION)
        }
        context.startActivity(
            Intent.createChooser(send, context.getString(R.string.trophies_share)).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK),
        )
    }

    private fun decode(path: String?, maxSize: Int): Bitmap? = path?.let {
        runCatching {
            val bounds = BitmapFactory.Options().apply { inJustDecodeBounds = true }
            BitmapFactory.decodeFile(it, bounds)
            var sample = 1
            while (bounds.outWidth / (sample * 2) >= maxSize && bounds.outHeight / (sample * 2) >= maxSize) sample *= 2
            BitmapFactory.decodeFile(it, BitmapFactory.Options().apply { inSampleSize = sample })
        }.getOrNull()
    }

    /** Destination that fills [target] with the bitmap's aspect, cropping the overflow. */
    private fun cropRect(bitmap: Bitmap, target: RectF): RectF {
        val scale = maxOf(target.width() / bitmap.width, target.height() / bitmap.height)
        val w = bitmap.width * scale
        val h = bitmap.height * scale
        return RectF(target.centerX() - w / 2, target.centerY() - h / 2, target.centerX() + w / 2, target.centerY() + h / 2)
    }

    private fun drawRounded(canvas: Canvas, bitmap: Bitmap, rect: RectF, radius: Float) {
        val shader = BitmapShader(bitmap, Shader.TileMode.CLAMP, Shader.TileMode.CLAMP)
        val dest = cropRect(bitmap, rect)
        shader.setLocalMatrix(
            Matrix().apply {
                setScale(dest.width() / bitmap.width, dest.height() / bitmap.height)
                postTranslate(dest.left, dest.top)
            },
        )
        canvas.drawRoundRect(rect, radius, radius, Paint(Paint.ANTI_ALIAS_FLAG).apply { this.shader = shader })
    }
}
