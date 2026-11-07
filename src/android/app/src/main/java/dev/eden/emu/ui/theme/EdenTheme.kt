// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

package dev.eden.emu.ui.theme

import androidx.compose.runtime.Composable
import androidx.compose.runtime.CompositionLocalProvider
import androidx.compose.runtime.staticCompositionLocalOf
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.sp

private val LocalEdenTypography = staticCompositionLocalOf {
    EdenTypography(
        title = TextStyle(
            fontFamily = FontFamily.SansSerif,
            fontWeight = FontWeight.SemiBold,
            fontSize = 20.sp,
        ),
        body = TextStyle(
            fontFamily = FontFamily.SansSerif,
            fontWeight = FontWeight.Normal,
            fontSize = 16.sp,
        ),
        label = TextStyle(
            fontFamily = FontFamily.SansSerif,
            fontWeight = FontWeight.Medium,
            fontSize = 14.sp,
        ),
    )
}

object EdenTheme {
    val colors: EdenColors
        @Composable get() = LocalEdenColors.current

    val typography: EdenTypography
        @Composable get() = LocalEdenTypography.current
}

@Composable
fun EdenTheme(
    colors: EdenColors = LocalEdenColors.current,
    typography: EdenTypography = LocalEdenTypography.current,
    content: @Composable () -> Unit,
) {
    CompositionLocalProvider(
        LocalEdenColors provides colors,
        LocalEdenTypography provides typography,
        content = content,
    )
}

data class EdenTypography(
    val title: TextStyle,
    val body: TextStyle,
    val label: TextStyle,
)
