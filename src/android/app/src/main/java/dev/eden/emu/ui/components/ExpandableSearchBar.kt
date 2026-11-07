// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

package dev.eden.emu.ui.components

import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.focusable
import androidx.compose.foundation.interaction.MutableInteractionSource
import androidx.compose.foundation.interaction.collectIsFocusedAsState
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.text.BasicTextField
import androidx.compose.foundation.text.KeyboardActions
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.material3.Icon
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.focus.FocusRequester
import androidx.compose.ui.focus.focusRequester
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.SolidColor
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.input.key.Key
import androidx.compose.ui.input.key.KeyEventType
import androidx.compose.ui.input.key.key
import androidx.compose.ui.input.key.onPreviewKeyEvent
import androidx.compose.ui.input.key.type
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.LocalFocusManager
import androidx.compose.ui.platform.LocalSoftwareKeyboardController
import androidx.compose.ui.res.vectorResource
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.input.ImeAction
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import kotlinx.coroutines.delay
import org.yuzu.yuzu_emu.R
import dev.eden.emu.ui.theme.Dimens
import dev.eden.emu.ui.theme.EdenTheme
import dev.eden.emu.ui.utils.ConfirmKeys

/**
 * Expandable search bar for controller-based navigation.
 * ________________________________________________________________
 * Note: Focus Manager on this doesn't work good. Need a hide impl.
 * when it looses focus for better Controller Experience
 * ________________________________________________________________
 */
@Composable
fun ExpandableSearchBar(
    query: String,
    onQueryChange: (String) -> Unit,
    isExpanded: Boolean,
    onExpandedChange: (Boolean) -> Unit,
    focusRequester: FocusRequester,
    modifier: Modifier = Modifier,
) {
    val context = LocalContext.current
    val searchHint = context.getString(R.string.home_search_games)
    val keyboardController = LocalSoftwareKeyboardController.current
    val focusManager = LocalFocusManager.current

    val iconInteractionSource = remember { MutableInteractionSource() }
    val isIconFocused by iconInteractionSource.collectIsFocusedAsState()

    // Track if we have an active filter (submitted search, if keyboard just hides, it kinda breaks)
    val hasActiveFilter = query.isNotEmpty() && !isExpanded
    val textFieldFocusRequester = remember { FocusRequester() }

    // Focus text field when expanded
    LaunchedEffect(isExpanded) {
        if (isExpanded) {
            delay(100)
            try {
                textFieldFocusRequester.requestFocus()
                keyboardController?.show()
            } catch (_: Exception) {}
        }
    }

    // Use a fixed width container to prevent layout shift.
    Box(
        modifier = modifier.width(if (isExpanded) 300.dp else 48.dp),
        contentAlignment = Alignment.CenterEnd,
    ) {
        if (isExpanded) {
            Row(
                modifier = Modifier
                    .width(300.dp)
                    .clip(RoundedCornerShape(24.dp))
                    .background(EdenTheme.colors.surface)
                    .border(
                        width = 2.dp,
                        color = EdenTheme.colors.primary,
                        shape = RoundedCornerShape(24.dp)
                    ),
                verticalAlignment = Alignment.CenterVertically,
            ) {
                Spacer(modifier = Modifier.width(16.dp))

                // Text input - NO onFocusChanged to prevent auto-close issues
                // (Needs to be "submitted", hiding keyboard breaks focus flow)
                BasicTextField(
                    value = query,
                    onValueChange = onQueryChange,
                    modifier = Modifier
                        .weight(1f)
                        .focusRequester(textFieldFocusRequester)
                        .onPreviewKeyEvent { keyEvent ->
                            if (keyEvent.type == KeyEventType.KeyUp) {
                                when (keyEvent.key) {
                                    Key.Escape, Key.Back -> {
                                        onQueryChange("")
                                        onExpandedChange(false)
                                        keyboardController?.hide()
                                        focusManager.clearFocus()
                                        true
                                    }
                                    Key.Enter, Key.NumPadEnter -> {
                                        keyboardController?.hide()
                                        onExpandedChange(false)
                                        focusManager.clearFocus()
                                        true
                                    }
                                    else -> false
                                }
                            } else false
                        },
                    textStyle = TextStyle(
                        color = Color.White,
                        fontSize = 16.sp,
                    ),
                    cursorBrush = SolidColor(EdenTheme.colors.primary),
                    singleLine = true,
                    keyboardOptions = KeyboardOptions(imeAction = ImeAction.Search),
                    keyboardActions = KeyboardActions(
                        onSearch = {
                            keyboardController?.hide()
                            onExpandedChange(false)
                            focusManager.clearFocus()
                        }
                    ),
                    decorationBox = { innerTextField ->
                        Box(
                            modifier = Modifier.padding(vertical = 14.dp),
                            contentAlignment = Alignment.CenterStart,
                        ) {
                            if (query.isEmpty()) {
                                androidx.compose.foundation.text.BasicText(
                                    text = searchHint,
                                    style = TextStyle(
                                        color = Color.White.copy(alpha = 0.5f),
                                        fontSize = 16.sp,
                                    ),
                                    maxLines = 1,
                                    overflow = TextOverflow.Ellipsis,
                                )
                            }
                            innerTextField()
                        }
                    }
                )

                // Clear/Close button (X icon)
                Box(
                    modifier = Modifier
                        .size(40.dp)
                        .clip(CircleShape)
                        .clickable {
                            onQueryChange("")
                            onExpandedChange(false)
                            keyboardController?.hide()
                            focusManager.clearFocus()
                        }
                        .padding(8.dp),
                    contentAlignment = Alignment.Center,
                ) {
                    Icon(
                        imageVector = ImageVector.vectorResource(R.drawable.ic_clear),
                        contentDescription = context.getString(R.string.home_clear),
                        tint = Color.White.copy(alpha = 0.7f),
                        modifier = Modifier.size(20.dp),
                    )
                }

                Spacer(modifier = Modifier.width(4.dp))
            }
        } else {
            // Collapsed state: search icon
            Box(
                modifier = Modifier
                    .size(48.dp)
                    .clip(CircleShape)
                    .background(
                        if (isIconFocused) EdenTheme.colors.primary
                        else EdenTheme.colors.surface
                    )
                    .focusRequester(focusRequester)
                    .focusable(interactionSource = iconInteractionSource)
                    .clickable(
                        interactionSource = iconInteractionSource,
                        indication = null,
                    ) {
                        if (hasActiveFilter) {
                            onQueryChange("")
                        } else {
                            onExpandedChange(true)
                        }
                    }
                    .onPreviewKeyEvent { keyEvent ->
                        if (keyEvent.type == KeyEventType.KeyUp && keyEvent.key in ConfirmKeys) {
                            if (hasActiveFilter) {
                                onQueryChange("")
                            } else {
                                onExpandedChange(true)
                            }
                            true
                        } else false
                    },
                contentAlignment = Alignment.Center,
            ) {
                Icon(
                    imageVector = ImageVector.vectorResource(R.drawable.ic_search),
                    contentDescription = context.getString(R.string.home_search),
                    tint = if (hasActiveFilter) EdenTheme.colors.primary else Color.White,
                    modifier = Modifier.size(24.dp),
                )

                // Active filter indicator dot
                if (hasActiveFilter) {
                    Box(
                        modifier = Modifier
                            .align(Alignment.TopEnd)
                            .padding(6.dp)
                            .size(10.dp)
                            .clip(CircleShape)
                            .background(EdenTheme.colors.primary)
                    )
                }
            }
        }
    }
}
