// SPDX-FileCopyrightText: Copyright 2025 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Copyright 2023 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

package org.yuzu.yuzu_emu.utils

import android.app.PendingIntent
import android.app.Service
import android.content.Intent
import android.os.IBinder
import android.os.Process
import androidx.core.app.NotificationCompat
import androidx.core.app.NotificationManagerCompat
import org.yuzu.yuzu_emu.NativeLibrary
import org.yuzu.yuzu_emu.R
import org.yuzu.yuzu_emu.activities.EmulationActivity

/**
 * A service that shows a permanent notification in the background to avoid the app getting
 * cleared from memory by the system.
 */
class ForegroundService : Service() {
    companion object {
        const val EMULATION_RUNNING_NOTIFICATION = 0x1000

        const val ACTION_STOP = "stop"

        private const val TASK_REMOVED_STOP_TIMEOUT_MS = 5000L
    }

    private fun showRunningNotification() {
        // Intent is used to resume emulation if the notification is clicked
        val contentIntent = PendingIntent.getActivity(
            this,
            0,
            Intent(this, EmulationActivity::class.java),
            PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT
        )
        val builder =
            NotificationCompat.Builder(this, getString(R.string.app_notification_channel_id))
                .setSmallIcon(R.drawable.ic_stat_notification_logo)
                .setContentTitle(getString(R.string.app_name))
                .setContentText(getString(R.string.app_notification_running))
                .setPriority(NotificationCompat.PRIORITY_DEFAULT)
                .setOngoing(true)
                .setVibrate(null)
                .setSound(null)
                .setContentIntent(contentIntent)
        startForeground(EMULATION_RUNNING_NOTIFICATION, builder.build())
    }

    override fun onBind(intent: Intent): IBinder? {
        return null
    }

    override fun onCreate() {
        showRunningNotification()
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        if (intent?.action == ACTION_STOP) {
            try {
                NotificationManagerCompat.from(this).cancel(EMULATION_RUNNING_NOTIFICATION)
                stopForeground(STOP_FOREGROUND_REMOVE)
            } catch (e: Exception) {
                Log.error("Failed to stop foreground service")
            }
            stopSelfResult(startId)
            return START_NOT_STICKY
        }

        if (intent != null) {
            showRunningNotification()
        }
        return START_STICKY
    }

    override fun onDestroy() =
        NotificationManagerCompat.from(this).cancel(EMULATION_RUNNING_NOTIFICATION)

    // Swiping the app away from recents ends the game. Otherwise the paused session stays in the
    // cached process (where Android may freeze it) until the app is force-stopped.
    // Fallback: the process is then ended, also when the session does not shut down in time.
    override fun onTaskRemoved(rootIntent: Intent?) {
        super.onTaskRemoved(rootIntent)
        Log.info("[ForegroundService] Task removed; stopping emulation and ending the process")
        Thread({
            if (NativeLibrary.isRunning() &&
                !NativeLibrary.stopEmulationAndWait(TASK_REMOVED_STOP_TIMEOUT_MS)
            ) {
                Log.warning("[ForegroundService] Emulation did not stop in time")
            }
            Process.killProcess(Process.myPid())
        }, "TaskRemovedStop").start()
    }
}
