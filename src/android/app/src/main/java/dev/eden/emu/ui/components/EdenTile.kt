// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

package dev.eden.emu.ui.components

import androidx.compose.foundation.BorderStroke
import androidx.compose.foundation.ExperimentalFoundationApi
import androidx.compose.foundation.MarqueeAnimationMode
import androidx.compose.foundation.background
import androidx.compose.foundation.basicMarquee
import androidx.compose.foundation.border
import androidx.compose.foundation.combinedClickable
import androidx.compose.foundation.focusable
import androidx.compose.foundation.interaction.MutableInteractionSource
import androidx.compose.foundation.interaction.collectIsFocusedAsState
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.text.BasicText
import androidx.compose.runtime.Composable
import androidx.compose.runtime.remember
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.draw.clipToBounds
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.input.key.key
import androidx.compose.ui.input.key.onPreviewKeyEvent
import androidx.compose.ui.input.key.type
import androidx.compose.ui.input.key.KeyEventType
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.semantics.role
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.compose.ui.viewinterop.AndroidView
import org.yuzu.yuzu_emu.R
import org.yuzu.yuzu_emu.model.Game
import org.yuzu.yuzu_emu.utils.GameIconUtils
import dev.eden.emu.ui.theme.EdenTheme
import dev.eden.emu.ui.theme.Shapes
import dev.eden.emu.ui.utils.ConfirmKeys
import dev.eden.emu.ui.utils.MenuKeys
import java.util.Locale

@OptIn(ExperimentalFoundationApi::class)
@Composable
fun EdenTile(
    title: String,
    onClick: () -> Unit,
    modifier: Modifier = Modifier,
    onLongClick: (() -> Unit)? = null,
    imageUri: String? = null,
    iconSize: Dp = 140.dp,
    tileHeight: Dp = 168.dp,
    useLargerFont: Boolean = false,
) {
    val interactionSource = remember { MutableInteractionSource() }
    val isFocused = interactionSource.collectIsFocusedAsState().value
    val borderColor = if (isFocused) EdenTheme.colors.primary else EdenTheme.colors.surface

    Column(
        modifier = modifier
            .width(iconSize)
            .height(tileHeight)
            .semantics { role = Role.Button }
            .onPreviewKeyEvent { e ->
                if (e.type == KeyEventType.KeyUp) {
                    when {
                        e.key in ConfirmKeys -> { onClick(); true }
                        e.key in MenuKeys -> { onLongClick?.invoke(); true }
                        else -> false
                    }
                } else false
            }
            .focusable(interactionSource = interactionSource)
            .combinedClickable(
                interactionSource = interactionSource,
                indication = null,
                onClick = onClick,
                onLongClick = onLongClick,
            ),
        horizontalAlignment = Alignment.CenterHorizontally,
    ) {
        // Game icon
        Box(
            modifier = Modifier
                .size(iconSize)
                .background(EdenTheme.colors.surface, Shapes.medium)
                .border(BorderStroke(3.dp, borderColor), Shapes.medium)
                .clip(Shapes.medium),
            contentAlignment = Alignment.Center,
        ) {
            if (imageUri.isNullOrBlank()) {
                BasicText(
                    text = title.take(1).uppercase(Locale.getDefault()),
                    style = EdenTheme.typography.title.copy(color = EdenTheme.colors.onSurface),
                )
            } else {
                AndroidView(
                    factory = { ctx ->
                        android.widget.ImageView(ctx).apply {
                            layoutParams = android.view.ViewGroup.LayoutParams(-1, -1)
                            scaleType = android.widget.ImageView.ScaleType.CENTER_CROP
                            setImageResource(R.drawable.default_icon)
                            GameIconUtils.loadGameIcon(Game(title, imageUri, "0", "", "", false), this)
                        }
                    },
                    modifier = Modifier.fillMaxSize()
                )
            }
        }

        // Game title with marquee
        MarqueeText(title, isFocused, 32.dp, useLargerFont, Modifier.width(iconSize))
    }
}

@Composable
private fun MarqueeText(
    text: String,
    isAnimating: Boolean,
    height: Dp,
    useLargerFont: Boolean,
    modifier: Modifier = Modifier,
) {
    val style = if (useLargerFont) {
        EdenTheme.typography.body.copy(Color.White, 18.sp, fontWeight = FontWeight.Bold)
    } else {
        EdenTheme.typography.label.copy(Color.White)
    }

    Box(modifier.height(height).clipToBounds(), Alignment.Center) {
        BasicText(
            text = text,
            style = style,
            maxLines = 1,
            softWrap = false,
            overflow = TextOverflow.Ellipsis,
            modifier = if (isAnimating) {
                Modifier.basicMarquee(
                    iterations = Int.MAX_VALUE,
                    animationMode = MarqueeAnimationMode.Immediately,
                    initialDelayMillis = 150, // does not seem to take effect
                    repeatDelayMillis = 1000,
                    velocity = 30.dp,
                )
            } else Modifier,
        )
    }
}
