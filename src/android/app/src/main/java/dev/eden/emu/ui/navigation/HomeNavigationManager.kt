// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

package dev.eden.emu.ui.navigation

import androidx.compose.runtime.MutableState
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.ui.focus.FocusRequester
import androidx.compose.ui.input.key.*
import dev.eden.emu.ui.components.HeaderFocusRequesters
import dev.eden.emu.ui.utils.NavKeys

/** Navigation zones for the home screen */
enum class NavigationZone { HEADER, CONTENT/*, FOOTER */ }

/**
 * Manages focus and navigation between Header and Content zones.
 * Uses MutableState for reactive UI updates.
 * (Default Focus Manager breaks and is based on "placement", not suitable)
 */
class HomeNavigationManager(
    private val headerFocusRequesters: HeaderFocusRequesters,
    private val contentFocusRequester: FocusRequester,
) {
    private val _currentZone: MutableState<NavigationZone> = mutableStateOf(NavigationZone.CONTENT)
    val currentZone: NavigationZone get() = _currentZone.value

    private val _headerIndex: MutableState<Int> = mutableIntStateOf(0)
    val headerIndex: Int get() = _headerIndex.value

    // Expose state for Compose observation
    val currentZoneState: MutableState<NavigationZone> get() = _currentZone
    val headerIndexState: MutableState<Int> get() = _headerIndex

    private val headerElements = listOf(
        headerFocusRequesters.userButton,
        headerFocusRequesters.searchButton,
        headerFocusRequesters.sortButton,
        headerFocusRequesters.filterButton,
        headerFocusRequesters.settingsButton,
    )

    fun navigateUp() {
        if (_currentZone.value == NavigationZone.CONTENT) {
            _currentZone.value = NavigationZone.HEADER
            headerElements.getOrNull(_headerIndex.value)?.requestFocus()
        }
    }

    fun navigateDown() {
        if (_currentZone.value == NavigationZone.HEADER) {
            _currentZone.value = NavigationZone.CONTENT
            contentFocusRequester.requestFocus()
        }
    }

    fun navigateLeft() {
        if (_currentZone.value == NavigationZone.HEADER && _headerIndex.value > 0) {
            _headerIndex.value--
            headerElements.getOrNull(_headerIndex.value)?.requestFocus()
        }
    }

    fun navigateRight() {
        if (_currentZone.value == NavigationZone.HEADER && _headerIndex.value < headerElements.lastIndex) {
            _headerIndex.value++
            headerElements.getOrNull(_headerIndex.value)?.requestFocus()
        }
    }

    fun requestInitialFocus() {
        _currentZone.value = NavigationZone.CONTENT
        contentFocusRequester.requestFocus()
    }

    fun resetToContent() {
        _currentZone.value = NavigationZone.CONTENT
    }
}

/** Handle D-Pad navigation for home screen */
fun handleHomeNavigation(keyEvent: KeyEvent, nav: HomeNavigationManager): Boolean {
    if (keyEvent.type != KeyEventType.KeyDown) return false

    return when (keyEvent.key) {
        in NavKeys.up -> { nav.navigateUp(); true }
        in NavKeys.down -> { nav.navigateDown(); true }
        in NavKeys.left -> if (nav.currentZone == NavigationZone.HEADER) { nav.navigateLeft(); true } else false
        in NavKeys.right -> if (nav.currentZone == NavigationZone.HEADER) { nav.navigateRight(); true } else false
        else -> false
    }
}
