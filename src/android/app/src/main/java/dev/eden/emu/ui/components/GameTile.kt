// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

package dev.eden.emu.ui.components

import org.yuzu.yuzu_emu.model.Game

data class GameTile(
    val id: String,
    val title: String,
    val uri: String? = null,
    val iconUri: String? = null,
    val programId: Long = 0L,
    val developer: String = "",
    val version: String = "",
    val isHomebrew: Boolean = false,
) {
    companion object {
        fun fromGame(game: Game): GameTile = GameTile(
            id = game.path,
            title = game.title,
            uri = game.path,
            iconUri = game.path,
            programId = game.programId.toLongOrNull() ?: 0L,
            developer = game.developer,
            version = game.version,
            isHomebrew = game.isHomebrew,
        )
    }
}
