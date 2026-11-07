// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "core/game_overrides.h"
#include "core/game_settings.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <fstream>
#include <sstream>
#include <unordered_map>

#include "common/fs/path_util.h"
#include "common/logging/log.h"
#include "common/settings.h"
#include "video_core/renderer_base.h"

namespace Core::GameOverrides {
    namespace {

        std::string Trim(std::string_view str) {
            const auto start = str.find_first_not_of(" \t\r\n");
            if (start == std::string_view::npos) return "";
            const auto end = str.find_last_not_of(" \t\r\n");
            return std::string(str.substr(start, end - start + 1));
        }

        std::string ToLower(std::string str) {
            std::transform(str.begin(), str.end(), str.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return str;
        }

        bool ToBool(const std::string& value) {
            return value == "true" || value == "1" || value == "yes" || value == "on";
        }

        GameSettings::OS DetectOS() {
#if defined(_WIN32)
            return GameSettings::OS::Windows;
#elif defined(__FIREOS__)
            return GameSettings::OS::FireOS;
#elif defined(__ANDROID__)
            return GameSettings::OS::Android;
#elif defined(__OHOS__)
            return GameSettings::OS::HarmonyOS;
#elif defined(__HAIKU__)
            return GameSettings::OS::HaikuOS;
#elif defined(__DragonFly__)
            return GameSettings::OS::DragonFlyBSD;
#elif defined(__NetBSD__)
            return GameSettings::OS::NetBSD;
#elif defined(__OpenBSD__)
            return GameSettings::OS::OpenBSD;
#elif defined(_AIX)
            return GameSettings::OS::AIX;
#elif defined(__managarm__)
            return GameSettings::OS::Managarm;
#elif defined(__redox__)
            return GameSettings::OS::RedoxOS;
#elif defined(__APPLE__)
            return GameSettings::OS::MacOS;
#elif defined(__FreeBSD__)
            return GameSettings::OS::FreeBSD;
#elif defined(__sun) && defined(__SVR4)
            return GameSettings::OS::Solaris;
#elif defined(__linux__)
            return GameSettings::OS::Linux;
#else
            return GameSettings::OS::Unknown;
#endif
        }

        GameSettings::GPUVendor DetectGPUVendor(const std::string& vendor_string) {
            std::string gpu = vendor_string;
            std::transform(gpu.begin(), gpu.end(), gpu.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

            if (gpu.find("nvidia") != std::string::npos || gpu.find("geforce") != std::string::npos) {
                return GameSettings::GPUVendor::Nvidia;
            }
            if (gpu.find("amd") != std::string::npos || gpu.find("radeon") != std::string::npos ||
                gpu.find("radv") != std::string::npos) {
                return GameSettings::GPUVendor::AMD;
            }
            if (gpu.find("intel") != std::string::npos) {
                return GameSettings::GPUVendor::Intel;
            }
            if (gpu.find("apple") != std::string::npos || gpu.find("molten") != std::string::npos) {
                return GameSettings::GPUVendor::Apple;
            }
            if (gpu.find("qualcomm") != std::string::npos || gpu.find("adreno") != std::string::npos ||
                gpu.find("turnip") != std::string::npos) {
                return GameSettings::GPUVendor::Qualcomm;
            }
            if (gpu.find("mali") != std::string::npos) {
                return GameSettings::GPUVendor::ARM;
            }
            if (gpu.find("powervr") != std::string::npos || gpu.find("pvr") != std::string::npos) {
                return GameSettings::GPUVendor::Imagination;
            }
            if (gpu.find("microsoft") != std::string::npos) {
                return GameSettings::GPUVendor::Microsoft;
            }
            return GameSettings::GPUVendor::Unknown;
        }

        std::string OSToString(GameSettings::OS os) {
            switch (os) {
                case GameSettings::OS::Windows: return "windows";
                case GameSettings::OS::Linux: return "linux";
                case GameSettings::OS::MacOS: return "macos";
                case GameSettings::OS::Android: return "android";
                case GameSettings::OS::FireOS: return "fireos";
                case GameSettings::OS::HarmonyOS: return "harmonyos";
                case GameSettings::OS::FreeBSD: return "freebsd";
                case GameSettings::OS::DragonFlyBSD: return "dragonflybsd";
                case GameSettings::OS::NetBSD: return "netbsd";
                case GameSettings::OS::OpenBSD: return "openbsd";
                case GameSettings::OS::HaikuOS: return "haikuos";
                case GameSettings::OS::AIX: return "aix";
                case GameSettings::OS::Managarm: return "managarm";
                case GameSettings::OS::RedoxOS: return "redoxos";
                case GameSettings::OS::Solaris: return "solaris";
                default: return "unknown";
            }
        }

        std::string VendorToString(GameSettings::GPUVendor vendor) {
            switch (vendor) {
                case GameSettings::GPUVendor::Nvidia: return "nvidia";
                case GameSettings::GPUVendor::AMD: return "amd";
                case GameSettings::GPUVendor::Intel: return "intel";
                case GameSettings::GPUVendor::Apple: return "apple";
                case GameSettings::GPUVendor::Qualcomm: return "qualcomm";
                case GameSettings::GPUVendor::ARM: return "arm";
                case GameSettings::GPUVendor::Imagination: return "imagination";
                case GameSettings::GPUVendor::Microsoft: return "microsoft";
                default: return "unknown";
            }
        }

#define SET_OVERRIDE(setting, new_value) \
do { \
s.setting.SetGlobal(false); \
s.setting.SetValue(new_value); \
} while(0)

        void ApplySetting(const std::string& key, const std::string& value) {
            const std::string k = ToLower(key);
            const std::string v = ToLower(value);
            auto& s = Settings::values;

            if (k == "backend" || k == "renderer_backend") {
                if (v == "vulkan") {
                    SET_OVERRIDE(renderer_backend, Settings::RendererBackend::Vulkan);
                } else if (v == "opengl" || v == "opengl_glsl") {
                    SET_OVERRIDE(renderer_backend, Settings::RendererBackend::OpenGL_GLSL);
                } else if (v == "opengl_glasm") {
                    SET_OVERRIDE(renderer_backend, Settings::RendererBackend::OpenGL_GLASM);
                } else if (v == "opengl_spirv") {
                    SET_OVERRIDE(renderer_backend, Settings::RendererBackend::OpenGL_SPIRV);
                } else if (v == "null") {
                    SET_OVERRIDE(renderer_backend, Settings::RendererBackend::Null);
                }
            }
            else if (k == "vsync") {
                SET_OVERRIDE(vsync_mode, ToBool(v) ? Settings::VSyncMode::Fifo : Settings::VSyncMode::Immediate);
            }
            else if (k == "vsync_mode") {
                if (v == "immediate" || v == "off") {
                    SET_OVERRIDE(vsync_mode, Settings::VSyncMode::Immediate);
                } else if (v == "fifo" || v == "on") {
                    SET_OVERRIDE(vsync_mode, Settings::VSyncMode::Fifo);
                } else if (v == "fifo_relaxed") {
                    SET_OVERRIDE(vsync_mode, Settings::VSyncMode::FifoRelaxed);
                } else if (v == "mailbox") {
                    SET_OVERRIDE(vsync_mode, Settings::VSyncMode::Mailbox);
                }
            }
            else if (k == "gpu_accuracy" || k == "accuracy_level") {
                if (v == "low" || v == "fast") {
                    SET_OVERRIDE(gpu_accuracy, Settings::GpuAccuracy::Low);
                } else if (v == "medium" || v == "balanced" || v == "normal") {
                    SET_OVERRIDE(gpu_accuracy, Settings::GpuAccuracy::Medium);
                } else if (v == "high" || v == "accurate") {
                    SET_OVERRIDE(gpu_accuracy, Settings::GpuAccuracy::High);
                }
            }
            else if (k == "cpu_accuracy") {
                if (v == "auto") {
                    SET_OVERRIDE(cpu_accuracy, Settings::CpuAccuracy::Auto);
                } else if (v == "accurate") {
                    SET_OVERRIDE(cpu_accuracy, Settings::CpuAccuracy::Accurate);
                } else if (v == "unsafe") {
                    SET_OVERRIDE(cpu_accuracy, Settings::CpuAccuracy::Unsafe);
                } else if (v == "paranoid") {
                    SET_OVERRIDE(cpu_accuracy, Settings::CpuAccuracy::Paranoid);
                }
            }
            else if (k == "astc_decode_mode" || k == "accelerate_astc") {
                if (v == "cpu") {
                    SET_OVERRIDE(accelerate_astc, Settings::AstcDecodeMode::Cpu);
                } else if (v == "gpu") {
                    SET_OVERRIDE(accelerate_astc, Settings::AstcDecodeMode::Gpu);
                } else if (v == "cpu_async") {
                    SET_OVERRIDE(accelerate_astc, Settings::AstcDecodeMode::CpuAsynchronous);
                }
            }
            else if (k == "fast_gpu_time") {
                if (v == "off" || v == "false" || v == "0" || v == "normal") {
                    SET_OVERRIDE(fast_gpu_time, Settings::GpuOverclock::Normal);
                } else if (v == "medium" || v == "true" || v == "1") {
                    SET_OVERRIDE(fast_gpu_time, Settings::GpuOverclock::Medium);
                } else if (v == "high") {
                    SET_OVERRIDE(fast_gpu_time, Settings::GpuOverclock::High);
                }
            }
            else if (k == "resolution" || k == "resolution_setup") {
                if (v == "0.25x") {
                    SET_OVERRIDE(resolution_setup, Settings::ResolutionSetup::Res1_4X);
                } else if (v == "0.5x" || v == "half") {
                    SET_OVERRIDE(resolution_setup, Settings::ResolutionSetup::Res1_2X);
                } else if (v == "0.75x") {
                    SET_OVERRIDE(resolution_setup, Settings::ResolutionSetup::Res3_4X);
                } else if (v == "1x" || v == "native") {
                    SET_OVERRIDE(resolution_setup, Settings::ResolutionSetup::Res1X);
                } else if (v == "1.25x") {
                    SET_OVERRIDE(resolution_setup, Settings::ResolutionSetup::Res5_4X);
                } else if (v == "1.5x") {
                    SET_OVERRIDE(resolution_setup, Settings::ResolutionSetup::Res3_2X);
                } else if (v == "2x") {
                    SET_OVERRIDE(resolution_setup, Settings::ResolutionSetup::Res2X);
                } else if (v == "3x") {
                    SET_OVERRIDE(resolution_setup, Settings::ResolutionSetup::Res3X);
                } else if (v == "4x") {
                    SET_OVERRIDE(resolution_setup, Settings::ResolutionSetup::Res4X);
                } else if (v == "5x") {
                    SET_OVERRIDE(resolution_setup, Settings::ResolutionSetup::Res5X);
                } else if (v == "6x") {
                    SET_OVERRIDE(resolution_setup, Settings::ResolutionSetup::Res6X);
                } else if (v == "7x") {
                    SET_OVERRIDE(resolution_setup, Settings::ResolutionSetup::Res7X);
                } else if (v == "8x") {
                    SET_OVERRIDE(resolution_setup, Settings::ResolutionSetup::Res8X);
                }
            }
            else if (k == "async_gpu" || k == "use_asynchronous_gpu_emulation") {
                SET_OVERRIDE(use_asynchronous_gpu_emulation, ToBool(v));
            }
            else if (k == "reactive_flushing" || k == "use_reactive_flushing") {
                SET_OVERRIDE(use_reactive_flushing, ToBool(v));
            }
            else if (k == "multicore" || k == "use_multi_core") {
                SET_OVERRIDE(use_multi_core, ToBool(v));
            }
            else if (k == "async_shaders" || k == "use_asynchronous_shaders") {
                SET_OVERRIDE(use_asynchronous_shaders, ToBool(v));
            }
            else if (k == "pipeline_cache" || k == "use_vulkan_driver_pipeline_cache") {
                SET_OVERRIDE(use_vulkan_driver_pipeline_cache, ToBool(v));
            }
            else if (k == "compute_pipelines" || k == "enable_compute_pipelines") {
                SET_OVERRIDE(enable_compute_pipelines, ToBool(v));
            }
            else if (k == "use_disk_shader_cache") {
                SET_OVERRIDE(use_disk_shader_cache, ToBool(v));
            }
            else if (k == "barrier_feedback_loops") {
                SET_OVERRIDE(barrier_feedback_loops, ToBool(v));
            }
            else if (k == "async_presentation") {
                SET_OVERRIDE(async_presentation, ToBool(v));
            }
            else if (k == "use_squashed_iterated_blend") {
                s.use_squashed_iterated_blend = ToBool(v);
            }
            else if (k == "smo" || k == "sync_memory_operations") {
                SET_OVERRIDE(sync_memory_operations, ToBool(v));
            }
            else if (k == "nvdec" || k == "nvdec_emulation") {
                if (v == "off" || v == "disabled") {
                    SET_OVERRIDE(nvdec_emulation, Settings::NvdecEmulation::Off);
                } else if (v == "cpu") {
                    SET_OVERRIDE(nvdec_emulation, Settings::NvdecEmulation::Cpu);
                } else if (v == "gpu") {
                    SET_OVERRIDE(nvdec_emulation, Settings::NvdecEmulation::Gpu);
                }
            }
            else if (k == "enable_buffer_history" || k == "buffer_history") {
                SET_OVERRIDE(enable_buffer_history, ToBool(v));
            }
            else if (k == "fix_bloom_effects" || k == "fix_bloom") {
                SET_OVERRIDE(fix_bloom_effects, ToBool(v));
            }
            else if (k == "dma_accuracy") {
                if (v == "default") {
                    SET_OVERRIDE(dma_accuracy, Settings::DmaAccuracy::Default);
                } else if (v == "unsafe" || v == "fast") {
                    SET_OVERRIDE(dma_accuracy, Settings::DmaAccuracy::Unsafe);
                } else if (v == "safe" || v == "stable") {
                    SET_OVERRIDE(dma_accuracy, Settings::DmaAccuracy::Safe);
                }
            }
            else if (k == "airplane_mode" || k == "airplane") {
                SET_OVERRIDE(airplane_mode, ToBool(v));
            }
            else if (k == "controller_applet_mode" || k == "controller_applet") {
                if (v == "lle" || v == "real") {
                    SET_OVERRIDE(controller_applet_mode, Settings::AppletMode::LLE);
                } else if (v == "hle" || v == "custom") {
                    SET_OVERRIDE(controller_applet_mode, Settings::AppletMode::HLE);
                }
            }
            else {
                LOG_WARNING(Core, "Unknown game override setting: {}={}", key, value);
            }
        }

#undef SET_OVERRIDE

        std::optional<Section> ParseSectionHeader(std::string_view header) {
            if (header.size() < 2 || header.front() != '[' || header.back() != ']') {
                return std::nullopt;
            }
            header = header.substr(1, header.size() - 2);

            Section section{};

            const auto pipe_pos = header.find('|');
            std::string_view title_part = header;
            std::string_view conditions_part;

            if (pipe_pos != std::string_view::npos) {
                title_part = header.substr(0, pipe_pos);
                conditions_part = header.substr(pipe_pos + 1);
            }

            std::string title_str = Trim(title_part);
            if (title_str.starts_with("0x") || title_str.starts_with("0X")) {
                title_str = title_str.substr(2);
            }

            std::uint64_t title_id = 0;
            auto [ptr, ec] = std::from_chars(title_str.data(), title_str.data() + title_str.size(),
                                             title_id, 16);
            if (ec != std::errc{}) {
                return std::nullopt;
            }
            section.title_id = title_id;

            if (!conditions_part.empty()) {
                std::string cond_str(conditions_part);
                std::istringstream stream(cond_str);
                std::string token;

                while (std::getline(stream, token, ',')) {
                    token = Trim(token);
                    const auto colon_pos = token.find(':');
                    if (colon_pos == std::string::npos) continue;

                    std::string cond_key = ToLower(Trim(token.substr(0, colon_pos)));
                    std::string cond_value = ToLower(Trim(token.substr(colon_pos + 1)));

                    if (cond_key == "vendor") {
                        section.condition.vendor = cond_value;
                    } else if (cond_key == "os") {
                        section.condition.os.push_back(cond_value);
                    } else if (cond_key == "cpu" || cond_key == "cpu_backend") {
                        section.condition.cpu_backend = cond_value;
                    }
                }
            }

            return section;
        }

        std::string CpuBackendToString(Settings::CpuBackend backend) {
            switch (backend) {
                case Settings::CpuBackend::Dynarmic: return "jit";
                case Settings::CpuBackend::Nce: return "nce";
                default: return "unknown";
            }
        }

        bool ConditionMatches(const Condition& cond, const GameSettings::EnvironmentInfo& env) {
            if (cond.vendor.has_value() && cond.vendor.value() != VendorToString(env.vendor)) {
                return false;
            }
            if (!cond.os.empty()) {
                const std::string current_os = OSToString(env.os);
                bool os_match = std::any_of(cond.os.begin(), cond.os.end(),
                    [&current_os](const std::string& os) { return os == current_os; });
                if (!os_match) {
                    return false;
                }
            }
            if (cond.cpu_backend.has_value()) {
                const std::string current_cpu = CpuBackendToString(Settings::values.cpu_backend.GetValue());
                const std::string& required = cond.cpu_backend.value();
                if (required == "jit" || required == "dynarmic") {
                    if (current_cpu != "jit") return false;
                } else if (required == "nce") {
                    if (current_cpu != "nce") return false;
                } else {
                    return false;
                }
            }
            return true;
        }

        std::vector<Section> ParseOverridesFile(const std::filesystem::path& path) {
            std::vector<Section> sections;
            std::ifstream file(path);
            if (!file.is_open()) {
                return sections;
            }

            Section* current_section = nullptr;
            std::string line;

            while (std::getline(file, line)) {
                line = Trim(line);

                if (line.empty() || line.front() == ';' || line.front() == '#') {
                    continue;
                }

                if (line.front() == '[' && line.back() == ']') {
                    auto section = ParseSectionHeader(line);
                    if (section) {
                        sections.push_back(std::move(*section));
                        current_section = &sections.back();
                    } else {
                        current_section = nullptr;
                        LOG_WARNING(Core, "Invalid section header in overrides.ini: {}", line);
                    }
                    continue;
                }

                if (current_section) {
                    const auto eq_pos = line.find('=');
                    if (eq_pos != std::string::npos) {
                        std::string setting_key = Trim(line.substr(0, eq_pos));
                        std::string setting_value = Trim(line.substr(eq_pos + 1));
                        current_section->settings.emplace_back(std::move(setting_key), std::move(setting_value));
                    }
                }
            }

            return sections;
        }

        bool IsEarlySetting(const std::string& key) {
            std::array early_settings = {
                "backend",
                "renderer_backend",
                "multicore",
                "use_multi_core",
            };
            const std::string k = ToLower(key);
            return std::ranges::any_of(early_settings,
                                       [&k](const char* s) { return k == s; });
        }

        bool EarlyConditionMatches(const Condition& cond, GameSettings::OS current_os,
                                   const std::optional<GameSettings::GPUVendor>& detected_vendor) {
            if (!cond.os.empty()) {
                const std::string current_os_str = OSToString(current_os);
                bool os_match = std::any_of(cond.os.begin(), cond.os.end(),
                    [&current_os_str](const std::string& os) { return os == current_os_str; });
                if (!os_match) {
                    return false;
                }
            }
            if (cond.vendor.has_value()) {
                if (!detected_vendor.has_value()) {
                    return false;
                }
                if (cond.vendor.value() != VendorToString(*detected_vendor)) {
                    return false;
                }
            }
            if (cond.cpu_backend.has_value()) {
                const std::string current_cpu = CpuBackendToString(Settings::values.cpu_backend.GetValue());
                const std::string& required = cond.cpu_backend.value();
                if (required == "jit" || required == "dynarmic") {
                    if (current_cpu != "jit") return false;
                } else if (required == "nce" /* || required == "native" */) {
                    if (current_cpu != "nce") return false;
                } else {
                    return false;
                }
            }
            return true;
        }

        int GetSpecificity(const Section& s) {
            int score = 0;
            if (s.condition.vendor.has_value()) score++;
            if (!s.condition.os.empty()) score++;
            if (s.condition.cpu_backend.has_value()) score++;
            return score;
        }

    } // anonymous namespace

    std::filesystem::path GetOverridesPath() {
        return Common::FS::GetEdenPath(Common::FS::EdenPath::CacheDir) / "overrides.ini";
    }

    bool OverridesFileExists() {
        return std::filesystem::exists(GetOverridesPath());
    }

    std::optional<std::uint32_t> GetOverridesFileVersion() {
        const auto path = GetOverridesPath();
        std::ifstream file(path);
        if (!file.is_open()) {
            return std::nullopt;
        }

        std::string line;
        if (std::getline(file, line)) {
            line = Trim(line);
            if (line.starts_with(kVersionPrefix)) {
                const auto version_str = line.substr(std::strlen(kVersionPrefix));
                std::uint32_t version = 0;
                auto [ptr, ec] = std::from_chars(version_str.data(),
                                                  version_str.data() + version_str.size(), version);
                if (ec == std::errc{}) {
                    return version;
                }
            }
        }
        return std::nullopt;
    }

    void ApplyEarlyOverrides(std::uint64_t program_id, const std::string& gpu_vendor) {
        const auto path = GetOverridesPath();
        if (!std::filesystem::exists(path)) {
            return;
        }

        const auto current_os = DetectOS();
        std::optional<GameSettings::GPUVendor> detected_vendor;
        if (!gpu_vendor.empty()) {
            detected_vendor = DetectGPUVendor(gpu_vendor);
        }

        auto sections = ParseOverridesFile(path);

        // Filter matching sections
        std::vector<Section> matching;
        for (auto& section : sections) {
            if (section.title_id != program_id) continue;
            if (!EarlyConditionMatches(section.condition, current_os, detected_vendor)) continue;
            matching.push_back(std::move(section));
        }

        // Sort by base first, then os, then vendor, then both
        std::sort(matching.begin(), matching.end(),
                  [](const Section& a, const Section& b) {
                      return GetSpecificity(a) < GetSpecificity(b);
                  });

        // Apply only early settings, only apply before renderer overrides
        for (const auto& section : matching) {
            for (const auto& [setting_key, setting_value] : section.settings) {
                if (IsEarlySetting(setting_key)) {
                    LOG_INFO(Core, "Game override {:016X}: {}={}",
                             program_id, setting_key, setting_value);
                    ApplySetting(setting_key, setting_value);
                }
            }
        }
    }

    void ApplyLateOverrides(std::uint64_t program_id, const VideoCore::RendererBase& renderer) {
        const auto path = GetOverridesPath();
        if (!std::filesystem::exists(path)) {
            return;
        }

        const auto env = GameSettings::DetectEnvironment(renderer);
        auto sections = ParseOverridesFile(path);

        // Filter matching sections
        std::vector<Section> matching;
        for (auto& section : sections) {
            if (section.title_id != program_id) continue;
            if (!ConditionMatches(section.condition, env)) continue;
            matching.push_back(std::move(section));
        }

        // Sort by base first, then single condition, then multiple
        std::sort(matching.begin(), matching.end(),
                  [](const Section& a, const Section& b) {
                      return GetSpecificity(a) < GetSpecificity(b);
                  });

        // Apply settings (skip early ones, they were already applied, like renderr_backend)
        for (const auto& section : matching) {
            for (const auto& [setting_key, setting_value] : section.settings) {
                if (!IsEarlySetting(setting_key)) {
                    LOG_INFO(Core, "Game override {:016X}: {}={}",
                             program_id, setting_key, setting_value);
                    ApplySetting(setting_key, setting_value);
                }
            }
        }
    }
} // namespace Core::GameOverrides
