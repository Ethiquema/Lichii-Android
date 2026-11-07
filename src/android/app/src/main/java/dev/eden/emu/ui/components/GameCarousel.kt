// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

package dev.eden.emu.ui.components

import androidx.compose.foundation.gestures.snapping.SnapPosition
import androidx.compose.foundation.gestures.snapping.rememberSnapFlingBehavior
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.lazy.LazyRow
import androidx.compose.foundation.lazy.itemsIndexed
import androidx.compose.foundation.lazy.rememberLazyListState
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.derivedStateOf
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.runtime.snapshotFlow
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.focus.FocusRequester
import androidx.compose.ui.focus.focusRequester
import androidx.compose.ui.focus.onFocusChanged
import androidx.compose.ui.graphics.graphicsLayer
import androidx.compose.ui.input.key.Key
import androidx.compose.ui.input.key.KeyEventType
import androidx.compose.ui.input.key.key
import androidx.compose.ui.input.key.onPreviewKeyEvent
import androidx.compose.ui.input.key.type
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp
import androidx.compose.ui.zIndex
import kotlin.math.abs
import kotlin.math.cos
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch

@Composable
fun GameCarousel(
    gameTiles: List<GameTile>,
    onGameClick: (GameTile) -> Unit,
    onGameLongClick: (GameTile) -> Unit = {},
    focusRequester: FocusRequester,
    onNavigateUp: () -> Unit = {},
    modifier: Modifier = Modifier,
    initialFocusedIndex: Int = 0,
    onFocusedIndexChanged: (Int) -> Unit = {},
    onShowGameInfo: (GameTile) -> Unit = {},
) {
    if (gameTiles.isEmpty()) return

    val safeInitialIndex = initialFocusedIndex.coerceIn(0, (gameTiles.size - 1).coerceAtLeast(0))

    val listState = rememberLazyListState(
        initialFirstVisibleItemIndex = safeInitialIndex,
        initialFirstVisibleItemScrollOffset = 0
    )
    val snapFlingBehavior = rememberSnapFlingBehavior(
        lazyListState = listState,
        snapPosition = SnapPosition.Center
    )
    val coroutineScope = rememberCoroutineScope()

    var centerItemIndex by remember { mutableIntStateOf(safeInitialIndex) }

    LaunchedEffect(centerItemIndex) {
        onFocusedIndexChanged(centerItemIndex)
    }

    suspend fun centerOnIndex(index: Int, animate: Boolean = true) {
        if (gameTiles.isEmpty()) return
        if (listState.layoutInfo.visibleItemsInfo.none { it.index == index }) {
            if (animate) {
                listState.animateScrollToItem(index)
            } else {
                listState.scrollToItem(index)
            }
        }
        delay(10)
        val layoutInfo = listState.layoutInfo
        val itemInfo = layoutInfo.visibleItemsInfo.firstOrNull { it.index == index } ?: return
        val viewportCenter = (layoutInfo.viewportStartOffset + layoutInfo.viewportEndOffset) / 2
        val desiredStart = viewportCenter - itemInfo.size / 2
        if (itemInfo.offset != desiredStart) {
            val scrollOffset = -(viewportCenter - itemInfo.size / 2)
            if (animate) {
                listState.animateScrollToItem(index, scrollOffset = scrollOffset)
            } else {
                listState.scrollToItem(index, scrollOffset = scrollOffset)
            }
        }
    }

    LaunchedEffect(Unit) {
        listState.scrollToItem(safeInitialIndex)
        delay(50)
        centerOnIndex(safeInitialIndex, animate = false)
    }

    val focusRequesters = remember(gameTiles.size) {
        List(gameTiles.size) { FocusRequester() }
    }

    var isFocusTriggeredScroll by remember { mutableStateOf(false) }

    LaunchedEffect(listState) {
        snapshotFlow { listState.isScrollInProgress }
            .collect { isScrolling ->
                if (!isScrolling && !isFocusTriggeredScroll && gameTiles.isNotEmpty()) {
                    val firstVisible = listState.firstVisibleItemIndex
                    val scrollOffset = listState.firstVisibleItemScrollOffset
                    val layoutInfo = listState.layoutInfo
                    val firstVisibleItem = layoutInfo.visibleItemsInfo.firstOrNull()
                    val itemSize = firstVisibleItem?.size ?: 1
                    val newCenterIndex = if (scrollOffset > itemSize / 2) {
                        (firstVisible + 1).coerceAtMost(gameTiles.size - 1)
                    } else {
                        firstVisible
                    }
                    if (newCenterIndex != centerItemIndex && newCenterIndex in focusRequesters.indices) {
                        centerItemIndex = newCenterIndex
                        try {
                            focusRequesters[newCenterIndex].requestFocus()
                        } catch (_: Exception) { }
                    }
                }
                // Reset the flag when scroll stops
                if (!isScrolling) {
                    isFocusTriggeredScroll = false
                }
            }
    }

    // Carousel settings (matching original CarouselRecyclerView)
    val borderScale = 0.6f
    val borderAlpha = 0.35f
    val overlapFactor = 0.15f

    BoxWithConstraints(
        modifier = modifier
            .fillMaxSize(),
        contentAlignment = Alignment.Center,
    ) {
        val density = LocalDensity.current
        val screenWidthPx = with(density) { maxWidth.toPx() }
        val screenHeightPx = with(density) { maxHeight.toPx() }

        val tileTextHeight = 40.dp
        val tileSpacing = 6.dp

        // Card size
        val totalTextAreaPx = with(density) { (tileTextHeight + tileSpacing).toPx() }
        val cardSizePx = (screenHeightPx - totalTextAreaPx) * 0.7f
        val cardSize = with(density) { cardSizePx.toDp() }
        val tileHeight = cardSize + tileSpacing + tileTextHeight

        // Horizontal padding to center first/last card
        val horizontalPaddingPx = (screenWidthPx - cardSizePx) / 2f
        val horizontalPadding = with(density) { horizontalPaddingPx.toDp() }

        // Negative spacing for overlap, no overlap currently tho
        val overlapPx = cardSizePx * overlapFactor
        val itemSpacing = with(density) { (-overlapPx).toDp() }

        val centerPushPx = cardSizePx * 0.12f

        val screenCenterPx = screenWidthPx / 2f

        val itemWidthWithSpacing = cardSizePx - overlapPx

        LazyRow(
            state = listState,
            modifier = Modifier.fillMaxSize(),
            flingBehavior = snapFlingBehavior,
            horizontalArrangement = Arrangement.spacedBy(itemSpacing),
            contentPadding = PaddingValues(start = horizontalPadding, end = horizontalPadding, top = 30.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            itemsIndexed(gameTiles, key = { _, tile -> tile.id }) { index, tile ->
                val distanceFromCenter by remember {
                    derivedStateOf {
                        val itemInfo = listState.layoutInfo.visibleItemsInfo.find { it.index == index }
                        if (itemInfo == null) {
                            1f
                        } else {
                            val itemLeftInScreen = itemInfo.offset.toFloat() + horizontalPaddingPx
                            val itemCenterInScreen = itemLeftInScreen + itemInfo.size / 2f

                            val distance = abs(itemCenterInScreen - screenCenterPx)

                            (distance / screenCenterPx).coerceIn(0f, 1f)
                        }
                    }
                }

                val shapedScale = cos(distanceFromCenter * Math.PI * 0.5).toFloat().coerceIn(0f, 1f)
                val scale = borderScale + (1f - borderScale) * shapedScale

                val shapedAlpha = cos(distanceFromCenter * Math.PI * 0.5).toFloat().coerceIn(0f, 1f)
                val alpha = borderAlpha + (1f - borderAlpha) * shapedAlpha

                val zIndex = 100f * (1f - distanceFromCenter)

                val itemInfo = listState.layoutInfo.visibleItemsInfo.find { it.index == index }
                val isLeftOfCenter = if (itemInfo != null) {
                    val itemLeftInScreen = itemInfo.offset.toFloat() + horizontalPaddingPx
                    val itemCenterInScreen = itemLeftInScreen + itemInfo.size / 2f
                    itemCenterInScreen < screenCenterPx
                } else {
                    index < (gameTiles.size / 2)
                }
                val pushFactor = (1f - distanceFromCenter) * distanceFromCenter * 4f
                val horizontalPush = centerPushPx * pushFactor * (if (isLeftOfCenter) -1f else 1f)

                val tileModifier = if (index == centerItemIndex) {
                    Modifier
                        .focusRequester(focusRequester)
                        .focusRequester(focusRequesters[index])
                } else {
                    Modifier.focusRequester(focusRequesters[index])
                }

                Box(
                    modifier = Modifier
                        .size(width = cardSize, height = tileHeight)
                        .zIndex(zIndex)
                        .onFocusChanged { focusState ->
                            if (focusState.hasFocus && centerItemIndex != index) {
                                isFocusTriggeredScroll = true
                                centerItemIndex = index
                                coroutineScope.launch {
                                    centerOnIndex(index, animate = true)
                                }
                            }
                        }
                        .onPreviewKeyEvent { keyEvent ->
                            if (keyEvent.type == KeyEventType.KeyDown) {
                                when (keyEvent.key) {
                                    Key.ButtonX -> {
                                        onShowGameInfo(tile)
                                        true
                                    }
                                    Key.DirectionUp, Key.DirectionUpLeft, Key.DirectionUpRight -> {
                                        onNavigateUp()
                                        true
                                    }
                                    Key.DirectionLeft -> {
                                        if (index > 0) {
                                            focusRequesters[index - 1].requestFocus()
                                        }
                                        true
                                    }
                                    Key.DirectionRight -> {
                                        if (index < gameTiles.size - 1) {
                                            focusRequesters[index + 1].requestFocus()
                                        }
                                        true
                                    }
                                    else -> false
                                }
                            } else {
                                false
                            }
                        }
                        .graphicsLayer {
                            scaleX = scale
                            scaleY = scale
                            this.alpha = alpha
                            translationX = horizontalPush
                        },
                    contentAlignment = Alignment.Center,
                ) {
                    EdenTile(
                        title = tile.title,
                        iconSize = cardSize,
                        tileHeight = tileHeight,
                        onClick = {
                            if (index == centerItemIndex) {
                                onGameClick(tile)
                            } else {
                                isFocusTriggeredScroll = true
                                centerItemIndex = index
                                coroutineScope.launch {
                                    centerOnIndex(index, animate = true)
                                }
                                focusRequesters[index].requestFocus()
                            }
                        },
                        onLongClick = {
                            if (index == centerItemIndex) {
                                onGameLongClick(tile)
                            } else {
                                isFocusTriggeredScroll = true
                                centerItemIndex = index
                                coroutineScope.launch {
                                    centerOnIndex(index, animate = true)
                                    onGameLongClick(tile)
                                }
                                focusRequesters[index].requestFocus()
                            }
                        },
                        modifier = tileModifier,
                        imageUri = tile.iconUri,
                        useLargerFont = true,
                    )
                }
            }
        }
    }
}
