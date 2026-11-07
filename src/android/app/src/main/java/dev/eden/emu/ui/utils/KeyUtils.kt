// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

package dev.eden.emu.ui.utils

import androidx.compose.ui.input.key.Key

val ConfirmKeys = setOf(Key.Enter, Key.DirectionCenter, Key.ButtonA)
val MenuKeys = setOf(Key.ButtonX, Key.Menu)
val BackKeys = setOf(Key.Escape, Key.Back, Key.ButtonB)

object NavKeys {
    val up = setOf(Key.DirectionUp, Key.W)
    val down = setOf(Key.DirectionDown, Key.S)
    val left = setOf(Key.DirectionLeft, Key.A)
    val right = setOf(Key.DirectionRight, Key.D)
}
