package org.capyps5.emulator.ui.theme

import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.darkColorScheme
import androidx.compose.runtime.Composable
import androidx.compose.ui.graphics.Color

val CapyPrimary = Color(0xFF0072CE)
val CapyPrimaryVariant = Color(0xFF004B8D)
val CapySecondary = Color(0xFF00E5FF)
val CapyBackground = Color(0xFF0A0C12)
val CapySurface = Color(0xFF141722)
val CapySurfaceVariant = Color(0xFF1F2433)
val CapyTextPrimary = Color(0xFFF2F4F8)
val CapyTextSecondary = Color(0xFF8E96A8)

private val DarkColorScheme = darkColorScheme(
    primary = CapyPrimary,
    secondary = CapySecondary,
    background = CapyBackground,
    surface = CapySurface,
    surfaceVariant = CapySurfaceVariant,
    onPrimary = Color.White,
    onSecondary = Color.Black,
    onBackground = CapyTextPrimary,
    onSurface = CapyTextPrimary
)

@Composable
fun CapyPS5Theme(content: @Composable () -> Unit) {
    MaterialTheme(
        colorScheme = DarkColorScheme,
        content = content
    )
}
