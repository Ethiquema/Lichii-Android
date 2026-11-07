// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "core/core.h"
#include "core/hle/kernel/svc_results.h"
#include "core/hle/service/ipc_helpers.h"
#include "core/hle/service/audio/audio.h"
#include "core/hle/service/audio/nx2_audio_user_client.h"
#include "core/hle/service/audio/audio_controller.h"
#include "core/hle/service/audio/audio_in_manager.h"
#include "core/hle/service/audio/audio_out_manager.h"
#include "core/hle/service/audio/audio_renderer_manager.h"
#include "core/hle/service/audio/final_output_recorder_manager.h"
#include "core/hle/service/audio/final_output_recorder_manager_for_applet.h"
#include "core/hle/service/audio/hardware_opus_decoder_manager.h"
#include "core/hle/service/server_manager.h"
#include "core/hle/service/service.h"

namespace Service::Audio {

class INativeAudioManager final : public ServiceFramework<INativeAudioManager> {
public:
    INativeAudioManager(Core::System& system_, std::shared_ptr<IAudioInManager> input,
                        std::shared_ptr<IAudioOutManager> output,
                        std::shared_ptr<IAudioRendererManager> renderer)
        : ServiceFramework{system_, "aud:u"}, m_input{std::move(input)},
          m_output{std::move(output)}, m_renderer{std::move(renderer)} {
        static const FunctionInfo functions[] = {
            {0, &INativeAudioManager::GetAudioInManager, "GetAudioInManager"},
            {1, &INativeAudioManager::GetAudioOutManager, "GetAudioOutManager"},
            {2, &INativeAudioManager::GetAudioRendererManager, "GetAudioRendererManager"},
        };
        RegisterHandlers(functions);
    }

    Result HandleSyncRequest(Kernel::KServerSession& session, HLERequestContext& ctx) override {
        if (!ctx.IsTipc()) {
            return ServiceFrameworkBase::HandleSyncRequest(session, ctx);
        }
        const auto guard = LockService();
        LOG_WARNING(Service_Audio, "Native audio unsupported command={}, words={:08x}", ctx.GetCommand(),
                    fmt::join(std::span<const u32>(ctx.CommandBuffer() + 2,
                        std::min<u32>(ctx.CommandBuffer()[1], 32)), ","));
        InvokeRequestTipc(ctx);
        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(Kernel::ResultNotImplemented.raw);
        return system.IsPoweredOn() ? ctx.WriteToOutgoingCommandBuffer() : ResultSuccess;
    }

private:
    void GetAudioInManager(HLERequestContext& ctx) {
        IPC::ResponseBuilder rb{ctx, 2, 0, 1};
        rb.Push(ResultSuccess);
        rb.PushIpcInterface(ctx, m_input);
    }
    void GetAudioOutManager(HLERequestContext& ctx) {
        IPC::ResponseBuilder rb{ctx, 2, 0, 1};
        rb.Push(ResultSuccess);
        rb.PushIpcInterface(ctx, m_output);
    }
    void GetAudioRendererManager(HLERequestContext& ctx) {
        LOG_DEBUG(Service_Audio, "Native user client opens existing audio renderer manager");
        IPC::ResponseBuilder rb{ctx, 2, 0, 1};
        rb.Push(ResultSuccess);
        rb.PushIpcInterface(ctx, m_renderer);
    }
    std::shared_ptr<IAudioInManager> m_input;
    std::shared_ptr<IAudioOutManager> m_output;
    std::shared_ptr<IAudioRendererManager> m_renderer;
};

class IAudioOutManagerForApplet final : public ServiceFramework<IAudioOutManagerForApplet> {
public:
    explicit IAudioOutManagerForApplet(Core::System& system_)
        : ServiceFramework{system_, "audout:a"} {}

    static constexpr auto functions = CreateStaticMap(
            FunctionInfo{0, nullptr, "RequestSuspend"},
            FunctionInfo{1, nullptr, "RequestResume"},
            FunctionInfo{2, nullptr, "GetProcessMasterVolume"},
            FunctionInfo{3, nullptr, "SetProcessMasterVolume"},
            FunctionInfo{4, nullptr, "GetProcessRecordVolume"},
            FunctionInfo{5, nullptr, "SetProcessRecordVolume"}
        );
    std::optional<FunctionInfoBase> FindRequest(u32 key) override {
        return HandlerTableGenerateWithFind(key, functions);
    }
};

class IAudioSnoopManager final : public ServiceFramework<IAudioSnoopManager> {
public:
    explicit IAudioSnoopManager(Core::System& system_)
        : ServiceFramework{system_, "auddev"} {}

    static constexpr auto functions = CreateStaticMap(
            FunctionInfo{0, nullptr, "GetDspStatistics"},
            FunctionInfo{1, nullptr, "GetAppletStateSummaries"},
            FunctionInfo{2, nullptr, "SetDspStatisticsParameter"},
            FunctionInfo{3, nullptr, "GetDspStatisticsParameter"},
            FunctionInfo{6, nullptr, "GetDspUsage"}
        );
    std::optional<FunctionInfoBase> FindRequest(u32 key) override {
        return HandlerTableGenerateWithFind(key, functions);
    }
};

class IAudioInManagerForApplet final : public ServiceFramework<IAudioInManagerForApplet> {
public:
    explicit IAudioInManagerForApplet(Core::System& system_)
        : ServiceFramework{system_, "audin:a"} {}

    static constexpr auto functions = CreateStaticMap(
            FunctionInfo{0, nullptr, "RequestSuspend"},
            FunctionInfo{1, nullptr, "RequestResume"},
            FunctionInfo{2, nullptr, "GetProcessMasterVolume"},
            FunctionInfo{3, nullptr, "SetProcessMasterVolume"}
        );
    std::optional<FunctionInfoBase> FindRequest(u32 key) override {
        return HandlerTableGenerateWithFind(key, functions);
    }
};

class IAudioRendererManagerForApplet final : public ServiceFramework<IAudioRendererManagerForApplet> {
public:
    explicit IAudioRendererManagerForApplet(Core::System& system_)
        : ServiceFramework{system_, "audren:a"} {}

    static constexpr auto functions = CreateStaticMap(
            FunctionInfo{0, nullptr, "RequestSuspend"},
            FunctionInfo{1, nullptr, "RequestResume"},
            FunctionInfo{2, nullptr, "GetProcessMasterVolume"},
            FunctionInfo{3, nullptr, "SetProcessMasterVolume"},
            FunctionInfo{4, nullptr, "RegisterAppletResourceUserId"},
            FunctionInfo{5, nullptr, "UnregisterAppletResourceUserId"},
            FunctionInfo{6, nullptr, "GetProcessRecordVolume"},
            FunctionInfo{7, nullptr, "SetProcessRecordVolume"}
        );
    std::optional<FunctionInfoBase> FindRequest(u32 key) override {
        return HandlerTableGenerateWithFind(key, functions);
    }
};

class IAudioOutManagerForDebugger final : public ServiceFramework<IAudioOutManagerForDebugger> {
public:
    explicit IAudioOutManagerForDebugger(Core::System& system_)
        : ServiceFramework{system_, "audout:d"} {}

    static constexpr auto functions = CreateStaticMap(
            FunctionInfo{0, nullptr, "RequestSuspend"},
            FunctionInfo{1, nullptr, "RequestResume"}
        );
    std::optional<FunctionInfoBase> FindRequest(u32 key) override {
        return HandlerTableGenerateWithFind(key, functions);
    }
};

class IAudioInManagerForDebugger final : public ServiceFramework<IAudioInManagerForDebugger> {
public:
    explicit IAudioInManagerForDebugger(Core::System& system_)
        : ServiceFramework{system_, "audin:d"} {}

    static constexpr auto functions = CreateStaticMap(
            FunctionInfo{0, nullptr, "RequestSuspend"},
            FunctionInfo{1, nullptr, "RequestResume"}
        );
    std::optional<FunctionInfoBase> FindRequest(u32 key) override {
        return HandlerTableGenerateWithFind(key, functions);
    }
};

class IFinalOutputRecorderManagerForDebugger final : public ServiceFramework<IFinalOutputRecorderManagerForDebugger> {
public:
    explicit IFinalOutputRecorderManagerForDebugger(Core::System& system_)
        : ServiceFramework{system_, "audrec:d"} {}

    static constexpr auto functions = CreateStaticMap(
            FunctionInfo{0, nullptr, "RequestSuspend"},
            FunctionInfo{1, nullptr, "RequestResume"}
        );
    std::optional<FunctionInfoBase> FindRequest(u32 key) override {
        return HandlerTableGenerateWithFind(key, functions);
    }
};

class IAudioRendererManagerForDebugger final : public ServiceFramework<IAudioRendererManagerForDebugger> {
public:
    explicit IAudioRendererManagerForDebugger(Core::System& system_)
        : ServiceFramework{system_, "audren:d"} {}

    static constexpr auto functions = CreateStaticMap(
            FunctionInfo{0, nullptr, "RequestSuspend"},
            FunctionInfo{1, nullptr, "RequestResume"}
        );
    std::optional<FunctionInfoBase> FindRequest(u32 key) override {
        return HandlerTableGenerateWithFind(key, functions);
    }
};

class IAudioSystemManagerForApplet final : public ServiceFramework<IAudioSystemManagerForApplet> {
public:
    explicit IAudioSystemManagerForApplet(Core::System& system_)
        : ServiceFramework{system_, "aud:a"} {}

    static constexpr auto functions = CreateStaticMap(
            FunctionInfo{0, nullptr, "RegisterAppletResourceUserId"},
            FunctionInfo{1, nullptr, "UnregisterAppletResourceUserId"},
            FunctionInfo{2, nullptr, "RequestSuspendAudio"},
            FunctionInfo{3, nullptr, "RequestResumeAudio"},
            FunctionInfo{4, nullptr, "GetAudioOutputProcessMasterVolume"},
            FunctionInfo{5, nullptr, "SetAudioOutputProcessMasterVolume"},
            FunctionInfo{6, nullptr, "GetAudioInputProcessMasterVolume"},
            FunctionInfo{7, nullptr, "SetAudioInputProcessMasterVolume"},
            FunctionInfo{8, nullptr, "GetAudioOutputProcessRecordVolume"},
            FunctionInfo{9, nullptr, "SetAudioOutputProcessRecordVolume"},
            FunctionInfo{10, nullptr, "GetAppletStateSummaries"}
        );
    std::optional<FunctionInfoBase> FindRequest(u32 key) override {
        return HandlerTableGenerateWithFind(key, functions);
    }
};

class IAudioSystemManagerForDebugger final : public ServiceFramework<IAudioSystemManagerForDebugger> {
public:
    explicit IAudioSystemManagerForDebugger(Core::System& system_)
        : ServiceFramework{system_, "aud:d"} {}

    static constexpr auto functions = CreateStaticMap(
            FunctionInfo{0, nullptr, "RequestSuspendAudioForDebug"},
            FunctionInfo{1, nullptr, "RequestResumeAudioForDebug"}
        );
    std::optional<FunctionInfoBase> FindRequest(u32 key) override {
        return HandlerTableGenerateWithFind(key, functions);
    }
};

void LoopProcess(Core::System& system) {
    auto server_manager = std::make_unique<ServerManager>(system);
    auto sysmix = std::make_shared<Nx2SysmixStreams>(system);

    server_manager->RegisterNamedService("aud:a", std::make_shared<IAudioSystemManagerForApplet>(system), 30);
    server_manager->RegisterNamedService("aud:d", std::make_shared<IAudioSystemManagerForDebugger>(system), 30);

    server_manager->RegisterNamedService("audout:d", std::make_shared<IAudioOutManagerForDebugger>(system), 30);
    server_manager->RegisterNamedService("audin:d", std::make_shared<IAudioInManagerForDebugger>(system), 30);
    server_manager->RegisterNamedService("audrec:d", std::make_shared<IFinalOutputRecorderManagerForDebugger>(system), 30);
    server_manager->RegisterNamedService("audren:d", std::make_shared<IAudioRendererManagerForDebugger>(system), 30);

    auto audio_input = std::make_shared<IAudioInManager>(system);
    server_manager->RegisterNamedService("audin:u", audio_input, 30);
    server_manager->RegisterNamedService("audin:a", std::make_shared<IAudioInManagerForApplet>(system), 30);
    auto audio_output = std::make_shared<IAudioOutManager>(system);
    server_manager->RegisterNamedService("audout:u", audio_output, 30);
    server_manager->RegisterNamedService("audout:a", std::make_shared<IAudioOutManagerForApplet>(system), 30);
    server_manager->RegisterNamedService("auddev", std::make_shared<IAudioSnoopManager>(system), 30);
    // Depends on audout:u and audin:u on ctor!
    server_manager->RegisterNamedService("audctl", std::make_shared<IAudioController>(system), 30);
    server_manager->RegisterNamedService("audrec:a", std::make_shared<IFinalOutputRecorderManagerForApplet>(system), 30);
    server_manager->RegisterNamedService("audrec:u", std::make_shared<IFinalOutputRecorderManager>(system), 30);
    server_manager->RegisterNamedService("audren:u", std::make_shared<IAudioRendererManager>(system), 30);
    // Switch 2 audio entry point (nn::audio::CreateUserClientSessionByHipc).
    server_manager->RegisterNamedService("aud:u", std::make_shared<IUserClientSession>(system), 30);
    server_manager->RegisterNamedService("auddmg:u", std::make_shared<Devmgr::IUserSessionManager>(system), 30);
    server_manager->RegisterNamedService("audsmx:u", std::make_shared<Sysmix::IUserSessionManager>(system), 30);
    server_manager->RegisterNamedService("audren:a", std::make_shared<IAudioRendererManagerForApplet>(system), 30);
    server_manager->RegisterNamedService("hwopus", std::make_shared<IHardwareOpusDecoderManager>(system), 25);
    server_manager->RegisterNamedService("aud:u",
        std::make_shared<INativeAudioManager>(system, audio_input, audio_output, audio_renderer), 30);
    ServerManager::RunServer(std::move(server_manager));
}

} // namespace Service::Audio
