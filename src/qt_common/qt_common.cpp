// SPDX-FileCopyrightText: Copyright 2025 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "qt_common.h"
#include "common/fs/fs.h"

#include <QGuiApplication>
#include <QStringLiteral>
#include "common/logging.h"
#include "core/frontend/emu_window.h"

#include <QFile>

#include <QMessageBox>

#include <JlCompress.h>

#if !defined(WIN32) && !defined(__APPLE__)
#include <qpa/qplatformnativeinterface.h>
#elif defined(__APPLE__)
#include <objc/message.h>
#endif

namespace QtCommon {

#ifdef YUZU_QT_WIDGETS
QWidget* rootObject = nullptr;
#else
QObject* rootObject = nullptr;
#endif

std::unique_ptr<Core::System> system = nullptr;
std::shared_ptr<FileSys::RealVfsFilesystem> vfs = nullptr;
std::unique_ptr<FileSys::ManualContentProvider> provider = nullptr;

Core::Frontend::WindowSystemType GetWindowSystemType()
{
    // Determine WSI type based on Qt platform.
    QString platform_name = QGuiApplication::platformName();
    if (platform_name == QStringLiteral("windows"))
        return Core::Frontend::WindowSystemType::Windows;
    else if (platform_name == QStringLiteral("xcb"))
        return Core::Frontend::WindowSystemType::X11;
    else if (platform_name == QStringLiteral("wayland"))
        return Core::Frontend::WindowSystemType::Wayland;
    else if (platform_name == QStringLiteral("wayland-egl"))
        return Core::Frontend::WindowSystemType::Wayland;
    else if (platform_name == QStringLiteral("cocoa"))
        return Core::Frontend::WindowSystemType::Cocoa;
    else if (platform_name == QStringLiteral("android"))
        return Core::Frontend::WindowSystemType::Android;

    LOG_CRITICAL(Frontend, "Unknown Qt platform {}!", platform_name.toStdString());
    return Core::Frontend::WindowSystemType::Windows;
} // namespace Core::Frontend::WindowSystemType

Core::Frontend::EmuWindow::WindowSystemInfo GetWindowSystemInfo(QWindow* window)
{
    Core::Frontend::EmuWindow::WindowSystemInfo wsi;
    wsi.type = GetWindowSystemType();

#if defined(WIN32)
    // Our Win32 Qt external doesn't have the private API.
    wsi.render_surface = reinterpret_cast<void*>(window->winId());
#elif defined(__APPLE__)
    wsi.render_surface = reinterpret_cast<void* (*) (id, SEL)>(
        objc_msgSend)(reinterpret_cast<id>(window->winId()), sel_registerName("layer"));
#else
    QPlatformNativeInterface* pni = QGuiApplication::platformNativeInterface();
    wsi.display_connection = pni->nativeResourceForWindow("display", window);
    if (wsi.type == Core::Frontend::WindowSystemType::Wayland)
        wsi.render_surface = window ? pni->nativeResourceForWindow("surface", window) : nullptr;
    else
        wsi.render_surface = window ? reinterpret_cast<void*>(window->winId()) : nullptr;
#endif
    wsi.render_surface_scale = window ? static_cast<float>(window->devicePixelRatio()) : 1.0f;

    return wsi;
}

const QString tr(const char* str)
{
    return QGuiApplication::tr(str);
}

const QString tr(const std::string& str)
{
    return QGuiApplication::tr(str.c_str());
}

#ifdef YUZU_QT_WIDGETS
void Init(QWidget* root)
#else
void Init(QObject* root)
#endif
{
    system = std::make_unique<Core::System>();
    rootObject = root;
    vfs = std::make_unique<FileSys::RealVfsFilesystem>();
    provider = std::make_unique<FileSys::ManualContentProvider>();

    // initialization stuff
    Common::FS::CreateEdenPaths();

    system->Initialize();

    Common::Log::Initialize();
    Common::Log::Start();

    Network::Init();

    QtCommon::Meta::RegisterMetaTypes();

    // build version
    const auto branch_name = std::string(Common::g_scm_branch);
    const auto description = std::string(Common::g_scm_desc);
    const auto build_id = std::string(Common::g_build_id);

    const auto yuzu_build = fmt::format("Eden Development Build | {}-{}", branch_name, description);
    const auto override_build =
        fmt::format(fmt::runtime(std::string(Common::g_title_bar_format_idle)), build_id);
    const auto yuzu_build_version = override_build.empty() ? yuzu_build : override_build;
    const auto processor_count = std::thread::hardware_concurrency();

    // info logging
    LOG_INFO(Frontend, "Eden Version: {}", yuzu_build_version);
    LogRuntimes();
#ifdef ARCHITECTURE_x86_64
    const auto& caps = Common::g_cpu_caps;
    std::string cpu_string = caps.cpu_string;
    if (caps.avx || caps.avx2 || caps.avx512f) {
        cpu_string += " | AVX";
        if (caps.avx512f) {
            cpu_string += "512";
        } else if (caps.avx2) {
            cpu_string += '2';
        }
        if (caps.fma) {
            cpu_string += " | FMA";
        }
    }
    LOG_INFO(Frontend, "Host CPU: {}", cpu_string);
    if (std::optional<int> processor_core = Common::GetProcessorCount()) {
        LOG_INFO(Frontend, "Host CPU Cores: {}", *processor_core);
    }
#endif
    LOG_INFO(Frontend, "Host CPU Threads: {}", processor_count);
    LOG_INFO(Frontend, "Host OS: {}", PrettyProductName().toStdString());
    LOG_INFO(Frontend, "Host RAM: {:.2f} GiB",
             Common::GetMemInfo().TotalPhysicalMemory / f64{1_GiB});
    LOG_INFO(Frontend, "Host Swap: {:.2f} GiB", Common::GetMemInfo().TotalSwapMemory / f64{1_GiB});
#ifdef _WIN32
    LOG_INFO(Frontend, "Host Timer Resolution: {:.4f} ms", std::chrono::duration_cast<std::chrono::duration<f64, std::milli>>(Common::Windows::SetCurrentTimerResolutionToMaximum()).count());
#endif

    // Remove cached contents generated during the previous session
    RemoveCachedContents();
}

std::filesystem::path GetEdenCommand() {
    std::filesystem::path command;

    QString appimage = QString::fromLocal8Bit(getenv("APPIMAGE"));
    if (!appimage.isEmpty()) {
        command = std::filesystem::path{appimage.toStdString()};
    } else {
        const QStringList args = QGuiApplication::arguments();
        command = args[0].toStdString();
    }

    // If relative path, make it an absolute path
    if (command.c_str()[0] == '.') {
        command = Common::FS::GetCurrentDir() / command;
    }

    return command;
}

} // namespace QtCommon
