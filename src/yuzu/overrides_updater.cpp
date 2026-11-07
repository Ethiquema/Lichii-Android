// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "overrides_updater.h"

#include <QCheckBox>
#include <QMessageBox>
#include <QPushButton>
#include <QtConcurrent>
#include <fstream>

#include "common/logging/log.h"
#include "core/game_overrides.h"
#include "qt_common/config/uisettings.h"

#include <httplib.h>

#ifdef YUZU_BUNDLED_OPENSSL
#include <openssl/cert.h>
#endif

OverridesUpdater::OverridesUpdater(QWidget* parent)
    : QObject(parent), parent_widget(parent) {}

OverridesUpdater::~OverridesUpdater() = default;

void OverridesUpdater::CheckAndUpdate() {
    if (!UISettings::values.enable_global_overrides.GetValue()) {
        return;
    }

    if (!UISettings::values.overrides_consent_given.GetValue()) {
        ShowConsentDialog();
        return;
    }

    if (UISettings::values.auto_update_overrides.GetValue()) {
        (void)QtConcurrent::run([this] {
            DownloadAndSave();
        });
    }
}

void OverridesUpdater::ShowConsentDialog() {
    QMessageBox msgBox(parent_widget);
    msgBox.setWindowTitle(tr("Game Overrides Database"));
    msgBox.setIcon(QMessageBox::Question);
    msgBox.setText(tr("Would you like to update the game overrides database?"));
    msgBox.setInformativeText(
        tr("This will download a config file from GitHub to enable critical per-game settings.\n\n"
           "Your choice will be saved. You can change this later in:\n"
           "Settings -> General -> Enable global game overrides"));

    QCheckBox autoUpdateCheckbox(tr("Automatically update the list"), &msgBox);
    autoUpdateCheckbox.setChecked(true);
    msgBox.setCheckBox(&autoUpdateCheckbox);

    auto* yesButton = msgBox.addButton(tr("Yes, Enable"), QMessageBox::AcceptRole);
    msgBox.addButton(tr("No Thanks"), QMessageBox::RejectRole);

    msgBox.setDefaultButton(yesButton);
    msgBox.exec();

    auto* clicked = msgBox.clickedButton();

    UISettings::values.overrides_consent_given.SetValue(true);

    if (clicked == yesButton) {
        UISettings::values.auto_update_overrides.SetValue(autoUpdateCheckbox.isChecked());
        UISettings::values.enable_global_overrides.SetValue(true);
        emit ConfigChanged();
        DownloadOverrides();
    } else {
        UISettings::values.auto_update_overrides.SetValue(false);
        UISettings::values.enable_global_overrides.SetValue(false);
        emit ConfigChanged();
    }
}

void OverridesUpdater::DownloadOverrides() {
    (void)QtConcurrent::run([this] {
        const bool success = DownloadAndSave();
        emit UpdateCompleted(success, success
            ? tr("Game overrides updated successfully.")
            : tr("Failed to download game overrides."));
    });
}

std::optional<std::string> OverridesUpdater::FetchOverridesFile() {
    try {
        constexpr std::size_t timeout_seconds = 3;

        std::unique_ptr<httplib::Client> client = std::make_unique<httplib::Client>(kOverridesUrl);
        client->set_connection_timeout(timeout_seconds);
        client->set_read_timeout(timeout_seconds);
        client->set_write_timeout(timeout_seconds);

#ifdef YUZU_BUNDLED_OPENSSL
        client->load_ca_cert_store(kCert, sizeof(kCert));
#endif

        httplib::Request request{
            .method = "GET",
            .path = kOverridesPath,
        };

        client->set_follow_location(true);
        httplib::Result result = client->send(request);

        if (!result) {
            LOG_ERROR(Frontend, "GET to {}{} returned null", kOverridesUrl, kOverridesPath);
            return {};
        }

        return result.value().body;
    } catch (const std::exception& e) {
        LOG_ERROR(Frontend, "Failed to fetch overrides: {}", e.what());
        return {};
    }
}

bool OverridesUpdater::DownloadAndSave() {
    LOG_INFO(Frontend, "Checking for game overrides updates...");

    const auto response = FetchOverridesFile();
    if (!response) {
        return false;
    }

    const auto& data = *response;

    auto remote_version = ParseVersion(data);
    if (!remote_version) {
        LOG_ERROR(Frontend, "Downloaded overrides file has invalid format (no version header)");
        return false;
    }

    auto local_version = Core::GameOverrides::GetOverridesFileVersion();
    if (local_version && *local_version >= *remote_version) {
        LOG_INFO(Frontend, "Game overrides are up to date (v{})", *local_version);
        return true;
    }

    const auto path = Core::GameOverrides::GetOverridesPath();
    std::filesystem::create_directories(path.parent_path());

    std::ofstream file(path, std::ios::binary);
    if (!file.is_open()) {
        LOG_ERROR(Frontend, "Failed to write overrides file: {}", path.string());
        return false;
    }

    file.write(data.data(), static_cast<std::streamsize>(data.size()));
    file.close();

    LOG_INFO(Frontend, "Game overrides updated to version {}", *remote_version);
    return true;
}

std::optional<std::uint32_t> OverridesUpdater::ParseVersion(const std::string& data) {
    // Look for "; version=X" on first line
    const auto newline_pos = data.find('\n');
    const std::string first_line = (newline_pos != std::string::npos)
        ? data.substr(0, newline_pos)
        : data;

    constexpr const char* prefix = "; version=";
    const auto prefix_pos = first_line.find(prefix);
    if (prefix_pos == std::string::npos) {
        return {};
    }

    const auto version_str = first_line.substr(prefix_pos + std::strlen(prefix));
    try {
        return static_cast<std::uint32_t>(std::stoul(version_str));
    } catch (...) {
        return {};
    }
}
