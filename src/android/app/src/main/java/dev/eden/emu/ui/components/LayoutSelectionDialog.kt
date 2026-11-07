// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

package dev.eden.emu.ui.components

import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.text.BasicText
import androidx.compose.runtime.Composable
import androidx.compose.runtime.remember
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.focus.FocusRequester
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.sp
import androidx.compose.ui.window.Dialog
import androidx.compose.ui.window.DialogProperties
import org.yuzu.yuzu_emu.R
import dev.eden.emu.ui.theme.Dimens
import dev.eden.emu.ui.theme.EdenTheme
import dev.eden.emu.ui.theme.Shapes
import dev.eden.emu.ui.utils.DialogButton

enum class LayoutMode { TWO_ROW, CAROUSEL /*, ONE_ROW  */ }

@Composable
fun LayoutSelectionDialog(
    onDismiss: () -> Unit,
    onSelectGrid: () -> Unit,
    onSelectCarousel: () -> Unit,
) {
    val context = LocalContext.current
    val focusRequesters = remember { List(3) { FocusRequester() } }

    Dialog(onDismissRequest = onDismiss, properties = DialogProperties(dismissOnBackPress = true, dismissOnClickOutside = true)) {
        Column(
            modifier = Modifier
                .clip(Shapes.large)
                .background(EdenTheme.colors.surface)
                .padding(Dimens.paddingXl),
            horizontalAlignment = Alignment.CenterHorizontally,
        ) {
            BasicText(
                text = context.getString(R.string.home_layout),
                style = EdenTheme.typography.title.copy(color = Color.White, fontSize = 20.sp, fontWeight = FontWeight.Bold)
            )

            Spacer(Modifier.height(Dimens.paddingXl))

            DialogButton(
                text = context.getString(R.string.view_grid),
                onClick = { onSelectGrid(); onDismiss() },
                focusRequester = focusRequesters[0],
            )

            Spacer(Modifier.height(Dimens.paddingMd))

            DialogButton(
                text = context.getString(R.string.view_carousel),
                onClick = { onSelectCarousel(); onDismiss() },
                focusRequester = focusRequesters[1],
            )

            Spacer(Modifier.height(Dimens.paddingXl))

            DialogButton(
                text = context.getString(R.string.cancel),
                onClick = onDismiss,
                focusRequester = focusRequesters[2],
                isSecondary = true,
            )
        }
    }
}
