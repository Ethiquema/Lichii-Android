// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

package dev.eden.emu.ui.theme

import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.ui.unit.dp

// Grid background
val EdenGridSpacing = 58.dp
val EdenGridLineWidth = 2.dp
const val EdenGridSpeedDpPerSecond = 12f

// Common dimensions
object Dimens {
    // Padding
    val paddingXs = 4.dp
    val paddingSm = 8.dp
    val paddingMd = 12.dp
    val paddingLg = 16.dp
    val paddingXl = 24.dp

    // Icon sizes
    val iconSm = 24.dp
    val iconMd = 32.dp
    val iconLg = 48.dp

    // Border
    val borderWidth = 2.dp

    // Corner radius
    val radiusSm = 8.dp
    val radiusMd = 12.dp
    val radiusLg = 16.dp
}

// Common shapes
object Shapes {
    val small = RoundedCornerShape(Dimens.radiusSm)
    val medium = RoundedCornerShape(Dimens.radiusMd)
    val large = RoundedCornerShape(Dimens.radiusLg)
}
