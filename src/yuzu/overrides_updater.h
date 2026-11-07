// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QObject>
#include <QString>
#include <optional>

class QWidget;

class OverridesUpdater : public QObject {
    Q_OBJECT

public:
    explicit OverridesUpdater(QWidget* parent = nullptr);
    ~OverridesUpdater() override;

    void CheckAndUpdate();
    void DownloadOverrides();

signals:
    void ConfigChanged();
    void UpdateCompleted(bool success, const QString& message);

private:
    void ShowConsentDialog();
    bool DownloadAndSave();
    static std::optional<std::uint32_t> ParseVersion(const std::string& data);
    static std::optional<std::string> FetchOverridesFile();

    QWidget* parent_widget{};

    static constexpr const char* kOverridesUrl = "https://raw.githubusercontent.com";
    static constexpr const char* kOverridesPath = "/eden-emulator/eden-overrides/refs/heads/master/overrides.ini";
};
