// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

package org.yuzu.yuzu_emu.fragments

import android.app.ActivityManager
import android.app.ApplicationExitInfo
import android.app.usage.UsageStatsManager
import android.content.ActivityNotFoundException
import android.content.Context
import android.content.Intent
import android.os.Build
import android.os.Bundle
import android.os.PowerManager
import android.os.StatFs
import android.provider.Settings as AndroidSettings
import android.view.LayoutInflater
import android.view.View
import android.text.format.DateUtils
import android.view.ViewGroup
import android.widget.Toast
import androidx.annotation.RequiresApi
import androidx.core.net.toUri
import androidx.core.view.ViewCompat
import androidx.core.view.WindowInsetsCompat
import androidx.core.view.updatePadding
import androidx.fragment.app.Fragment
import androidx.fragment.app.activityViewModels
import androidx.lifecycle.lifecycleScope
import androidx.navigation.findNavController
import com.google.android.material.R as MaterialR
import com.google.android.material.color.MaterialColors
import com.google.android.material.transition.MaterialSharedAxis
import java.io.File
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import org.yuzu.yuzu_emu.BuildConfig
import org.yuzu.yuzu_emu.HomeNavigationDirections
import org.yuzu.yuzu_emu.R
import org.yuzu.yuzu_emu.databinding.CardReadinessCheckBinding
import org.yuzu.yuzu_emu.databinding.FragmentSwitch2ReadinessBinding
import org.yuzu.yuzu_emu.features.settings.model.BooleanSetting
import org.yuzu.yuzu_emu.features.settings.model.IntSetting
import org.yuzu.yuzu_emu.features.settings.model.Settings
import org.yuzu.yuzu_emu.features.settings.model.StringSetting
import org.yuzu.yuzu_emu.features.settings.ui.SettingsActivity
import org.yuzu.yuzu_emu.features.settings.ui.SettingsActivityArgs
import org.yuzu.yuzu_emu.features.settings.ui.SettingsSubscreen
import org.yuzu.yuzu_emu.model.HomeViewModel
import org.yuzu.yuzu_emu.utils.BugReport
import org.yuzu.yuzu_emu.utils.GameHelper
import org.yuzu.yuzu_emu.utils.GpuDriverHelper
import org.yuzu.yuzu_emu.utils.MemoryUtil
import org.yuzu.yuzu_emu.utils.NativeConfig
import org.yuzu.yuzu_emu.utils.ViewUtils.updateMargins

class Switch2ReadinessFragment : Fragment() {
    private var _binding: FragmentSwitch2ReadinessBinding? = null
    private val binding get() = _binding!!

    private val homeViewModel: HomeViewModel by activityViewModels()

    private var lastChecks: List<Check> = emptyList()

    private enum class Status { PASS, WARNING, FAIL, INFO }

    private enum class Fix(val labelId: Int) {
        DRIVER_MANAGER(R.string.readiness_open_driver_manager),
        GRAPHICS_SETTINGS(R.string.readiness_open_settings),
        DEBUG_SETTINGS(R.string.readiness_open_settings),
        GAME_FOLDERS(R.string.readiness_open_game_folders),
        ALL_FILES_ACCESS(R.string.readiness_grant_access),
        APP_SETTINGS(R.string.readiness_open_app_settings)
    }

    private class Check(
        val title: String,
        val status: Status,
        val message: String,
        val fix: Fix? = null
    )

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        enterTransition = MaterialSharedAxis(MaterialSharedAxis.X, true)
        returnTransition = MaterialSharedAxis(MaterialSharedAxis.X, false)
        reenterTransition = MaterialSharedAxis(MaterialSharedAxis.X, false)
    }

    override fun onCreateView(
        inflater: LayoutInflater,
        container: ViewGroup?,
        savedInstanceState: Bundle?
    ): View {
        _binding = FragmentSwitch2ReadinessBinding.inflate(inflater)
        return binding.root
    }

    override fun onViewCreated(view: View, savedInstanceState: Bundle?) {
        super.onViewCreated(view, savedInstanceState)
        homeViewModel.setStatusBarShadeVisibility(visible = false)

        binding.toolbarReadiness.setNavigationOnClickListener {
            requireActivity().onBackPressedDispatcher.onBackPressed()
        }
        binding.buttonRunAgain.setOnClickListener { runChecks() }
        binding.buttonShareReport.setOnClickListener { shareReport() }

        setInsets()
    }

    // Also re-runs after coming back from a screen opened by a fix action.
    override fun onResume() {
        super.onResume()
        runChecks()
    }

    override fun onDestroyView() {
        super.onDestroyView()
        _binding = null
    }

    private fun runChecks() {
        binding.buttonRunAgain.isEnabled = false
        binding.textSummary.setText(R.string.readiness_running)
        val context = requireContext().applicationContext
        viewLifecycleOwner.lifecycleScope.launch {
            val checks = withContext(Dispatchers.IO) { collectChecks(context) }
            lastChecks = checks
            showChecks(checks)
            binding.buttonRunAgain.isEnabled = true
        }
    }

    private fun collectChecks(context: Context): List<Check> = with(context) {
        val isAdreno = GpuDriverHelper.isAdrenoGpu()
        val checks = mutableListOf(
            checkDevice(),
            checkMemory(),
            if (isAdreno) {
                check(R.string.readiness_gpu, Status.PASS, R.string.readiness_gpu_adreno)
            } else {
                check(R.string.readiness_gpu, Status.WARNING, R.string.readiness_gpu_untested)
            },
            checkDriver(isAdreno)
        )
        checks.addAll(checkSettings())
        checks.add(checkFilesAccess())
        checks.add(checkGames())
        // Release builds don't read the debug switch file.
        if (BuildConfig.BUILD_TYPE != "release") {
            checks.add(checkDebugSwitches())
        }
        checks.add(checkStorage())
        checks.add(checkBattery())
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            checks.add(checkLastExit())
            checks.add(checkMemoryKiller())
        }
        return checks
    }

    private fun Context.check(
        titleId: Int,
        status: Status,
        messageId: Int,
        vararg formatArgs: Any,
        fix: Fix? = null
    ): Check = Check(getString(titleId), status, getString(messageId, *formatArgs), fix)

    private fun Context.checkDevice(): Check {
        val release = Build.VERSION.RELEASE
        return when {
            !Build.SUPPORTED_64_BIT_ABIS.contains("arm64-v8a") ->
                check(R.string.readiness_device, Status.FAIL, R.string.readiness_device_not_arm64)

            Build.VERSION.SDK_INT < Build.VERSION_CODES.TIRAMISU -> check(
                R.string.readiness_device,
                Status.FAIL,
                R.string.readiness_device_old_android,
                release
            )

            else ->
                check(R.string.readiness_device, Status.PASS, R.string.readiness_device_ok, release)
        }
    }

    private fun Context.checkMemory(): Check {
        val ram = MemoryUtil.getDeviceRAM()
        // totalMem excludes memory the firmware and kernel reserve: 16 GB phones report about
        // 14.5-15.5 GiB and 12 GB phones about 10.5-11.5 GiB.
        val gigabytes = MemoryUtil.totalMemory / MemoryUtil.Gb
        return when {
            gigabytes >= 14f ->
                check(R.string.readiness_ram, Status.PASS, R.string.readiness_ram_ok, ram)

            gigabytes >= 10f ->
                check(R.string.readiness_ram, Status.WARNING, R.string.readiness_ram_low, ram)

            else -> check(R.string.readiness_ram, Status.FAIL, R.string.readiness_ram_too_low, ram)
        }
    }

    private fun Context.checkDriver(isAdreno: Boolean): Check {
        val title = R.string.readiness_driver
        if (!isAdreno) {
            return check(title, Status.INFO, R.string.readiness_driver_not_adreno)
        }
        val driverPath = StringSetting.DRIVER_PATH.getString(needsGlobal = true)
        if (driverPath.isEmpty()) {
            return check(
                title,
                Status.WARNING,
                R.string.readiness_driver_stock,
                fix = Fix.DRIVER_MANAGER
            )
        }
        val metadata = GpuDriverHelper.getMetadataFromZip(File(driverPath))
        val name = metadata.name ?: File(driverPath).name
        val isBundledTurnip = name.contains("turnip", true) &&
            (name.contains("purple", true) || metadata.author?.contains("purple", true) == true)
        return if (isBundledTurnip) {
            check(title, Status.PASS, R.string.readiness_driver_ok, name)
        } else {
            check(
                title,
                Status.WARNING,
                R.string.readiness_driver_other,
                name,
                fix = Fix.DRIVER_MANAGER
            )
        }
    }

    private fun Context.checkSettings(): List<Check> {
        val dynarmic = IntSetting.CPU_BACKEND.getInt(needsGlobal = true) == CPU_BACKEND_DYNARMIC
        val asyncShaders =
            BooleanSetting.RENDERER_ASYNCHRONOUS_SHADERS.getBoolean(needsGlobal = true)
        val asyncPresentation = NativeConfig.getBoolean(KEY_ASYNC_PRESENTATION, true)
        val resolution = IntSetting.RENDERER_RESOLUTION.getInt(needsGlobal = true)
        return listOf(
            settingCheck(
                R.string.cpu_backend,
                dynarmic,
                R.string.readiness_cpu_backend_ok,
                R.string.readiness_cpu_backend_bad,
                Fix.DEBUG_SETTINGS
            ),
            settingCheck(
                R.string.readiness_async_shaders,
                asyncShaders,
                R.string.readiness_async_shaders_ok,
                R.string.readiness_async_shaders_bad,
                Fix.GRAPHICS_SETTINGS
            ),
            settingCheck(
                R.string.readiness_async_presentation,
                !asyncPresentation,
                R.string.readiness_async_presentation_ok,
                R.string.readiness_async_presentation_bad,
                null
            ),
            settingCheck(
                R.string.readiness_resolution,
                resolution <= RESOLUTION_1X,
                R.string.readiness_resolution_ok,
                R.string.readiness_resolution_bad,
                Fix.GRAPHICS_SETTINGS
            ),
            check(
                R.string.readiness_memory_layout,
                Status.INFO,
                R.string.readiness_memory_layout_info
            )
        )
    }

    private fun Context.settingCheck(
        titleId: Int,
        ok: Boolean,
        okId: Int,
        badId: Int,
        fix: Fix?
    ): Check =
        if (ok) {
            check(titleId, Status.PASS, okId)
        } else {
            check(titleId, Status.WARNING, badId, fix = fix)
        }

    private fun Context.checkFilesAccess(): Check =
        if (GameHelper.hasAllFilesAccess()) {
            check(R.string.readiness_files_access, Status.PASS, R.string.readiness_files_access_ok)
        } else {
            check(
                R.string.readiness_files_access,
                Status.WARNING,
                R.string.readiness_files_access_missing,
                fix = Fix.ALL_FILES_ACCESS
            )
        }

    private fun Context.checkGames(): Check {
        val title = R.string.readiness_games
        if (!GameHelper.hasAllFilesAccess()) {
            return check(
                title,
                Status.WARNING,
                R.string.readiness_games_no_access,
                fix = Fix.ALL_FILES_ACCESS
            )
        }
        val games = NativeConfig.getGameDirs()
            .flatMap { GameHelper.findLibraryExeFSGames(it) }
            .distinctBy { it.path }
        return if (games.isEmpty()) {
            check(title, Status.WARNING, R.string.readiness_games_none, fix = Fix.GAME_FOLDERS)
        } else {
            val titles = games.joinToString { it.title }
            check(title, Status.PASS, R.string.readiness_games_found, titles)
        }
    }

    private fun Context.checkDebugSwitches(): Check {
        val title = R.string.readiness_debug_switches
        val file = File(getExternalFilesDir(null), DEBUG_ENV_FILE)
        val names = try {
            if (file.isFile) {
                file.readLines()
                    .map { it.trim() }
                    .filter { it.isNotEmpty() && !it.startsWith("#") && it.contains('=') }
                    .map { it.substringBefore('=').trim() }
            } else {
                emptyList()
            }
        } catch (_: Exception) {
            emptyList()
        }
        return if (names.isEmpty()) {
            check(title, Status.PASS, R.string.readiness_debug_switches_none)
        } else {
            check(
                title,
                Status.WARNING,
                R.string.readiness_debug_switches_active,
                names.joinToString(),
                file.path
            )
        }
    }

    private fun Context.checkStorage(): Check {
        val directory = getExternalFilesDir(null) ?: filesDir
        val available = StatFs(directory.path).availableBytes.toFloat()
        val size = MemoryUtil.bytesToSizeUnit(available)
        return if (available >= MIN_FREE_STORAGE_GB * MemoryUtil.Gb) {
            check(R.string.readiness_storage, Status.PASS, R.string.readiness_storage_ok, size)
        } else {
            check(R.string.readiness_storage, Status.WARNING, R.string.readiness_storage_low, size)
        }
    }

    private fun Context.checkBattery(): Check {
        val title = R.string.readiness_battery
        val powerManager = getSystemService(PowerManager::class.java)
        val activityManager = getSystemService(ActivityManager::class.java)
        val usageStats = getSystemService(UsageStatsManager::class.java)
        return when {
            activityManager.isBackgroundRestricted -> check(
                title,
                Status.FAIL,
                R.string.readiness_battery_background_restricted,
                fix = Fix.APP_SETTINGS
            )

            !powerManager.isIgnoringBatteryOptimizations(packageName) -> check(
                title,
                Status.WARNING,
                R.string.readiness_battery_restricted,
                fix = Fix.APP_SETTINGS
            )

            Build.VERSION.SDK_INT >= Build.VERSION_CODES.R &&
                usageStats.appStandbyBucket == UsageStatsManager.STANDBY_BUCKET_RESTRICTED -> check(
                title,
                Status.WARNING,
                R.string.readiness_battery_standby_restricted,
                fix = Fix.APP_SETTINGS
            )

            else -> check(title, Status.PASS, R.string.readiness_battery_ok)
        }
    }

    // The current process is not in the history yet, so the newest entry is the previous run.
    @RequiresApi(Build.VERSION_CODES.TIRAMISU)
    private fun Context.checkLastExit(): Check {
        val title = R.string.readiness_last_exit
        val exit = getSystemService(ActivityManager::class.java)
            .getHistoricalProcessExitReasons(packageName, 0, 1)
            .firstOrNull() ?: return check(title, Status.INFO, R.string.readiness_last_exit_none)
        val time = DateUtils.getRelativeTimeSpanString(
            exit.timestamp,
            System.currentTimeMillis(),
            DateUtils.MINUTE_IN_MILLIS
        )
        return when (exit.reason) {
            ApplicationExitInfo.REASON_EXIT_SELF,
            ApplicationExitInfo.REASON_USER_REQUESTED,
            ApplicationExitInfo.REASON_USER_STOPPED ->
                check(title, Status.PASS, R.string.readiness_last_exit_normal, time)

            ApplicationExitInfo.REASON_LOW_MEMORY,
            ApplicationExitInfo.REASON_FREEZER,
            ApplicationExitInfo.REASON_EXCESSIVE_RESOURCE_USAGE ->
                check(title, Status.WARNING, R.string.readiness_last_exit_memory, time)

            ApplicationExitInfo.REASON_CRASH,
            ApplicationExitInfo.REASON_CRASH_NATIVE ->
                check(title, Status.WARNING, R.string.readiness_last_exit_crash, time)

            ApplicationExitInfo.REASON_ANR ->
                check(title, Status.WARNING, R.string.readiness_last_exit_anr, time)

            else -> check(
                title,
                Status.INFO,
                R.string.readiness_last_exit_other,
                BugReport.exitReasonName(exit.reason),
                time
            )
        }
    }

    // Android doesn't say whether its low memory killer is armed, but it keeps the reasons the
    // app's process ended: kills for memory (lmkd), the cached app freezer and excessive resource
    // use show up there, as does a phone that is low on memory right now.
    @RequiresApi(Build.VERSION_CODES.TIRAMISU)
    private fun Context.checkMemoryKiller(): Check {
        val title = R.string.readiness_memory_killer
        val activityManager = getSystemService(ActivityManager::class.java)
        val since = System.currentTimeMillis() - MEMORY_KILL_WINDOW_MS
        val kills = activityManager.getHistoricalProcessExitReasons(packageName, 0, 0)
            .filter {
                it.timestamp >= since && it.reason in setOf(
                    ApplicationExitInfo.REASON_LOW_MEMORY,
                    ApplicationExitInfo.REASON_FREEZER,
                    ApplicationExitInfo.REASON_EXCESSIVE_RESOURCE_USAGE
                )
            }
        if (kills.isNotEmpty()) {
            val last = DateUtils.getRelativeTimeSpanString(
                kills.maxOf { it.timestamp },
                System.currentTimeMillis(),
                DateUtils.MINUTE_IN_MILLIS
            )
            return check(
                title,
                Status.WARNING,
                R.string.readiness_memory_killer_kills,
                kills.size,
                last,
                fix = Fix.APP_SETTINGS
            )
        }
        val memoryInfo = ActivityManager.MemoryInfo()
        activityManager.getMemoryInfo(memoryInfo)
        if (memoryInfo.lowMemory) {
            return check(
                title,
                Status.WARNING,
                R.string.readiness_memory_killer_low_now,
                MemoryUtil.bytesToSizeUnit(memoryInfo.availMem.toFloat())
            )
        }
        return check(title, Status.PASS, R.string.readiness_memory_killer_ok)
    }

    private fun shareReport() {
        val context = requireContext().applicationContext
        val lines = lastChecks.map { "[${it.status}] ${it.title}: ${it.message}" }
        binding.buttonShareReport.isEnabled = false
        viewLifecycleOwner.lifecycleScope.launch {
            val report = withContext(Dispatchers.IO) {
                try {
                    BugReport.write(context, lines)
                } catch (_: Exception) {
                    null
                }
            }
            binding.buttonShareReport.isEnabled = true
            if (report == null) {
                Toast.makeText(context, R.string.readiness_share_report_failed, Toast.LENGTH_SHORT)
                    .show()
                return@launch
            }
            BugReport.share(requireActivity(), report, getText(R.string.readiness_share_report))
        }
    }

    private fun showChecks(checks: List<Check>) {
        val list = binding.listChecks
        list.removeViews(1, list.childCount - 1)
        checks.forEach { check ->
            val card = CardReadinessCheckBinding.inflate(layoutInflater, list, false)
            val (iconId, colorAttr, statusId) = when (check.status) {
                Status.PASS -> Triple(
                    R.drawable.ic_check_circle,
                    MaterialR.attr.colorPrimary,
                    R.string.readiness_status_pass
                )

                Status.WARNING -> Triple(
                    R.drawable.ic_info_outline,
                    MaterialR.attr.colorTertiary,
                    R.string.readiness_status_warning
                )

                Status.FAIL -> Triple(
                    R.drawable.ic_clear,
                    MaterialR.attr.colorError,
                    R.string.readiness_status_fail
                )

                Status.INFO -> Triple(
                    R.drawable.ic_info_outline,
                    MaterialR.attr.colorOnSurfaceVariant,
                    R.string.readiness_status_info
                )
            }
            val color = MaterialColors.getColor(card.root, colorAttr)
            card.iconStatus.setImageResource(iconId)
            card.iconStatus.setColorFilter(color)
            card.textTitle.text = check.title
            card.textStatus.setText(statusId)
            card.textStatus.setTextColor(color)
            card.textMessage.text = check.message
            check.fix?.let { fix ->
                card.buttonFix.setText(fix.labelId)
                card.buttonFix.visibility = View.VISIBLE
                card.buttonFix.setOnClickListener { applyFix(fix) }
            }
            list.addView(card.root)
        }
        binding.textSummary.text = getString(
            R.string.readiness_summary,
            checks.count { it.status == Status.PASS },
            checks.count { it.status == Status.WARNING },
            checks.count { it.status == Status.FAIL }
        )
    }

    private fun applyFix(fix: Fix) {
        when (fix) {
            Fix.DRIVER_MANAGER -> openSubscreen(SettingsSubscreen.DRIVER_MANAGER)
            Fix.GAME_FOLDERS -> openSubscreen(SettingsSubscreen.GAME_FOLDERS)
            Fix.GRAPHICS_SETTINGS -> openSettings(Settings.MenuTag.SECTION_RENDERER)
            Fix.DEBUG_SETTINGS -> openSettings(Settings.MenuTag.SECTION_DEBUG)
            Fix.ALL_FILES_ACCESS -> if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
                openSystemSettings(AndroidSettings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION)
            }
            Fix.APP_SETTINGS ->
                openSystemSettings(AndroidSettings.ACTION_APPLICATION_DETAILS_SETTINGS)
        }
    }

    private fun openSubscreen(destination: SettingsSubscreen) {
        val action =
            HomeNavigationDirections.actionGlobalSettingsSubscreenActivity(destination, null)
        binding.root.findNavController().navigate(action)
    }

    private fun openSettings(menuTag: Settings.MenuTag) {
        val intent = Intent(requireContext(), SettingsActivity::class.java)
            .putExtras(SettingsActivityArgs(null, menuTag).toBundle())
        startActivity(intent)
    }

    private fun openSystemSettings(action: String) {
        val intent = Intent(action, "package:${requireContext().packageName}".toUri())
        try {
            startActivity(intent)
        } catch (_: ActivityNotFoundException) {
            startActivity(Intent(AndroidSettings.ACTION_SETTINGS))
        }
    }

    private fun setInsets() =
        ViewCompat.setOnApplyWindowInsetsListener(
            binding.root
        ) { _: View, windowInsets: WindowInsetsCompat ->
            val barInsets = windowInsets.getInsets(WindowInsetsCompat.Type.systemBars())
            val cutoutInsets = windowInsets.getInsets(WindowInsetsCompat.Type.displayCutout())

            val leftInsets = barInsets.left + cutoutInsets.left
            val rightInsets = barInsets.right + cutoutInsets.right

            binding.toolbarReadiness.updateMargins(left = leftInsets, right = rightInsets)

            val fabSpacing = resources.getDimensionPixelSize(R.dimen.spacing_fab)
            binding.buttonShareReport.updateMargins(
                left = leftInsets + fabSpacing,
                bottom = barInsets.bottom + fabSpacing
            )
            binding.buttonRunAgain.updateMargins(
                left = leftInsets + fabSpacing,
                right = rightInsets + fabSpacing,
                bottom = barInsets.bottom + fabSpacing
            )

            binding.scrollReadiness.updateMargins(left = leftInsets, right = rightInsets)
            binding.scrollReadiness.updatePadding(
                bottom = barInsets.bottom +
                    resources.getDimensionPixelSize(R.dimen.spacing_bottom_list_fab)
            )

            windowInsets
        }

    companion object {
        private const val CPU_BACKEND_DYNARMIC = 0
        private const val RESOLUTION_1X = 3
        private const val KEY_ASYNC_PRESENTATION = "async_presentation"
        private const val DEBUG_ENV_FILE = "nx2_env.txt"
        private const val MIN_FREE_STORAGE_GB = 10
        private const val MEMORY_KILL_WINDOW_MS = 7 * DateUtils.DAY_IN_MILLIS
    }
}
