// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: Copyright 2022 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <condition_variable>
#include <mutex>
#include <span>
#include <vector>
#include <queue>

#include "common/common_funcs.h"
#include "core/internal_network/sockets.h"
#include "network/room_member.h"

namespace Network {

class ProxySocket : public SocketBase {
public:
    explicit ProxySocket() noexcept;
    ~ProxySocket() override;

    bool AcceptsProxyPacket(const ProxyPacket& packet) override;

    void HandleProxyPacket(const ProxyPacket& packet) override;
    Errno Initialize(Domain domain, Type type, Protocol socket_protocol) override;
    Errno Close() override;
    std::pair<AcceptResult, Errno> Accept() override;
    Errno Connect(Network::SockAddrIn addr_in) override;
    std::pair<Network::SockAddrIn, Errno> GetPeerName() override;
    std::pair<Network::SockAddrIn, Errno> GetSockName() override;
    Errno Bind(Network::SockAddrIn addr) override;
    Errno Listen(s32 backlog) override;
    Errno Shutdown(ShutdownHow how) override;
    std::pair<s32, Errno> Recv(int flags, std::span<u8> message) override;
    std::pair<s32, Errno> RecvFrom(int flags, std::span<u8> message, Network::SockAddrIn* addr) override;
    std::pair<s32, Errno> ReceivePacket(int flags, std::span<u8> message, Network::SockAddrIn* addr, std::size_t max_length);
    std::pair<s32, Errno> Send(std::span<const u8> message, int flags) override;
    void SendPacket(ProxyPacket& packet);
    std::pair<s32, Errno> SendTo(u32 flags, std::span<const u8> message, const Network::SockAddrIn* addr) override;
    bool GetNonBlock() override;
    Errno SetNonBlock(bool enable) override;
    Errno SetSockOpt(Network::SocketLevel level, Network::OptName option, std::span<const u8> value) override;
    std::pair<Errno, Errno> GetPendingError() override;
    bool IsOpened() const override;

private:
    /// Expects packets_mutex to be held
    bool MatchesPacket(const ProxyPacket& packet) const;

    /// Takes the front packet as RecvFrom does, waiting for one as the socket's mode has it unless
    /// flags holds FLAG_MSG_DONTWAIT
    std::pair<s32, Errno> Receive(int flags, std::span<u8> message, SockAddrIn* addr,
                                  u32& msg_flags);

    bool broadcast = false;
    bool reuse_addr = false;
    bool no_delay = false;
    bool closed = false;
    u32 send_timeout = 0;
    u32 receive_timeout = 0;
    bool is_bound = false;
    Network::SockAddrIn local_endpoint{};
    bool blocking = true;
    std::queue<ProxyPacket> received_packets;
    std::size_t front_read_offset = 0; ///< Bytes of the front TCP packet that were already read
    Protocol protocol{};
    Type socket_type{}; ///< Stream checks use the type, since protocol 0 means the default protocol

    std::mutex packets_mutex;
    std::condition_variable packets_cv;
};

} // namespace Network
