// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

package dev.eden.emu.ui.utils

import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.focusable
import androidx.compose.foundation.interaction.MutableInteractionSource
import androidx.compose.foundation.interaction.collectIsFocusedAsState
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.text.BasicText
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.remember
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.focus.FocusRequester
import androidx.compose.ui.focus.focusRequester
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.input.key.Key
import androidx.compose.ui.input.key.KeyEventType
import androidx.compose.ui.input.key.key
import androidx.compose.ui.input.key.onPreviewKeyEvent
import androidx.compose.ui.input.key.type
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import dev.eden.emu.ui.theme.Dimens
import dev.eden.emu.ui.theme.EdenTheme
import dev.eden.emu.ui.theme.Shapes

/**
 * New dialog button for consistent styling across dialogs
 */
@Composable
fun DialogButton(
    text: String,
    onClick: () -> Unit,
    focusRequester: FocusRequester,
    modifier: Modifier = Modifier,
    isSelected: Boolean = false,
    isSecondary: Boolean = false,
) {
    val interactionSource = remember { MutableInteractionSource() }
    val isFocused by interactionSource.collectIsFocusedAsState()

    val backgroundColor = when {
        isFocused -> EdenTheme.colors.primary
        isSelected -> EdenTheme.colors.primary.copy(alpha = 0.3f)
        isSecondary -> Color.Transparent
        else -> EdenTheme.colors.background
    }

    val borderColor = when {
        isFocused || isSelected -> EdenTheme.colors.primary
        isSecondary -> EdenTheme.colors.onBackground.copy(alpha = 0.3f)
        else -> EdenTheme.colors.onBackground.copy(alpha = 0.2f)
    }

    Box(
        modifier = modifier
            .fillMaxWidth()
            .clip(Shapes.medium)
            .background(backgroundColor)
            .border(Dimens.borderWidth, borderColor, Shapes.medium)
            .focusRequester(focusRequester)
            .focusable(interactionSource = interactionSource)
            .onPreviewKeyEvent { event ->
                if (event.type == KeyEventType.KeyUp && event.key in confirmKeys) {
                    onClick()
                    true
                } else false
            }
            .clickable(interactionSource, null, onClick = onClick)
            .padding(vertical = 14.dp, horizontal = 20.dp),
        contentAlignment = Alignment.Center,
    ) {
        BasicText(
            text = if (isSelected && !isSecondary) "✓ $text" else text,
            style = EdenTheme.typography.body.copy(color = Color.White, fontSize = 16.sp)
        )
    }
}

private val confirmKeys = setOf(Key.Enter, Key.DirectionCenter, Key.ButtonA)
