// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

package dev.eden.emu.ui.components

import androidx.annotation.DrawableRes
import androidx.compose.foundation.Image
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.text.BasicText
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.focus.FocusRequester
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.ColorFilter
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.painterResource
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import dev.eden.emu.ui.theme.Dimens
import org.yuzu.yuzu_emu.R

enum class FooterButton(@DrawableRes val iconRes: Int) {
    A(R.drawable.facebutton_a),
    B(R.drawable.facebutton_b),
    X(R.drawable.facebutton_x),
    Y(R.drawable.facebutton_y),
}

@Composable
fun HomeFooter(
    actions: List<FooterAction>,
    modifier: Modifier = Modifier,
) {
    Row(
        modifier = modifier
            .fillMaxWidth()
            .padding(horizontal = Dimens.paddingLg, vertical = 4.dp),
        horizontalArrangement = Arrangement.spacedBy(Dimens.paddingXl),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        actions.forEach { action ->
            Row(
                modifier = Modifier.padding(horizontal = Dimens.paddingXs, vertical = 2.dp),
                horizontalArrangement = Arrangement.spacedBy(6.dp),
                verticalAlignment = Alignment.CenterVertically,
            ) {
                Image(
                    painter = painterResource(id = action.button.iconRes),
                    contentDescription = null,
                    modifier = Modifier.size(24.dp),
                    colorFilter = ColorFilter.tint(Color.White.copy(alpha = 0.9f)),
                )
                BasicText(action.text, style = TextStyle(Color.White.copy(0.7f), 14.sp))
            }
        }
    }
}

data class FooterAction(val button: FooterButton, val text: String)
data class FooterFocusRequesters(val primaryAction: FocusRequester, val secondaryAction: FocusRequester)
