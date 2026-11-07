// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

package dev.eden.emu.ui.background

import androidx.compose.animation.core.LinearEasing
import androidx.compose.animation.core.RepeatMode
import androidx.compose.animation.core.animateFloat
import androidx.compose.animation.core.infiniteRepeatable
import androidx.compose.animation.core.rememberInfiniteTransition
import androidx.compose.animation.core.tween
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.offset
import androidx.compose.foundation.layout.requiredHeight
import androidx.compose.foundation.layout.requiredWidth
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.remember
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.drawWithCache
import androidx.compose.ui.draw.drawWithContent
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.drawscope.clipRect
import androidx.compose.ui.graphics.graphicsLayer
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp
import coil.compose.AsyncImage
import coil.decode.SvgDecoder
import coil.request.ImageRequest
import dev.eden.emu.ui.theme.EdenTheme
import kotlin.math.pow

@Composable
fun RetroGridBackground(
    modifier: Modifier = Modifier,
    lineColor: Color = EdenTheme.colors.primary.copy(alpha = 0.2f),
    spacing: Dp = 40.dp,
    lineWidth: Dp = 2.dp,
    speedDpPerSecond: Float = 12f,
    fadeColor: Color = EdenTheme.colors.background,
    logoScale: Float = 0.35f,
    logoAlpha: Float = 0.25f,
    horizonRatio: Float = 0.45f,
    logoAssetPath: String = "file:///android_asset/base.svg", // correct way?
    logoWidth: Dp? = null,
    logoHeight: Dp? = null,
    logoCropBottomRatio: Float = 0.33f,
) {
    val context = LocalContext.current
    val density = LocalDensity.current
    val durationMs = remember(spacing, speedDpPerSecond, density) {
        val spacingPx = spacing.value * density.density
        val speedPxPerSecond = speedDpPerSecond * density.density
        ((spacingPx / speedPxPerSecond) * 1000f).toInt().coerceAtLeast(300)
    }

    val transition = rememberInfiniteTransition(label = "grid")
    val phase by transition.animateFloat(
        initialValue = 0f,
        targetValue = 1f,
        animationSpec = infiniteRepeatable(
            animation = tween(durationMillis = durationMs, easing = LinearEasing),
            repeatMode = RepeatMode.Restart,
        ),
        label = "gridPhase",
    )

    BoxWithConstraints(modifier = modifier.fillMaxSize()) {
        val baseSize = maxWidth * logoScale
        val resolvedWidth = logoWidth ?: baseSize
        val resolvedHeight = logoHeight ?: baseSize
        val cropRatio = logoCropBottomRatio.coerceIn(0f, 0.9f)
        val visibleHeight = resolvedHeight * (1f - cropRatio)
        val visibleHeightPx = with(density) { visibleHeight.toPx() }
        val horizonY = maxHeight * horizonRatio
        val logoOffsetY = horizonY - (resolvedHeight * 0.5f) - 50.dp

        Box(modifier = Modifier.fillMaxSize()) {
            Box(
                modifier = Modifier
                    .fillMaxSize()
                    .drawWithCache {
                        val spacingPx = spacing.toPx().coerceAtLeast(8f)
                        val strokePx = lineWidth.toPx().coerceAtLeast(1f)
                        val horizonScale = 0.18f
                        val overscan = 5.0f
                        val perspectivePower = 2.2f
                        onDrawBehind {
                            val width = size.width
                            val height = size.height
                            val centerX = width * 0.5f
                            val horizonYpx = height * horizonRatio
                            val depth = (height - horizonYpx).coerceAtLeast(1f)
                            val offset = (1f - phase) * spacingPx
                            val baseHalf = centerX * overscan
                            val horizonHalf = baseHalf * horizonScale
                            val lineCount = (depth / (spacingPx * 0.65f))
                                .toInt()
                                .coerceAtLeast(20)

                            var x = centerX - baseHalf - spacingPx * 6f
                            while (x <= centerX + baseHalf + spacingPx * 6f) {
                                val endX = centerX + (x - centerX) * horizonScale
                                drawLine(
                                    color = lineColor,
                                    start = Offset(x, height),
                                    end = Offset(endX, horizonYpx),
                                    strokeWidth = strokePx,
                                )
                                x += spacingPx
                            }

                            var i = 0
                            while (i <= lineCount) {
                                val z = ((i + (offset / spacingPx)) / lineCount.toFloat())
                                    .coerceIn(0f, 1f)
                                val zCurve = z.toDouble().pow(perspectivePower.toDouble()).toFloat()
                                val y = horizonYpx + depth * zCurve
                                val halfWidth = horizonHalf + (baseHalf - horizonHalf) * z
                                drawLine(
                                    color = lineColor,
                                    start = Offset(centerX - halfWidth, y),
                                    end = Offset(centerX + halfWidth, y),
                                    strokeWidth = strokePx,
                                )
                                i += 1
                            }

                            val fadeHeight = height * 0.12f
                            val solidHeight = (horizonYpx - fadeHeight).coerceAtLeast(0f)
                            if (solidHeight > 0f) {
                                drawRect(
                                    color = fadeColor,
                                    topLeft = Offset(0f, 0f),
                                    size = Size(width, solidHeight),
                                )
                            }
                            drawRect(
                                brush = Brush.verticalGradient(
                                    colors = listOf(fadeColor, Color.Transparent),
                                    startY = horizonYpx - fadeHeight,
                                    endY = horizonYpx + fadeHeight,
                                ),
                                topLeft = Offset(0f, horizonYpx - fadeHeight),
                                size = Size(width, fadeHeight * 2f),
                            )
                        }
                    },
            )

            AsyncImage(
                model = ImageRequest.Builder(context)
                    .data(logoAssetPath)
                    .decoderFactory(SvgDecoder.Factory())
                    .build(),
                contentDescription = null,
                modifier = Modifier
                    .align(Alignment.TopCenter)
                    .requiredWidth(resolvedWidth)
                    .requiredHeight(resolvedHeight)
                    .offset(y = logoOffsetY)
                    .drawWithContent {
                        clipRect(
                            left = 0f,
                            top = 0f,
                            right = size.width,
                            bottom = visibleHeightPx,
                        ) {
                            this@drawWithContent.drawContent()
                        }
                    }
                    .graphicsLayer(alpha = logoAlpha),
            )
        }
    }
}
