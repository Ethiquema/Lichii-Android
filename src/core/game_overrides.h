// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace VideoCore {
    class RendererBase;
}

namespace Core::GameOverrides {
    // used to check against GitHub Release Version
    // repo at: https://github.com/eden-emulator/eden-overrides
    inline constexpr std::uint32_t kOverridesVersion = 1;
    inline constexpr const char* kVersionPrefix = "; version=";

    struct Condition {
        std::optional<std::string> vendor;
        std::vector<std::string> os;
        std::optional<std::string> cpu_backend;
    };

    struct Section {
        std::uint64_t title_id{};
        Condition condition;
        std::vector<std::pair<std::string, std::string>> settings;
    };

    std::filesystem::path GetOverridesPath();
    bool OverridesFileExists();
    std::optional<std::uint32_t> GetOverridesFileVersion();

    void ApplyEarlyOverrides(std::uint64_t program_id, const std::string& gpu_vendor);
    void ApplyLateOverrides(std::uint64_t program_id, const VideoCore::RendererBase& renderer);
} // namespace Core::GameOverrides
