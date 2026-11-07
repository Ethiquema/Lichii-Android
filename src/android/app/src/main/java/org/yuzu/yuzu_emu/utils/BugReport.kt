// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

package org.yuzu.yuzu_emu.utils

import android.app.Activity
import android.app.ActivityManager
import android.app.ApplicationExitInfo
import android.app.usage.UsageStatsManager
import android.content.ClipData
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.os.BatteryManager
import android.os.Build
import android.os.PowerManager
import androidx.core.content.FileProvider
import java.io.File
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale
import org.yuzu.yuzu_emu.BuildConfig
import org.yuzu.yuzu_emu.features.settings.model.StringSetting

// One text file for bug reports: device, power state, readiness results, why the app ended
// recently and both logs (the previous run's log holds a crash, the current one this run).
// Settings -> Share debug logs and the readiness check's Share report both send it.
object BugReport {
    /** Writes the report; [readiness] holds the readiness check lines, null when it didn't run. */
    fun write(context: Context, readiness: List<String>?): File {
        val activityManager = context.getSystemService(ActivityManager::class.java)
        val memoryInfo = ActivityManager.MemoryInfo().also { activityManager.getMemoryInfo(it) }
        val dateFormat = SimpleDateFormat("yyyy-MM-dd HH:mm:ss Z", Locale.US)
        val text = StringBuilder()
        text.appendLine("Eden SX2A report, ${dateFormat.format(Date())}")
        text.appendLine("App: ${BuildConfig.VERSION_NAME} (${context.packageName})")
        text.appendLine(
            "Device: ${Build.MANUFACTURER} ${Build.MODEL} (${Build.DEVICE}), Android " +
                "${Build.VERSION.RELEASE} (API ${Build.VERSION.SDK_INT})"
        )
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            text.appendLine("SoC: ${Build.SOC_MANUFACTURER} ${Build.SOC_MODEL}")
        }
        text.appendLine(
            "RAM: ${MemoryUtil.bytesToSizeUnit(memoryInfo.totalMem.toFloat())} total, " +
                "${MemoryUtil.bytesToSizeUnit(memoryInfo.availMem.toFloat())} available now, " +
                "low below ${MemoryUtil.bytesToSizeUnit(memoryInfo.threshold.toFloat())}" +
                if (memoryInfo.lowMemory) " (low now)" else ""
        )
        val driverPath = StringSetting.DRIVER_PATH.getString(needsGlobal = true)
        text.appendLine("GPU driver: ${driverPath.ifEmpty { "system" }.substringAfterLast('/')}")
        appendPowerState(context, text)
        text.appendLine()
        text.appendLine("Readiness check:")
        if (readiness == null) {
            text.appendLine("  (not run; Settings -> Switch 2 readiness check -> Share report includes it)")
        } else {
            readiness.forEach { text.appendLine("  $it") }
        }
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            text.appendLine()
            text.appendLine("Recent exits:")
            activityManager.getHistoricalProcessExitReasons(context.packageName, 0, 10)
                .forEach { exit ->
                    text.appendLine(
                        "  ${dateFormat.format(Date(exit.timestamp))} " +
                            "${exitReasonName(exit.reason)} status=${exit.status} " +
                            "importance=${exit.importance} " +
                            "pss=${exit.pss / 1024} MiB rss=${exit.rss / 1024} MiB " +
                            (exit.description ?: "")
                    )
                }
        }
        val logDir = File(context.getExternalFilesDir(null), "log")
        for ((name, label) in listOf(
            "eden_log.txt.old.txt" to "previous run",
            "eden_log.txt" to "this run"
        )) {
            val log = File(logDir, name)
            text.appendLine()
            text.appendLine("==== $name ($label) ====")
            if (!log.isFile) {
                text.appendLine("(missing)")
                continue
            }
            val bytes = log.readBytes()
            val skipped = (bytes.size - MAX_LOG_BYTES).coerceAtLeast(0)
            if (skipped > 0) {
                text.appendLine("(first $skipped bytes left out)")
            }
            compactLog(String(bytes, skipped, bytes.size - skipped), text)
        }
        val dir = File(context.cacheDir, "report").apply { mkdirs() }
        dir.listFiles()?.forEach { it.delete() }
        val stamp = SimpleDateFormat("yyyyMMdd-HHmmss", Locale.US).format(Date())
        return File(dir, "eden-sx2a-report-$stamp.txt").apply { writeText(text.toString()) }
    }

    private fun appendPowerState(context: Context, text: StringBuilder) {
        val powerManager = context.getSystemService(PowerManager::class.java)
        val activityManager = context.getSystemService(ActivityManager::class.java)
        text.appendLine(
            "Battery optimization: " +
                if (powerManager.isIgnoringBatteryOptimizations(context.packageName)) {
                    "off (unrestricted)"
                } else {
                    "on"
                }
        )
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.P) {
            text.appendLine(
                "Battery usage Restricted: " +
                    if (activityManager.isBackgroundRestricted) "yes" else "no"
            )
            val bucket = context.getSystemService(UsageStatsManager::class.java).appStandbyBucket
            text.appendLine("Standby bucket: ${standbyBucketName(bucket)}")
        }
        text.appendLine("Battery saver: " + if (powerManager.isPowerSaveMode) "on" else "off")
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
            text.appendLine("Thermal status: ${thermalStatusName(powerManager.currentThermalStatus)}")
        }
        val battery = context.registerReceiver(null, IntentFilter(Intent.ACTION_BATTERY_CHANGED))
        if (battery != null) {
            val level = battery.getIntExtra(BatteryManager.EXTRA_LEVEL, -1)
            val scale = battery.getIntExtra(BatteryManager.EXTRA_SCALE, 100)
            val plugged = battery.getIntExtra(BatteryManager.EXTRA_PLUGGED, 0) != 0
            if (level >= 0 && scale > 0) {
                text.appendLine(
                    "Battery: ${level * 100 / scale}%, " + if (plugged) "plugged in" else "on battery"
                )
            }
        }
    }

    private fun standbyBucketName(bucket: Int): String =
        when (bucket) {
            UsageStatsManager.STANDBY_BUCKET_ACTIVE -> "active"
            UsageStatsManager.STANDBY_BUCKET_WORKING_SET -> "working set"
            UsageStatsManager.STANDBY_BUCKET_FREQUENT -> "frequent"
            UsageStatsManager.STANDBY_BUCKET_RARE -> "rare"
            UsageStatsManager.STANDBY_BUCKET_RESTRICTED -> "restricted"
            else -> "bucket $bucket"
        }

    private fun thermalStatusName(status: Int): String =
        when (status) {
            PowerManager.THERMAL_STATUS_NONE -> "none"
            PowerManager.THERMAL_STATUS_LIGHT -> "light"
            PowerManager.THERMAL_STATUS_MODERATE -> "moderate"
            PowerManager.THERMAL_STATUS_SEVERE -> "severe"
            PowerManager.THERMAL_STATUS_CRITICAL -> "critical"
            PowerManager.THERMAL_STATUS_EMERGENCY -> "emergency"
            PowerManager.THERMAL_STATUS_SHUTDOWN -> "shutdown"
            else -> "status $status"
        }

    // Logs repeat the same message thousands of times (stubbed services, skipped draws, per-frame
    // lines). Messages are compared without their timestamp and with numbers masked: a run of one
    // message becomes a count, each message is kept at most MAX_SAME_LOG_LINES times, and the rest
    // are counted in a table. The last LOG_TAIL_LINES lines, where a crash shows, stay as they are.
    private fun compactLog(log: String, text: StringBuilder) {
        val lines = log.lines()
        val bodyEnd = (lines.size - LOG_TAIL_LINES).coerceAtLeast(0)
        val seen = HashMap<String, Int>()
        val lastSeen = HashMap<String, String>()
        var previous: String? = null
        var run = 0
        val endRun = {
            if (run > 0) {
                text.appendLine("    (similar line $run more times)")
            }
            run = 0
        }
        for (line in lines.subList(0, bodyEnd)) {
            if (!line.startsWith("[")) {
                endRun()
                previous = null
                text.appendLine(line)
                continue
            }
            val message = line.substringAfter("] ", line)
            val key = NUMBERS.replace(message, "#")
            val count = (seen[key] ?: 0) + 1
            seen[key] = count
            lastSeen[key] = message
            if (key == previous) {
                run++
                continue
            }
            endRun()
            previous = key
            if (count <= MAX_SAME_LOG_LINES) {
                text.appendLine(line)
            }
        }
        endRun()
        val repeated = seen.filterValues { it > MAX_SAME_LOG_LINES }
        if (repeated.isNotEmpty()) {
            text.appendLine("---- messages seen more than $MAX_SAME_LOG_LINES times ----")
            repeated.entries.sortedByDescending { it.value }.take(MAX_REPEAT_TABLE).forEach {
                text.appendLine("${it.value}x, last: ${lastSeen[it.key]}")
            }
        }
        if (bodyEnd < lines.size) {
            text.appendLine("---- last ${lines.size - bodyEnd} lines ----")
            lines.subList(bodyEnd, lines.size).forEach { text.appendLine(it) }
        }
    }

    fun exitReasonName(reason: Int): String =
        when (reason) {
            ApplicationExitInfo.REASON_EXIT_SELF -> "exit self"
            ApplicationExitInfo.REASON_SIGNALED -> "signaled"
            ApplicationExitInfo.REASON_LOW_MEMORY -> "low memory"
            ApplicationExitInfo.REASON_CRASH -> "crash (Java)"
            ApplicationExitInfo.REASON_CRASH_NATIVE -> "crash (native)"
            ApplicationExitInfo.REASON_ANR -> "not responding"
            ApplicationExitInfo.REASON_INITIALIZATION_FAILURE -> "initialization failure"
            ApplicationExitInfo.REASON_PERMISSION_CHANGE -> "permission change"
            ApplicationExitInfo.REASON_EXCESSIVE_RESOURCE_USAGE -> "excessive resource usage"
            ApplicationExitInfo.REASON_USER_REQUESTED -> "user requested"
            ApplicationExitInfo.REASON_USER_STOPPED -> "user stopped"
            ApplicationExitInfo.REASON_DEPENDENCY_DIED -> "dependency died"
            ApplicationExitInfo.REASON_OTHER -> "other"
            ApplicationExitInfo.REASON_FREEZER -> "freezer"
            ApplicationExitInfo.REASON_PACKAGE_STATE_CHANGE -> "package state change"
            ApplicationExitInfo.REASON_PACKAGE_UPDATED -> "package updated"
            else -> "reason $reason"
        }

    fun share(activity: Activity, report: File, title: CharSequence) {
        val uri = FileProvider.getUriForFile(activity, "${activity.packageName}.provider", report)
        val intent = Intent(Intent.ACTION_SEND)
            .setType("text/plain")
            .putExtra(Intent.EXTRA_STREAM, uri)
            .putExtra(Intent.EXTRA_SUBJECT, report.name)
            .addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION)
        intent.clipData = ClipData.newRawUri(report.name, uri)
        activity.startActivity(Intent.createChooser(intent, title))
    }

    private const val MAX_LOG_BYTES = 16 * 1024 * 1024
    private const val MAX_SAME_LOG_LINES = 5
    private const val MAX_REPEAT_TABLE = 100
    private const val LOG_TAIL_LINES = 200
    private val NUMBERS = Regex("0x[0-9a-fA-F]+|\\d+")
}
