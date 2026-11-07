// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

package dev.eden.emu.ui.components

import android.util.Log
import androidx.compose.foundation.background
import androidx.compose.foundation.focusable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.fillMaxHeight
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.lazy.grid.GridCells
import androidx.compose.foundation.lazy.grid.LazyHorizontalGrid
import androidx.compose.foundation.lazy.grid.itemsIndexed
import androidx.compose.foundation.lazy.grid.rememberLazyGridState
import androidx.compose.foundation.text.BasicText
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.focus.FocusRequester
import androidx.compose.ui.focus.focusRequester
import androidx.compose.ui.focus.onFocusChanged
import androidx.compose.ui.input.key.Key
import androidx.compose.ui.input.key.KeyEventType
import androidx.compose.ui.input.key.key
import androidx.compose.ui.input.key.onPreviewKeyEvent
import androidx.compose.ui.input.key.type
import androidx.compose.ui.platform.LocalContext
import org.yuzu.yuzu_emu.R
import dev.eden.emu.ui.theme.EdenTheme
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp

@Composable
fun GameGrid(
    gameTiles: List<GameTile>,
    onGameClick: (GameTile) -> Unit,
    onGameLongClick: (GameTile) -> Unit = {},
    onNavigateToSettings: () -> Unit = {},
    focusRequester: FocusRequester,
    onNavigateUp: () -> Unit = {},
    rowCount: Int = 2,
    tileIconSize: Dp = 100.dp,
    paddingTop: Dp = 0.dp,
    modifier: Modifier = Modifier,
    isLoading: Boolean = false,
    emptyMessage: String? = null,
    initialFocusedIndex: Int = 0,
    onFocusedIndexChanged: (Int) -> Unit = {},
    onShowGameInfo: (GameTile) -> Unit = {},
) {
    Box(
        modifier = modifier
            .fillMaxSize()
    ) {

        when {
            isLoading -> LoadingMessage()
            emptyMessage != null -> EmptyMessage(emptyMessage)
            else -> {
                GameGridContent(
                    gameTiles = gameTiles,
                    onGameClick = onGameClick,
                    onGameLongClick = onGameLongClick,
                    focusRequester = focusRequester,
                    onNavigateUp = onNavigateUp,
                    rowCount = rowCount,
                    tileIconSize = tileIconSize,
                    paddingTop = paddingTop,
                    initialFocusedIndex = initialFocusedIndex,
                    onFocusedIndexChanged = onFocusedIndexChanged,
                    onXPress = { index ->
                        gameTiles.getOrNull(index)?.let { tile ->
                            onShowGameInfo(tile)
                        }
                    },
                )
            }
        }
    }
}

@Composable
private fun GameGridContent(
    gameTiles: List<GameTile>,
    onGameClick: (GameTile) -> Unit,
    onGameLongClick: (GameTile) -> Unit = {},
    focusRequester: FocusRequester,
    onNavigateUp: () -> Unit,
    rowCount: Int,
    tileIconSize: Dp,
    paddingTop: Dp,
    initialFocusedIndex: Int = 0,
    onFocusedIndexChanged: (Int) -> Unit = {},
    onXPress: (Int) -> Unit = {},
) {
    val safeInitialIndex = initialFocusedIndex.coerceIn(0, (gameTiles.size - 1).coerceAtLeast(0))
    var focusedIndex by remember { mutableIntStateOf(safeInitialIndex) }

    val gridState = rememberLazyGridState(
        initialFirstVisibleItemIndex = safeInitialIndex
    )

    LaunchedEffect(gameTiles.size) {
        if (focusedIndex >= gameTiles.size) {
            focusedIndex = 0
            gridState.scrollToItem(0)
        }
    }

    val tileFocusRequesters = remember(gameTiles.size) {
        List(gameTiles.size) { FocusRequester() }
    }

    LaunchedEffect(focusRequester) {
        if (tileFocusRequesters.isNotEmpty()) {
            focusRequester.requestFocus()
        }
    }

    LaunchedEffect(Unit) {
        if (tileFocusRequesters.isNotEmpty() && safeInitialIndex in tileFocusRequesters.indices) {
            gridState.scrollToItem(safeInitialIndex)
            tileFocusRequesters[safeInitialIndex].requestFocus()
        }
    }

    LaunchedEffect(focusedIndex) {
        if (focusedIndex in tileFocusRequesters.indices) {
            tileFocusRequesters[focusedIndex].requestFocus()
            onFocusedIndexChanged(focusedIndex)
        }
    }

    val outerPaddingTop = 62.dp
    val outerPaddingBottom = 16.dp
    val outerPaddingHorizontal = 0.dp
    val gridVerticalSpacing = 0.dp
    val gridVerticalPadding = 12.dp
    val spacerHeight = 2.dp
    val minTextHeight = 42.dp

    BoxWithConstraints(
        modifier = Modifier
            .fillMaxSize()
            .padding(
                top = outerPaddingTop,
                start = outerPaddingHorizontal,
                end = outerPaddingHorizontal,
                bottom = outerPaddingBottom
            )
            .onPreviewKeyEvent { keyEvent ->
                if (keyEvent.type == KeyEventType.KeyDown && gameTiles.isNotEmpty()) {
                    val currentRow = focusedIndex % rowCount

                    when (keyEvent.key) {
                        Key.ButtonX -> {
                            onXPress(focusedIndex)
                            true
                        }
                        Key.DirectionRight, Key.D -> {
                            val nextIndex = focusedIndex + rowCount
                            if (nextIndex < gameTiles.size) {
                                focusedIndex = nextIndex
                            }
                            true
                        }
                        Key.DirectionLeft, Key.A -> {
                            val prevIndex = focusedIndex - rowCount
                            if (prevIndex >= 0) {
                                focusedIndex = prevIndex
                            }
                            true
                        }
                        Key.DirectionDown, Key.S -> {
                            val nextIndex = focusedIndex + 1
                            if (nextIndex < gameTiles.size && nextIndex % rowCount != 0) {
                                focusedIndex = nextIndex
                            }
                            true
                        }
                        Key.DirectionUp, Key.W -> {
                            if (currentRow > 0) {
                                focusedIndex -= 1
                                true
                            } else {
                                onNavigateUp()
                                true
                            }
                        }
                        else -> false
                    }
                } else {
                    false
                }
            },
        contentAlignment = Alignment.TopStart
    ) {
        val availableHeight = maxHeight
        val heightForRows = (availableHeight - (gridVerticalPadding * 2) -
            (gridVerticalSpacing * (rowCount - 1))).coerceAtLeast(0.dp)
        val perRowHeight = heightForRows.safeDiv(rowCount)
        val iconMaxSize = tileIconSize * 2
        val iconSizeForGrid = (perRowHeight - spacerHeight - minTextHeight)
            .coerceAtLeast(96.dp)
            .coerceAtMost(iconMaxSize)
        val tileHeight = perRowHeight.coerceAtLeast(iconSizeForGrid + spacerHeight + minTextHeight)

        LazyHorizontalGrid(
            rows = GridCells.Fixed(rowCount),
            state = gridState,
            modifier = Modifier
                .fillMaxWidth()
                .fillMaxHeight(),
            horizontalArrangement = Arrangement.spacedBy(12.dp),
            contentPadding = PaddingValues(horizontal = 24.dp, vertical = gridVerticalPadding),
        ) {
            itemsIndexed(gameTiles, key = { _, tile -> tile.id }) { index, tile ->
                val tileModifier = if (index == focusedIndex) {
                    Modifier
                        .focusRequester(focusRequester)
                        .focusRequester(tileFocusRequesters[index])
                } else {
                    Modifier.focusRequester(tileFocusRequesters[index])
                }

                EdenTile(
                    title = tile.title,
                    iconSize = iconSizeForGrid,
                    tileHeight = tileHeight,
                    onClick = {
                        focusedIndex = index
                        onGameClick(tile)
                    },
                    onLongClick = {
                        focusedIndex = index
                        onGameLongClick(tile)
                    },
                    modifier = tileModifier,
                    imageUri = tile.iconUri,
                )
            }
        }
    }
}

@Composable
private fun LoadingMessage() {
    val context = LocalContext.current
    Column(
        modifier = Modifier
            .fillMaxSize()
            .padding(16.dp),
        horizontalAlignment = Alignment.CenterHorizontally,
        verticalArrangement = Arrangement.Center,
    ) {
        CircularProgressIndicator(
            modifier = Modifier.padding(bottom = 16.dp),
            color = EdenTheme.colors.primary,
        )
        BasicText(
            text = context.getString(R.string.loading),
            style = EdenTheme.typography.body.copy(color = EdenTheme.colors.onBackground),
        )
    }
}

@Composable
private fun EmptyMessage(message: String) {
    val context = LocalContext.current
    Column(
        modifier = Modifier
            .fillMaxSize()
            .padding(16.dp),
        horizontalAlignment = Alignment.CenterHorizontally,
        verticalArrangement = Arrangement.Center,
    ) {
        BasicText(
            text = message,
            style = EdenTheme.typography.body.copy(color = EdenTheme.colors.onBackground),
        )
        BasicText(
            text = context.getString(R.string.manage_game_folders),
            style = EdenTheme.typography.label.copy(color = EdenTheme.colors.onBackground),
            modifier = Modifier.padding(top = 8.dp)
        )
    }
}

private fun Dp.safeDiv(divisor: Int): Dp = if (divisor > 0) this / divisor else 0.dp
