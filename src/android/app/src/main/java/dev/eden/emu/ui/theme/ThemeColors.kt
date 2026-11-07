// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

package dev.eden.emu.ui.theme

import androidx.compose.runtime.staticCompositionLocalOf
import androidx.compose.ui.graphics.Color

val EdenBrandColor = Color(0xFFA161F3)
val EdenSplashColor = EdenBrandColor
val EdenGridLine = Color(0x2AA161F3)
val EdenBackground = Color(0xFF191521)
val EdenSurface = Color(0xFF221C2B)
val EdenSurfaceVariant = Color(0xFF2D2538)
val EdenOnBackground = Color(0xFFEAE5F2)
val EdenOnSurface = Color(0xFFEAE5F2)
val EdenOnSurfaceVariant = Color(0xFFB0A8BA)
val EdenOnPrimary = Color(0xFFFFFFFF)

internal val LocalEdenColors = staticCompositionLocalOf {
    EdenColors(
        background = EdenBackground,
        surface = EdenSurface,
        surfaceVariant = EdenSurfaceVariant,
        primary = EdenBrandColor,
        onBackground = EdenOnBackground,
        onSurface = EdenOnSurface,
        onSurfaceVariant = EdenOnSurfaceVariant,
        onPrimary = EdenOnPrimary,
    )
}

data class EdenColors(
    val background: Color,
    val surface: Color,
    val surfaceVariant: Color,
    val primary: Color,
    val onBackground: Color,
    val onSurface: Color,
    val onSurfaceVariant: Color,
    val onPrimary: Color,
)
