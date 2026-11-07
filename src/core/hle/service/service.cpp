// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "common/scope_exit.h"
#include <vector>
#include <tuple>
#include <mutex>
#include <map>
#include <chrono>
#include <cstdlib>
#include <limits>
#include <fmt/ranges.h>
#include "common/assert.h"
#include "common/logging.h"
#include "common/settings.h"
#include "core/core.h"
#include "core/hle/ipc.h"
#include "core/hle/kernel/k_process.h"
#include "core/hle/kernel/k_thread.h"
#include "core/hle/kernel/kernel.h"
#include "core/hle/service/ipc_helpers.h"
#include "core/hle/service/service.h"
#include "core/hle/service/nx2_stub_interface.h"
#include "core/hle/service/sm/sm.h"
#include "core/reporter.h"
#include "frontend_common/firmware_manager.h"

namespace Service {

/// @brief Creates a function string for logging, complete with the name (or header code, depending
/// on what's passed in) the port name, and all the cmd_buf arguments.
[[maybe_unused]] static std::string MakeFunctionString(std::string_view name, std::string_view port_name, const u32* cmd_buf) {
    // Number of params == bits 0-5 + bits 6-11
    int num_params = (cmd_buf[0] & 0x3F) + ((cmd_buf[0] >> 6) & 0x3F);
    std::string function_string = fmt::format("fn '{}': port={}", name, port_name);
    for (int i = 1; i <= num_params; ++i)
        function_string += fmt::format(", cmd_buf[{}]={:#x}", i, cmd_buf[i]);
    return function_string;
}

ServiceFrameworkBase::ServiceFrameworkBase(Core::System& system_, const char* service_name_, u32 max_sessions_, InvokerFn* handler_invoker_)
    : SessionRequestHandler(system_.Kernel(), service_name_)
    , system{system_}
    , service_name{service_name_}
    , handler_invoker{handler_invoker_}
    , max_sessions{max_sessions_}
{}

ServiceFrameworkBase::~ServiceFrameworkBase() {
    // Wait for other threads to release access before destroying
    const auto guard = ServiceFrameworkBase::LockService();
}

void ServiceFrameworkBase::RegisterHandlersBase(const FunctionInfoBase* functions, std::size_t n) {
    // Usually this array is sorted by id already, so hint to insert at the end
    handlers.reserve(handlers.size() + n);
    for (std::size_t i = 0; i < n; ++i)
        handlers.emplace_hint(handlers.cend(), functions[i].expected_header, functions[i]);
}

void ServiceFrameworkBase::RegisterHandlersBaseTipc(const FunctionInfoBase* functions, std::size_t n) {
    // Usually this array is sorted by id already, so hint to insert at the end
    handlers_tipc.reserve(handlers_tipc.size() + n);
    for (std::size_t i = 0; i < n; ++i)
        handlers_tipc.emplace_hint(handlers_tipc.cend(), functions[i].expected_header, functions[i]);
}

void ServiceFrameworkBase::ReportUnimplementedFunction(HLERequestContext& ctx,
                                                       const FunctionInfoBase* info) {
    // EDEN_NX2_AUTO_STUB=1 auto-stubs every unknown command without touching the config. Switch 2
    // programs are always auto-stubbed: they still call many commands nobody has implemented.
    static const bool env_auto_stub = std::getenv("EDEN_NX2_AUTO_STUB") != nullptr;
    const auto* const caller = ctx.GetThread().GetOwnerProcess();
    const bool auto_stub = Settings::values.use_auto_stub || env_auto_stub ||
                           std::string_view{service_name}.starts_with(Nx2StubPrefix) ||
                           (caller != nullptr && caller->IsSwitch2());
    // Auto-stubbed commands are reported once each: title 1 polls nx2:camera 29 every frame
    // (~47 calls a second), and every report wrote a Critical line, which flushes the log file.
    if (auto_stub) {
        static std::mutex reported_mutex;
        static std::set<std::pair<std::string, u32>> reported;
        std::scoped_lock lk{reported_mutex};
        if (!reported.emplace(service_name, ctx.GetCommand()).second) {
            IPC::ResponseBuilder rb{ctx, 2};
            rb.Push(ResultSuccess);
            return;
        }
    }

    auto cmd_buf = ctx.CommandBuffer();
    std::string function_name = bool(info) ? info->name : "<unknown>";

    fmt::memory_buffer buf;
    fmt::format_to(std::back_inserter(buf), "function '{}({})': port='{}' cmd_buf={{[0]={:#x}", ctx.GetCommand(), function_name, service_name, cmd_buf[0]);
    // The Switch 2 video decoder sessions are being reverse engineered: dump whole requests.
    const int last_word = std::string_view{service_name}.starts_with("nx2:vdec") ? 47 : 8;
    for (int i = 1; i <= last_word; ++i)
        fmt::format_to(std::back_inserter(buf), ", [{}]={:#x}", i, cmd_buf[i]);
    buf.push_back('}');

    if (ctx.GetThread().GetOwnerProcess()->UsesPAuthReturns()) {
        const auto& context = ctx.GetThread().GetContext();
        LOG_DEBUG(Service, "Unsupported IPC caller pc={:#x}, lr={:#x}, sp={:#x}, fp={:#x}",
                  context.pc, context.lr, context.sp, context.fp);
        u64 frame = context.fp;
        for (u32 depth = 0; depth < 8 && frame != 0; ++depth) {
            if (frame > (std::numeric_limits<u64>::max)() - 15 ||
                !ctx.GetMemory().IsValidVirtualAddress(frame) ||
                !ctx.GetMemory().IsValidVirtualAddress(frame + 15)) {
                break;
            }
            const u64 next = ctx.GetMemory().Read64(frame);
            LOG_DEBUG(Service, "Unsupported IPC frame {}: return={:#x}", depth,
                      ctx.GetMemory().Read64(frame + 8));
            if (next <= frame) {
                break;
            }
            frame = next;
        }
    }
    system.GetReporter().SaveUnimplementedFunctionReport(ctx, ctx.GetCommand(), function_name, service_name);
    UNIMPLEMENTED_MSG("Unknown / unimplemented {}", fmt::to_string(buf));
    // EDEN_NX2_AUTO_STUB=1 auto-stubs every unknown command without touching the config. Switch 2
    // programs are always auto-stubbed: they still call many commands nobody has implemented.
    static const bool env_auto_stub = std::getenv("EDEN_NX2_AUTO_STUB") != nullptr;
    const auto* const caller = ctx.GetThread().GetOwnerProcess();
    if (Settings::values.use_auto_stub || env_auto_stub ||
        std::string_view{service_name}.starts_with(Nx2StubPrefix) ||
        (caller != nullptr && caller->IsSwitch2())) {
        LOG_WARNING(Service, "Using auto stub fallback!");
        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }
}

[[nodiscard]] static bool VersionGateCheck(Core::System& system, u32 vg) {
    const auto fw = FirmwareManager::GetFirmwareVersion(system).first;
    u32 const c_maj = fw.major;
    u32 const c_min = fw.minor;
    u32 const c_pat = fw.micro;
    u32 const cg = (c_pat << 0) | (c_min << 4) | (c_maj << 8);
    u32 const sg = (vg >> 16) & 0xffff;
    u32 const ug = vg & 0xffff;
    // feature available after current
    if (sg && cg < sg)
        return false;
    // feature until xxx (0 = up to current)
    return ug ? ug >= cg : true;
}

void ServiceFrameworkBase::InvokeRequest(HLERequestContext& ctx) {
    const bool is_cmd_read = ctx.GetCommand() == 0;
    auto const info = FindRequest(ctx.GetCommand());
    if (!info.has_value() || info->handler_callback == nullptr)
        return ReportUnimplementedFunction(ctx, &*info, false);

    if (!VersionGateCheck(system, info->version_gating))
    {
        return ReportUnimplementedFunction(ctx, &*info, true);
    }

    LOG_TRACE(Service, "{}", MakeFunctionString(info->name, GetServiceName(), ctx.CommandBuffer()));
    // EDEN_NX2_IPC_TRACE=1 logs every dispatched command and the result word it returned.
    static const bool ipc_trace = std::getenv("EDEN_NX2_IPC_TRACE") != nullptr;
    if (ipc_trace) {
        LOG_INFO(Service, "ipc {}::{} ({})", service_name, ctx.GetCommand(), info->name);
    }
    handler_invoker(this, info->handler_callback, ctx);
    if (is_i_storage && is_cmd_read) {
        const auto* const process = ctx.GetThread().GetOwnerProcess();
        if (process != nullptr && system.IsNVDECActiveForProcess(process->GetId())) {
            std::this_thread::sleep_for(std::chrono::microseconds{600});
        }
    }
}

void ServiceFrameworkBase::InvokeRequestTipc(HLERequestContext& ctx) {
    auto const info = FindRequestTipc(ctx.GetCommand());
    if (!info.has_value() || info->handler_callback == nullptr)
        return ReportUnimplementedFunction(ctx, &*info, false);

    LOG_ERROR(Service, "check for {} version_gating: {}", info->name, info->version_gating);
    if (!VersionGateCheck(system, info->version_gating))
    {
        return ReportUnimplementedFunction(ctx, &*info, true);
    }

    LOG_TRACE(Service, "{}", MakeFunctionString(info->name, GetServiceName(), ctx.CommandBuffer()));
    static const bool ipc_trace = std::getenv("EDEN_NX2_IPC_TRACE") != nullptr;
    if (ipc_trace) {
        const auto* w = ctx.CommandBuffer();
        LOG_INFO(Service, "tipc {}::{} ({}) [2]={:#x} [3]={:#x} [4]={:#x}", service_name,
                 ctx.GetCommand(), info->name, w[2], w[3], w[4]);
    }
    handler_invoker(this, info->handler_callback, ctx);
}

namespace {
// EDEN_NX2_IPC_SUMMARY: every 5 s, log how often each service command ran and with which result
// since the last summary (finds a game polling something that never becomes ready).
void CountRequest(const std::string& service, u32 command, u32 type, u32 result) {
    static const bool enabled = std::getenv("EDEN_NX2_IPC_SUMMARY") != nullptr;
    if (!enabled) {
        return;
    }
    static std::mutex mutex;
    static std::map<std::tuple<std::string, u32, u32, u32>, u32> counts;
    static auto last = std::chrono::steady_clock::now();
    std::scoped_lock lock{mutex};
    ++counts[{service, command, type, result}];
    const auto now = std::chrono::steady_clock::now();
    if (now - last < std::chrono::seconds{5}) {
        return;
    }
    last = now;
    std::vector<std::pair<u32, std::string>> lines;
    for (const auto& [key, count] : counts) {
        lines.emplace_back(count, fmt::format("{}::{}{} -> {:#x}", std::get<0>(key), std::get<1>(key),
                                              std::get<2>(key) >= 16 ? " (tipc)" : "",
                                              std::get<3>(key)));
    }
    std::sort(lines.begin(), lines.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    std::string text;
    for (size_t i = 0; i < std::min<size_t>(lines.size(), 24); ++i) {
        text += fmt::format(" | {}x {}", lines[i].first, lines[i].second);
    }
    LOG_INFO(Service, "ipc summary ({} kinds){}", lines.size(), text);
    counts.clear();
}
} // Anonymous namespace

Result ServiceFrameworkBase::HandleSyncRequest(Kernel::KServerSession& session,
                                               HLERequestContext& ctx) {
    const auto guard = LockService();
    const bool diag_account_lock = std::getenv("EDEN_DIAG_THREAD_DUMP") != nullptr &&
                                   std::string_view{service_name} == "acc:u0" &&
                                   ctx.GetCommand() == 150;

    Result result = ResultSuccess;

    switch (ctx.GetCommandType()) {
    case IPC::CommandType::Close:
    case IPC::CommandType::TIPC_Close: {
        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
        result = IPC::ResultSessionClosed;
        break;
    }
    case IPC::CommandType::ControlWithContext:
    case IPC::CommandType::Control: {
        system.ServiceManager().InvokeControlRequest(ctx);
        break;
    }
    case IPC::CommandType::RequestWithContext:
    case IPC::CommandType::Request: {
        if (diag_account_lock) {
            LOG_INFO(Service, "diag: acc:u0 cmd150 before handler");
            system.DumpGuestThreads();
        }
        InvokeRequest(ctx);
        if (diag_account_lock) {
            LOG_INFO(Service, "diag: acc:u0 cmd150 after handler");
            system.DumpGuestThreads();
        }
        break;
    }
    default:
        if (ctx.IsTipc()) {
            InvokeRequestTipc(ctx);
            break;
        }

        UNIMPLEMENTED_MSG("command_type={}", ctx.GetCommandType());
        break;
    }

    // If emulation was shutdown, we are closing service threads, do not write the response back to
    // memory that may be shutting down as well.
    if (system.IsPoweredOn()) {
        if (diag_account_lock) {
            LOG_INFO(Service, "diag: acc:u0 cmd150 before outgoing write");
        }
        const Result write_result = ctx.WriteToOutgoingCommandBuffer();
        if (diag_account_lock) {
            LOG_INFO(Service, "diag: acc:u0 cmd150 after outgoing write result={:#x}",
                     write_result.raw);
            system.DumpGuestThreads();
        }
    }

    return result;
}

} // namespace Service
