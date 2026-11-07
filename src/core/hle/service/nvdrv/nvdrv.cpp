// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: 2021 yuzu Emulator Project
// SPDX-FileCopyrightText: 2021 Skyline Team and Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <utility>
#include <vector>

#include "core/core.h"
#include "core/hle/kernel/k_event.h"
#include "core/hle/kernel/k_process.h"
#include "core/hle/service/ipc_helpers.h"
#include "core/hle/service/nvdrv/core/container.h"
#include "core/hle/service/nvdrv/devices/nvdevice.h"
#include "core/hle/service/nvdrv/devices/nvdisp_disp0.h"
#include "core/hle/service/nvdrv/devices/nvhost_as_gpu.h"
#include "core/hle/service/nvdrv/devices/nvhost_ctrl.h"
#include "core/hle/service/nvdrv/devices/nvhost_ctrl_gpu.h"
#include "core/hle/service/nvdrv/devices/nvhost_gpu.h"
#include "core/hle/service/nvdrv/devices/nvhost_tsg_gpu.h"
#include "core/hle/kernel/k_process.h"
#include "core/hle/service/nvdrv/devices/nvhost_nvdec.h"
#include "core/hle/service/nvdrv/devices/nvhost_nvdec_common.h"
#include "core/hle/service/nvdrv/devices/nvhost_nvjpg.h"
#include "core/hle/service/nvdrv/devices/nvhost_prof_gpu.h"
#include "core/hle/service/nvdrv/devices/nvhost_vic.h"
#include "core/hle/service/nvdrv/devices/nvmap.h"
#include "core/hle/service/nvdrv/devices/nx2_stub_device.h"
#include "core/hle/service/nvdrv/nvdrv.h"
#include "core/hle/service/nvdrv/nvdrv_interface.h"
#include "core/hle/service/nvdrv/nvmemp.h"
#include "core/hle/service/nvnflinger/nvnflinger.h"
#include "core/hle/service/server_manager.h"
#include "frontend_common/firmware_manager.h"
#include "video_core/gpu.h"
#include "video_core/nx2.h"

namespace Service::Nvidia {

EventInterface::EventInterface(Module& module_) : module{module_}, guard{}, on_signal{} {}

EventInterface::~EventInterface() = default;

Kernel::KEvent* EventInterface::CreateEvent(std::string name) {
    Kernel::KEvent* new_event = module.service_context.CreateEvent(std::move(name));
    return new_event;
}

void EventInterface::FreeEvent(Kernel::KEvent* event) {
    module.service_context.CloseEvent(event);
}

class NVGEM_C final : public ServiceFramework<NVGEM_C> {
public:
    explicit NVGEM_C(Core::System& system_)
        : ServiceFramework{system_, "nvgem:c"}
    {}

    std::optional<FunctionInfoBase> FindRequest(u32 key) override {
        return HandlerTableGenerateWithFind(key, functions);
    }
    static constexpr auto functions = CreateStaticMap(
        FunctionInfo{0, nullptr, "Initialize"},
        FunctionInfo{1, nullptr, "GetEventHandle"},
        FunctionInfo{2, nullptr, "ControlNotification"},
        FunctionInfo{3, nullptr, "SetNotificationPerm"},
        FunctionInfo{4, nullptr, "SetCoreDumpPerm"},
        FunctionInfo{5, nullptr, "GetAruid"},
        FunctionInfo{6, nullptr, "Reset"},
        FunctionInfo{7, nullptr, "GetAruid2"}
    );
};

class NVGEM_CD final : public ServiceFramework<NVGEM_CD> {
public:
    explicit NVGEM_CD(Core::System& system_)
        : ServiceFramework{system_, "nvgem:cd"}
    {}

    std::optional<FunctionInfoBase> FindRequest(u32 key) override {
        return HandlerTableGenerateWithFind(key, functions);
    }
    static constexpr auto functions = CreateStaticMap(
        FunctionInfo{0, nullptr, "Initialize"},
        FunctionInfo{1, nullptr, "GetAruid"},
        FunctionInfo{2, nullptr, "ReadNextBlock"},
        FunctionInfo{3, nullptr, "GetNextBlockSize"},
        FunctionInfo{4, nullptr, "ReadNextBlock2"}
    );
};

class NVDBG_D final : public ServiceFramework<NVDBG_D> {
public:
    explicit NVDBG_D(Core::System& system_)
        : ServiceFramework{system_, "nvdbg:d"}
    {}

    std::optional<FunctionInfoBase> FindRequest(u32 key) override {
        return HandlerTableGenerateWithFind(key, functions);
    }
    static constexpr auto functions = CreateStaticMap(
        FunctionInfo{0, nullptr, "Open"},
        FunctionInfo{1, nullptr, "Ioctl"},
        FunctionInfo{2, nullptr, "Close"},
        FunctionInfo{4, nullptr, "QueryEvent"},
        FunctionInfo{9, nullptr, "DumpStatus"},
        FunctionInfo{10, nullptr, "InitializeDevtools"},
        FunctionInfo{11, nullptr, "Ioctl2"},
        FunctionInfo{12, nullptr, "Ioctl3"},
        FunctionInfo{13, nullptr, "SetConfiguration"}
    );
};

void LoopProcess(Core::System& system) {
    auto server_manager = std::make_unique<ServerManager>(system);
    auto module = std::make_shared<Module>(system);
    server_manager->RegisterNamedService("nvdrv", std::make_shared<NVDRV>(system, module, "nvdrv"));

    const auto NvdrvInterfaceFactoryForApplets = [&, module] {
        return std::make_shared<NVDRV>(system, module, "nvdrv:a");
    };

    server_manager->RegisterNamedService("nvdrv:a", NvdrvInterfaceFactoryForApplets);
    server_manager->RegisterNamedService("nvdrv:s", std::make_shared<NVDRV>(system, module, "nvdrv:s"));
    server_manager->RegisterNamedService("nvdrv:t", std::make_shared<NVDRV>(system, module, "nvdrv:t"));
    server_manager->RegisterNamedService("nvmemp", std::make_shared<NVMEMP>(system));
    server_manager->RegisterNamedService("nvgem:c", std::make_shared<NVGEM_C>(system));
    server_manager->RegisterNamedService("nvgem:cd", std::make_shared<NVGEM_CD>(system));
    // +10.0.0
    if (FirmwareManager::GetFirmwareVersion(system).first.major >= 10) {
        server_manager->RegisterNamedService("nvdbg:d", std::make_shared<NVDBG_D>(system));
    }
    ServerManager::RunServer(std::move(server_manager));
}

Module::Module(Core::System& system)
    : container{system.Host1x()}, service_context{system, "nvdrv"}, events_interface{*this} {
    nx2_stub_builder = [this, &system](DeviceFD fd, const std::string& name) {
        auto device = std::make_shared<Devices::nx2_stub_device>(system, name);
        open_files.emplace(fd, device);
        return std::static_pointer_cast<Devices::nvdevice>(device);
    };
    builders["/dev/nvhost-as-gpu"] = [this, &system](DeviceFD fd) {
        auto device = std::make_shared<Devices::nvhost_as_gpu>(system, *this, container);
        return open_files.emplace(fd, std::move(device)).first;
    };
    builders["/dev/nvhost-gpu"] = [this, &system](DeviceFD fd) {
        auto device = std::make_shared<Devices::nvhost_gpu>(system, events_interface, container);
        return open_files.emplace(fd, std::move(device)).first;
    };
    builders["/dev/nvhost-tsg-gpu"] = [this, &system](DeviceFD fd) {
        auto device = std::make_shared<Devices::nvhost_tsg_gpu>(system, *this, container);
        return open_files.emplace(fd, std::move(device)).first;
    };
    builders["/dev/nvhost-ctrl-gpu"] = [this, &system](DeviceFD fd) {
        auto device = std::make_shared<Devices::nvhost_ctrl_gpu>(system, events_interface);
        return open_files.emplace(fd, std::move(device)).first;
    };
    builders["/dev/nvmap"] = [this, &system](DeviceFD fd) {
        auto device = std::make_shared<Devices::nvmap>(system, container);
        return open_files.emplace(fd, std::move(device)).first;
    };
    builders["/dev/nvdisp_disp0"] = [this, &system](DeviceFD fd) {
        auto device = std::make_shared<Devices::nvdisp_disp0>(system, container);
        return open_files.emplace(fd, std::move(device)).first;
    };
    builders["/dev/nvhost-ctrl"] = [this, &system](DeviceFD fd) {
        auto device = std::make_shared<Devices::nvhost_ctrl>(system, events_interface, container);
        return open_files.emplace(fd, std::move(device)).first;
    };
    builders["/dev/nvhost-nvdec"] = [this, &system](DeviceFD fd) {
        auto device = std::make_shared<Devices::nvhost_nvdec>(system, container);
        return open_files.emplace(fd, std::move(device)).first;
    };
    builders["/dev/nvhost-nvjpg"] = [this, &system](DeviceFD fd) {
        auto device = std::make_shared<Devices::nvhost_nvjpg>(system);
        return open_files.emplace(fd, std::move(device)).first;
    };
    builders["/dev/nvhost-prof-gpu"] = [this, &system](DeviceFD fd) {
        auto device = std::make_shared<Devices::nvhost_prof_gpu>(system);
        return open_files.emplace(fd, std::move(device)).first;
    };
    builders["/dev/nvhost-vic"] = [this, &system](DeviceFD fd) {
        auto device = std::make_shared<Devices::nvhost_vic>(system, container);
        return open_files.emplace(fd, std::move(device)).first;
    };
}

Module::~Module() {
    // Devices (including TSG-retained channels) release events and core state.
    // Destroy them while those module members are still alive.
    for (const auto& [fd, device] : open_files) { device->OnClose(fd); }
    open_files.clear();
}

NvResult Module::VerifyFD(DeviceFD fd) const {
    if (fd < 0) {
        LOG_ERROR(Service_NVDRV, "Invalid DeviceFD={}!", fd);
        return NvResult::InvalidState;
    }

    if (!FindDevice(fd)) {
        LOG_ERROR(Service_NVDRV, "Could not find DeviceFD={}!", fd);
        return NvResult::NotImplemented;
    }

    return NvResult::Success;
}

DeviceFD Module::Open(const std::string& device_name, NvCore::SessionId session_id) {
    if (device_name == "/dev/nvhost-tsg-gpu") {
        const auto* session = container.GetSession(session_id);
        if (!session || !session->process || !session->process->UsesPAuthReturns()) {
            return INVALID_NVDRV_FD;
        }
    }
    auto it = builders.find(device_name);
    if (it == builders.end() && device_name.starts_with("/dev/nvhost-")) {
        // Newer nvgpu nodes used by Switch 2 drivers (e.g. /dev/nvhost-tsg-gpu): serve them with
        // a logged stub instead of failing the open.
        LOG_WARNING(Service_NVDRV, "Opening unknown device {} as a Switch 2 stub", device_name);
        const DeviceFD fd = next_fd++;
        auto device = nx2_stub_builder(fd, device_name);
        device->OnOpen(session_id, fd);
        return fd;
    }
    if (it == builders.end()) {
        LOG_ERROR(Service_NVDRV, "Trying to open unknown device {}", device_name);
        return INVALID_NVDRV_FD;
    }

    const DeviceFD fd = next_fd++;
    std::shared_ptr<Devices::nvdevice> device;
    {
        std::scoped_lock lock(open_files_mutex);
        device = it->second(fd)->second;
    }

    device->OnOpen(session_id, fd);
    if (container.IsSessionActive(session_id)) {
        open_file_sessions.emplace(fd, session_id);
    }

    return fd;
}

NvResult Module::Ioctl1(DeviceFD fd, Ioctl command, std::span<const u8> input,
                        std::span<u8> output) {
    if (fd < 0) {
        LOG_ERROR(Service_NVDRV, "Invalid DeviceFD={}!", fd);
        return NvResult::InvalidState;
    }

    const auto device = FindDevice(fd);
    if (!device) {
        LOG_ERROR(Service_NVDRV, "Could not find DeviceFD={}!", fd);
        return NvResult::NotImplemented;
    }

    return device->Ioctl1(fd, command, input, output);
}

NvResult Module::Ioctl2(DeviceFD fd, Ioctl command, std::span<const u8> input,
                        std::span<const u8> inline_input, std::span<u8> output) {
    if (fd < 0) {
        LOG_ERROR(Service_NVDRV, "Invalid DeviceFD={}!", fd);
        return NvResult::InvalidState;
    }

    const auto device = FindDevice(fd);
    if (!device) {
        LOG_ERROR(Service_NVDRV, "Could not find DeviceFD={}!", fd);
        return NvResult::NotImplemented;
    }

    return device->Ioctl2(fd, command, input, inline_input, output);
}

NvResult Module::Ioctl3(DeviceFD fd, Ioctl command, std::span<const u8> input, std::span<u8> output,
                        std::span<u8> inline_output) {
    if (fd < 0) {
        LOG_ERROR(Service_NVDRV, "Invalid DeviceFD={}!", fd);
        return NvResult::InvalidState;
    }

    const auto device = FindDevice(fd);
    if (!device) {
        LOG_ERROR(Service_NVDRV, "Could not find DeviceFD={}!", fd);
        return NvResult::NotImplemented;
    }

    return device->Ioctl3(fd, command, input, output, inline_output);
}

NvResult Module::Close(DeviceFD fd) {
    if (fd < 0) {
        LOG_ERROR(Service_NVDRV, "Invalid DeviceFD={}!", fd);
        return NvResult::InvalidState;
    }

    const auto device = FindDevice(fd);
    if (!device) {
        LOG_ERROR(Service_NVDRV, "Could not find DeviceFD={}!", fd);
        return NvResult::NotImplemented;
    }

    {
        std::scoped_lock lock(open_files_mutex);
        open_files.erase(fd);
    }
    device->OnClose(fd);

    return NvResult::Success;
}

NvResult Module::QueryEvent(DeviceFD fd, u32 event_id, Kernel::KEvent*& event) {
    if (fd < 0) {
        LOG_ERROR(Service_NVDRV, "Invalid DeviceFD={}!", fd);
        return NvResult::InvalidState;
    }

    const auto device = FindDevice(fd);
    if (!device) {
        LOG_ERROR(Service_NVDRV, "Could not find DeviceFD={}!", fd);
        return NvResult::NotImplemented;
    }

    event = device->QueryEvent(event_id);
    if (!event) {
        return NvResult::BadParameter;
    }
    return NvResult::Success;
}

static bool ContainsSession(std::span<const NvCore::SessionId> session_ids,
                            NvCore::SessionId session_id) {
    return std::ranges::any_of(session_ids, [session_id](const auto candidate) {
        return candidate.id == session_id.id;
    });
}

static void AppendUniqueSession(std::vector<NvCore::SessionId>& session_ids,
                                NvCore::SessionId session_id) {
    if (!ContainsSession(session_ids, session_id)) {
        session_ids.push_back(session_id);
    }
}

size_t Module::CloseFilesForSessions(std::span<const NvCore::SessionId> session_ids) {
    std::vector<DeviceFD> fds;
    fds.reserve(open_file_sessions.size());

    for (const auto& [fd, session_id] : open_file_sessions) {
        if (ContainsSession(session_ids, session_id)) {
            fds.push_back(fd);
        }
    }

    for (const auto fd : fds) {
        Close(fd);
    }

    return fds.size();
}

void Module::CloseSession(NvCore::SessionId session_id) {
    container.CloseSession(session_id);
}

void Module::TrackSessionAruid(NvCore::SessionId session_id, u64 aruid) {
    const bool active = container.IsSessionActive(session_id);
    if (active) {
        session_aruids[session_id.id] = aruid;
    }
}

std::vector<NvCore::SessionId> Module::GetSessionIdsForAruid(u64 aruid) const {
    std::vector<NvCore::SessionId> session_ids;
    for (const auto& [session_id, session_aruid] : session_aruids) {
        if (session_aruid == aruid) {
            session_ids.push_back(NvCore::SessionId{session_id});
        }
    }

    return session_ids;
}

size_t Module::ResetForProcess(Kernel::KProcess* process) {
    const auto process_id = process != nullptr ? process->GetProcessId() : 0;
    auto session_ids = container.GetSessionIdsForProcess(process);

    if (process_id != 0) {
        for (const auto session_id : GetSessionIdsForAruid(process_id)) {
            AppendUniqueSession(session_ids, session_id);
        }
    }

    const auto active_session_ids = container.GetActiveSessionIds();
    const auto active_before = active_session_ids.size();
    const bool has_active_candidate =
        std::ranges::any_of(session_ids, [this](const auto session_id) {
            return container.IsSessionActive(session_id);
        });
    bool used_active_sessions = false;
    if (!has_active_candidate && !active_session_ids.empty()) {
        for (const auto session_id : active_session_ids) {
            AppendUniqueSession(session_ids, session_id);
        }
        used_active_sessions = true;
    }

    const auto closed_files = CloseFilesForSessions(session_ids);
    const auto closed_sessions = container.CloseSessions(session_ids);
    for (const auto session_id : session_ids) {
        if (!container.IsSessionActive(session_id)) {
            session_aruids.erase(session_id.id);
        }
    }

    if (used_active_sessions) {
        LOG_WARNING(Service_NVDRV,
                    "NextLoad: NVDRV reset used active sessions because process-owned "
                    "sessions were not found, process_id={}, sessions={}, files={}, active_before={}",
                    process_id, closed_sessions, closed_files, active_before);
    }
    return closed_sessions;
}

} // namespace Service::Nvidia
