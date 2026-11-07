// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

package dev.eden.emu.ui.components

import android.graphics.BitmapFactory
import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.focusable
import androidx.compose.foundation.interaction.MutableInteractionSource
import androidx.compose.foundation.interaction.collectIsFocusedAsState
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.text.BasicText
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
import androidx.compose.ui.graphics.asImageBitmap
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.input.key.key
import androidx.compose.ui.input.key.onPreviewKeyEvent
import androidx.compose.ui.input.key.type
import androidx.compose.ui.input.key.KeyEventType
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.vectorResource
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import org.yuzu.yuzu_emu.NativeLibrary
import org.yuzu.yuzu_emu.R
import dev.eden.emu.ui.theme.Dimens
import dev.eden.emu.ui.theme.EdenTheme
import dev.eden.emu.ui.theme.Shapes
import dev.eden.emu.ui.utils.ConfirmKeys
import java.io.File

@Composable
fun HomeHeader(
    currentUser: String,
    onUserClick: () -> Unit,
    searchQuery: String,
    onSearchQueryChange: (String) -> Unit,
    isSearchExpanded: Boolean,
    onSearchExpandedChange: (Boolean) -> Unit,
    onSortClick: () -> Unit,
    onFilterClick: () -> Unit,
    onSettingsClick: () -> Unit,
    focusRequesters: HeaderFocusRequesters,
    modifier: Modifier = Modifier,
) {
    val context = LocalContext.current
    var username by remember { mutableStateOf("Eden") }
    var imagePath by remember { mutableStateOf<String?>(null) }

    LaunchedEffect(currentUser) {
        val uuid = currentUser.ifEmpty { NativeLibrary.getCurrentUser() ?: "" }
        if (uuid.isNotEmpty()) {
            username = NativeLibrary.getUserUsername(uuid) ?: "Eden"
            imagePath = NativeLibrary.getUserImagePath(uuid)
        }
    }

    Row(
        modifier = modifier
            .fillMaxWidth()
            .padding(horizontal = Dimens.paddingXl, vertical = Dimens.paddingMd),
        horizontalArrangement = Arrangement.SpaceBetween,
        verticalAlignment = Alignment.CenterVertically,
    ) {
        UserProfileButton(username, imagePath, onUserClick, focusRequesters.userButton)

        Row(
            horizontalArrangement = Arrangement.spacedBy(Dimens.paddingLg),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            ExpandableSearchBar(searchQuery, onSearchQueryChange, isSearchExpanded, onSearchExpandedChange, focusRequesters.searchButton)
            HeaderIconButton(R.drawable.ic_sort, context.getString(R.string.home_sort), onSortClick, focusRequesters.sortButton)
            HeaderIconButton(R.drawable.ic_filter, context.getString(R.string.home_layout), onFilterClick, focusRequesters.filterButton)
            HeaderIconButton(R.drawable.ic_settings, context.getString(R.string.home_settings), onSettingsClick, focusRequesters.settingsButton)
        }
    }
}

@Composable
private fun UserProfileButton(
    username: String,
    imagePath: String?,
    onClick: () -> Unit,
    focusRequester: FocusRequester,
) {
    val interactionSource = remember { MutableInteractionSource() }
    val isFocused by interactionSource.collectIsFocusedAsState()

    val bitmap = remember(imagePath) {
        imagePath?.takeIf { it.isNotEmpty() }?.let { path ->
            File(path).takeIf { it.exists() }?.let { BitmapFactory.decodeFile(path) }
        }
    }

    val defaultBitmap = remember {
        NativeLibrary.getDefaultAccountBackupJpeg().let { BitmapFactory.decodeByteArray(it, 0, it.size) }
    }

    Row(
        modifier = Modifier
            .focusRequester(focusRequester)
            .focusable(interactionSource = interactionSource)
            .onPreviewKeyEvent { e ->
                if (e.type == KeyEventType.KeyDown && e.key in ConfirmKeys) { onClick(); true } else false
            }
            .clickable(interactionSource, null, onClick = onClick)
            .background(if (isFocused) EdenTheme.colors.primary else EdenTheme.colors.surface, Shapes.medium)
            .border(Dimens.borderWidth, if (isFocused) EdenTheme.colors.primary else EdenTheme.colors.surface, Shapes.medium)
            .padding(horizontal = Dimens.paddingMd, vertical = 6.dp),
        horizontalArrangement = Arrangement.spacedBy(Dimens.paddingMd),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        (bitmap ?: defaultBitmap)?.let { bmp ->
            Image(
                bitmap = bmp.asImageBitmap(),
                contentDescription = null,
                contentScale = ContentScale.Crop,
                modifier = Modifier.size(46.dp).clip(CircleShape)
            )
        }
        BasicText(username, style = EdenTheme.typography.body.copy(fontSize = 18.sp, color = Color.White))
    }
}

@Composable
fun HeaderIconButton(
    iconRes: Int,
    contentDescription: String,
    onClick: () -> Unit,
    focusRequester: FocusRequester,
    modifier: Modifier = Modifier,
) {
    val interactionSource = remember { MutableInteractionSource() }
    val isFocused by interactionSource.collectIsFocusedAsState()

    Box(
        modifier = modifier
            .size(Dimens.iconLg)
            .focusRequester(focusRequester)
            .focusable(interactionSource = interactionSource)
            .onPreviewKeyEvent { e ->
                if (e.type == KeyEventType.KeyDown && e.key in ConfirmKeys) { onClick(); true } else false
            }
            .clip(CircleShape)
            .background(if (isFocused) EdenTheme.colors.primary else EdenTheme.colors.surface)
            .clickable(interactionSource, null, onClick = onClick),
        contentAlignment = Alignment.Center,
    ) {
        Icon(
            imageVector = ImageVector.vectorResource(iconRes),
            contentDescription = contentDescription,
            tint = Color.White,
            modifier = Modifier.size(Dimens.iconSm),
        )
    }
}

data class HeaderFocusRequesters(
    val userButton: FocusRequester,
    val searchButton: FocusRequester,
    val sortButton: FocusRequester,
    val filterButton: FocusRequester,
    val settingsButton: FocusRequester,
)
