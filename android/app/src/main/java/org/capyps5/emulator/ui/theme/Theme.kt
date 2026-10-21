// SPDX-FileCopyrightText: Copyright 2026 CapyPS5 Project
// SPDX-License-Identifier: GPL-2.0-only

package org.capyps5.emulator.ui.theme

import androidx.compose.material3.LocalContentColor
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Typography
import androidx.compose.material3.darkColorScheme
import androidx.compose.runtime.Composable
import androidx.compose.runtime.CompositionLocalProvider
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.sp

object Capy {
    val Background = Color(0xFF07090E)
    val Surface = Color(0xFF11141C)
    val SurfaceHigh = Color(0xFF1A1F2B)
    val Outline = Color(0x22FFFFFF)
    val Accent = Color(0xFF3D8BFF)
    val AccentSoft = Color(0x333D8BFF)
    val TextPrimary = Color(0xFFF1F3F7)
    val TextSecondary = Color(0xFF9AA3B5)
    val TextMuted = Color(0xFF5E6778)
    val Good = Color(0xFF4ADE80)
    val Warn = Color(0xFFFACC15)
    val Bad = Color(0xFFF87171)
    val Scrim = Color(0xCC07090E)
}

private val DarkColorScheme = darkColorScheme(
    primary = Capy.Accent,
    onPrimary = Color.White,
    secondary = Capy.Accent,
    background = Capy.Background,
    onBackground = Capy.TextPrimary,
    surface = Capy.Surface,
    onSurface = Capy.TextPrimary,
    surfaceVariant = Capy.SurfaceHigh,
    onSurfaceVariant = Capy.TextSecondary,
    surfaceContainerHigh = Capy.SurfaceHigh,
    outline = Capy.Outline,
    error = Capy.Bad,
)

private val CapyTypography = Typography(
    displaySmall = TextStyle(fontSize = 30.sp, fontWeight = FontWeight.Bold, letterSpacing = (-0.5).sp),
    titleLarge = TextStyle(fontSize = 20.sp, fontWeight = FontWeight.SemiBold),
    titleMedium = TextStyle(fontSize = 15.sp, fontWeight = FontWeight.SemiBold),
    bodyMedium = TextStyle(fontSize = 13.sp),
    bodySmall = TextStyle(fontSize = 11.sp),
    labelLarge = TextStyle(fontSize = 13.sp, fontWeight = FontWeight.SemiBold, letterSpacing = 0.3.sp),
    labelSmall = TextStyle(fontSize = 10.sp, fontWeight = FontWeight.Medium, letterSpacing = 0.5.sp),
)

@Composable
fun CapyPS5Theme(content: @Composable () -> Unit) {
    MaterialTheme(colorScheme = DarkColorScheme, typography = CapyTypography) {
        // Screens draw their own backgrounds instead of using Surface, so set the text color here.
        CompositionLocalProvider(LocalContentColor provides Capy.TextPrimary, content = content)
    }
}
