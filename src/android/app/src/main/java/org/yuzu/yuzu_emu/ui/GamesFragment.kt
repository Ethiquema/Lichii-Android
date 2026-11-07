// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

package org.yuzu.yuzu_emu.ui

import android.os.Bundle
import android.os.Environment
import android.provider.Settings
import android.view.LayoutInflater
import android.view.View
import android.view.ViewGroup
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.key
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.focus.FocusRequester
import androidx.compose.ui.input.key.Key
import androidx.compose.ui.input.key.key
import androidx.compose.ui.input.key.onPreviewKeyEvent
import androidx.compose.ui.platform.ComposeView
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.ViewCompositionStrategy
import androidx.compose.ui.unit.dp
import androidx.fragment.app.Fragment
import androidx.fragment.app.activityViewModels
import androidx.navigation.fragment.findNavController
import java.util.Locale
import androidx.preference.PreferenceManager
import kotlinx.coroutines.delay
import org.yuzu.yuzu_emu.NativeLibrary
import org.yuzu.yuzu_emu.R
import org.yuzu.yuzu_emu.activities.EmulationActivity
import org.yuzu.yuzu_emu.model.GamesViewModel
import org.yuzu.yuzu_emu.model.HomeViewModel
import dev.eden.emu.ui.background.RetroGridBackground
import dev.eden.emu.ui.components.FooterAction
import dev.eden.emu.ui.components.FooterButton
import dev.eden.emu.ui.components.GameCarousel
import dev.eden.emu.ui.components.GameGrid
import dev.eden.emu.ui.components.GameTile
import dev.eden.emu.ui.components.HeaderFocusRequesters
import dev.eden.emu.ui.components.HomeFooter
import dev.eden.emu.ui.components.HomeHeader
import dev.eden.emu.ui.components.LayoutMode
import dev.eden.emu.ui.components.LayoutSelectionDialog
import dev.eden.emu.ui.components.SortMode
import dev.eden.emu.ui.components.SortSelectionDialog
import dev.eden.emu.ui.navigation.HomeNavigationManager
import dev.eden.emu.ui.navigation.NavigationZone
import dev.eden.emu.ui.navigation.handleHomeNavigation
import dev.eden.emu.ui.theme.EdenTheme
import org.yuzu.yuzu_emu.HomeNavigationDirections
import org.yuzu.yuzu_emu.model.Game

private const val PREF_LAYOUT_MODE = "home_layout_mode"
private const val PREF_SORT_MODE = "home_sort_mode"
private const val PREF_LAST_FOCUSED_INDEX = "home_last_focused_index"
private const val PREF_GAME_WAS_LAUNCHED = "home_game_was_launched"

/**
 * Compose-based Games Fragment with Header/Footer navigation
 */
class GamesFragment : Fragment() {
    private val gamesViewModel: GamesViewModel by activityViewModels()
    private val homeViewModel: HomeViewModel by activityViewModels()

    override fun onCreateView(
        inflater: LayoutInflater,
        container: ViewGroup?,
        savedInstanceState: Bundle?
    ): View {
        return ComposeView(requireContext()).apply {
            setViewCompositionStrategy(ViewCompositionStrategy.DisposeOnViewTreeLifecycleDestroyed)
            setContent {
                EdenTheme {
                    HomeScreen(
                        gamesViewModel = gamesViewModel,
                        onGameLaunch = { path: String ->
                            val game = gamesViewModel.games.value.find { it.path == path }
                            if (game != null) {
                                // Save last played time for sorting
                                PreferenceManager.getDefaultSharedPreferences(requireContext())
                                    .edit()
                                    .putLong(game.keyLastPlayedTime, System.currentTimeMillis())
                                    .putBoolean(PREF_GAME_WAS_LAUNCHED, true)
                                    .apply()

                                EmulationActivity.launch(
                                    requireActivity() as androidx.appcompat.app.AppCompatActivity,
                                    game
                                )
                            }
                        },
                        onNavigateToSettings = {
                            findNavController().navigate(R.id.action_gamesFragment_to_homeSettingsFragment)
                        },
                        onNavigateToUserManagement = {
                            findNavController().navigate(R.id.action_gamesFragment_to_profileManagerFragment)
                        },
                        onShowGameInfo = { game: Game ->
                            val action = HomeNavigationDirections.actionGlobalPerGamePropertiesFragment(game)
                            findNavController().navigate(action)
                        }
                    )
                }
            }
        }
    }

    override fun onViewCreated(view: View, savedInstanceState: Bundle?) {
        super.onViewCreated(view, savedInstanceState)
        homeViewModel.setStatusBarShadeVisibility(true)
    }

    override fun onResume() {
        super.onResume()
        // Only resort games if a game was actually launched (not when coming back from settings)
        val prefs = PreferenceManager.getDefaultSharedPreferences(requireContext())
        if (prefs.getBoolean(PREF_GAME_WAS_LAUNCHED, false)) {
            prefs.edit().putBoolean(PREF_GAME_WAS_LAUNCHED, false).apply()
            gamesViewModel.resortGames()
        }
    }
}

@Composable
private fun HomeScreen(
    gamesViewModel: GamesViewModel,
    onGameLaunch: (String) -> Unit,
    onNavigateToSettings: () -> Unit,
    onNavigateToUserManagement: () -> Unit,
    onShowGameInfo: (Game) -> Unit,
) {
    val context = LocalContext.current
    val games by gamesViewModel.games.collectAsState()
    val isLoading by gamesViewModel.isReloading.collectAsState()
    val shouldScrollToTop by gamesViewModel.shouldScrollToTop.collectAsState()

    val preferences = remember { PreferenceManager.getDefaultSharedPreferences(context) }
    val savedLayoutMode = remember {
        val savedValue = preferences.getString(PREF_LAYOUT_MODE, LayoutMode.CAROUSEL.name)
        try {
            LayoutMode.valueOf(savedValue ?: LayoutMode.CAROUSEL.name)
        } catch (_: Exception) {
            LayoutMode.CAROUSEL
        }
    }

    // Layout mode state (Grid or Carousel)
    var layoutMode by remember { mutableStateOf(savedLayoutMode) }
    var showLayoutDialog by remember { mutableStateOf(false) }
    var showSortDialog by remember { mutableStateOf(false) }
    var searchQuery by remember { mutableStateOf("") }
    var isSearchExpanded by remember { mutableStateOf(false) }

    // Game properties screen state
    var selectedGameTile by remember { mutableStateOf<GameTile?>(null) }

    // Sort mode state
    val savedSortMode = remember {
        val savedValue = preferences.getString(PREF_SORT_MODE, SortMode.LAST_PLAYED.name)
        try {
            SortMode.valueOf(savedValue ?: SortMode.LAST_PLAYED.name)
        } catch (_: Exception) {
            SortMode.LAST_PLAYED
        }
    }
    var sortMode by remember { mutableStateOf(savedSortMode) }

    // Save sort mode when it changes
    LaunchedEffect(sortMode) {
        preferences.edit().putString(PREF_SORT_MODE, sortMode.name).apply()
    }

    // Save layout mode when it changes
    LaunchedEffect(layoutMode) {
        preferences.edit().putString(PREF_LAYOUT_MODE, layoutMode.name).apply()
    }

    // Remember last focused index across layout changes - load from preferences
    val savedFocusedIndex = remember {
        preferences.getInt(PREF_LAST_FOCUSED_INDEX, 0)
    }
    var lastFocusedIndex by remember { mutableStateOf(savedFocusedIndex) }

    LaunchedEffect(lastFocusedIndex) {
        preferences.edit().putInt(PREF_LAST_FOCUSED_INDEX, lastFocusedIndex).apply()
    }

    var listVersion by remember { mutableStateOf(0) }

    LaunchedEffect(sortMode) {
        lastFocusedIndex = 0
        listVersion++
    }

    // Reset to first item when games are resorted (e.g. after playing a game)
    // Else the sorting only takes effect after relaunch
    LaunchedEffect(shouldScrollToTop) {
        if (shouldScrollToTop) {
            lastFocusedIndex = 0
            listVersion++ // Force complete recomposition
            gamesViewModel.setShouldScrollToTop(false)
        }
    }

    // Load current user UUID
    var currentUserUuid by remember { mutableStateOf("") }
    LaunchedEffect(Unit) {
        currentUserUuid = NativeLibrary.getCurrentUser() ?: ""
    }

    val headerFocusRequesters = remember {
        HeaderFocusRequesters(
            userButton = FocusRequester(),
            searchButton = FocusRequester(),
            sortButton = FocusRequester(),
            filterButton = FocusRequester(),
            settingsButton = FocusRequester(),
        )
    }
    // Don't recreate on layoutMode change - this causes zones state loss
    val contentFocusRequester = remember { FocusRequester() }

    val navigationManager = remember {
        HomeNavigationManager(
            headerFocusRequesters = headerFocusRequesters,
            contentFocusRequester = contentFocusRequester,
        )
    }

    // Request initial focus
    LaunchedEffect(Unit) {
        delay(200)
        navigationManager.requestInitialFocus()
    }

    // Filter and sort games based on search query and sort mode
    val filteredGames = remember(games, searchQuery, sortMode) {
        val filtered = if (searchQuery.isBlank()) {
            games
        } else {
            games.filter { game ->
                game.title.lowercase(Locale.getDefault())
                    .contains(searchQuery.lowercase(Locale.getDefault()))
            }
        }

        // Apply sorting
        when (sortMode) {
            SortMode.LAST_PLAYED -> {
                filtered.sortedByDescending { game ->
                    preferences.getLong(game.keyLastPlayedTime, 0L)
                }
            }

            SortMode.ALPHABETICAL -> {
                filtered.sortedBy { it.title.lowercase(Locale.getDefault()) }
            }
        }
    }

    val gameTiles = remember(filteredGames) {
        filteredGames.map { game ->
            GameTile.fromGame(game)
        }
    }

    // NO key handling at top level
    Box(modifier = Modifier.fillMaxSize()) {
        RetroGridBackground()

        val contentOffset = 24.dp
        key(listVersion, layoutMode) {
            when (layoutMode) {
                LayoutMode.TWO_ROW -> {
                    GameGrid(
                        gameTiles = gameTiles,
                        onGameClick = { tile: GameTile ->
                            val path = tile.uri ?: return@GameGrid
                            onGameLaunch(path)
                        },
                        onGameLongClick = { tile: GameTile ->
                            val game = filteredGames.firstOrNull { it.path == tile.uri }
                            if (game != null) {
                                onShowGameInfo(game)
                            }
                        },
                        onNavigateToSettings = {},
                        focusRequester = contentFocusRequester,
                        onNavigateUp = {
                            navigationManager.navigateUp()
                        },
                        rowCount = 2,
                        tileIconSize = 120.dp,
                        isLoading = isLoading,
                        emptyMessage = if (games.isEmpty() && !isLoading) context.getString(R.string.empty_gamelist) else null,
                        initialFocusedIndex = lastFocusedIndex,
                        onFocusedIndexChanged = { newIndex ->
                            lastFocusedIndex = newIndex
                        },
                        onShowGameInfo = { tile: GameTile ->
                            val game = filteredGames.firstOrNull { it.path == tile.uri }
                            if (game != null) {
                                onShowGameInfo(game)
                            }
                        },
                        modifier = Modifier
                            .fillMaxSize()
                            .padding(top = contentOffset),
                    )
                }

                LayoutMode.CAROUSEL -> {
                    GameCarousel(
                        gameTiles = gameTiles,
                        onGameClick = { tile: GameTile ->
                            val path = tile.uri ?: return@GameCarousel
                            onGameLaunch(path)
                        },
                        onGameLongClick = { tile: GameTile ->
                            val game = filteredGames.firstOrNull { it.path == tile.uri }
                            if (game != null) {
                                onShowGameInfo(game)
                            }
                        },
                        focusRequester = contentFocusRequester,
                        onNavigateUp = {
                            navigationManager.navigateUp()
                        },
                        initialFocusedIndex = lastFocusedIndex,
                        onFocusedIndexChanged = { newIndex ->
                            lastFocusedIndex = newIndex
                        },
                        onShowGameInfo = { tile: GameTile ->
                            val game = filteredGames.firstOrNull { it.path == tile.uri }
                            if (game != null) {
                                onShowGameInfo(game)
                            }
                        },
                        modifier = Modifier
                            .fillMaxSize()
                            .padding(top = contentOffset),
                    )
                }

                else -> {
                    // Default to Grid
                    GameGrid(
                        gameTiles = gameTiles,
                        onGameClick = { tile: GameTile ->
                            val path = tile.uri ?: return@GameGrid
                            onGameLaunch(path)
                        },
                        onNavigateToSettings = {},
                        focusRequester = contentFocusRequester,
                        onNavigateUp = {
                            navigationManager.navigateUp()
                        },
                        rowCount = 2,
                        tileIconSize = 120.dp,
                        isLoading = isLoading,
                        emptyMessage = if (games.isEmpty() && !isLoading) context.getString(R.string.empty_gamelist) else null,
                        modifier = Modifier
                            .fillMaxSize()
                            .padding(top = contentOffset),
                    )
                }
            }

            // Refocus content when layout mode changes
            LaunchedEffect(layoutMode) {
                delay(200)
                contentFocusRequester.requestFocus()
                navigationManager.resetToContent()
            }

            // Refocus content when sort mode changes
            LaunchedEffect(sortMode) {
                delay(200)
                contentFocusRequester.requestFocus()
                navigationManager.resetToContent()
            }

            // Header
            Box(
                modifier = Modifier
                    .align(Alignment.TopCenter)
                    .onPreviewKeyEvent { keyEvent ->
                        if (navigationManager.currentZone == NavigationZone.HEADER) {
                            handleHomeNavigation(keyEvent, navigationManager)
                        } else {
                            false
                        }
                    }
            ) {
                HomeHeader(
                    currentUser = currentUserUuid,
                    onUserClick = onNavigateToUserManagement,
                    searchQuery = searchQuery,
                    onSearchQueryChange = { query -> searchQuery = query },
                    isSearchExpanded = isSearchExpanded,
                    onSearchExpandedChange = { expanded -> isSearchExpanded = expanded },
                    onSortClick = {
                        showSortDialog = true
                    },
                    onFilterClick = {
                        showLayoutDialog = true
                    },
                    onSettingsClick = onNavigateToSettings,
                    focusRequesters = headerFocusRequesters,
                )
            }

            val currentZone by navigationManager.currentZoneState
            val headerIdx by navigationManager.headerIndexState
            val footerActions = when (currentZone) {
                NavigationZone.HEADER -> {
                    val headerText = when (headerIdx) {
                        0 -> context.getString(R.string.footer_open) // User profile
                        1 -> context.getString(R.string.home_search) // Search
                        else -> context.getString(R.string.footer_open) // Sort, Filter, Settings
                    }
                    listOf(FooterAction(FooterButton.A, headerText))
                }
                NavigationZone.CONTENT -> {
                    listOf(
                        FooterAction(FooterButton.A, context.getString(R.string.home_start)),
                        FooterAction(FooterButton.X, context.getString(R.string.footer_game_info)),
                    )
                }
            }
            HomeFooter(
                actions = footerActions,
                modifier = Modifier.align(Alignment.BottomCenter)
            )
        }

        // Layout selection dialog
        if (showLayoutDialog) {
            LayoutSelectionDialog(
                onDismiss = { showLayoutDialog = false },
                onSelectGrid = { layoutMode = LayoutMode.TWO_ROW },
                onSelectCarousel = { layoutMode = LayoutMode.CAROUSEL },
            )
        }

        // Sort selection dialog
        if (showSortDialog) {
            SortSelectionDialog(
                currentSortMode = sortMode,
                onDismiss = { showSortDialog = false },
                onSelectSort = { selectedSortMode ->
                    sortMode = selectedSortMode
                    gamesViewModel.resortGames()
                },
            )
        }
    }
}
