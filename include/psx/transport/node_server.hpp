#pragma once

#include "psx/os/socket.hpp"
#include "psx/os/tls.hpp"
#include "psx/result.hpp"
#include "psx/runtime/lamport_clock.hpp"
#include "psx/runtime/reactor.hpp"
#include "psx/transport/native_transport.hpp"
#include "psx/transport/node_stage_runner.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace psx::transport {

struct NodeServerLifecycleProbe;

// The psx/1 node daemon: accepts mTLS connections on a listening socket and, per
// connection, runs a NativeTransport(Role::Node) + NodeStageRunner so a
// controller can execute stages. All on one reactor; not thread-safe.
class NodeServer {
public:
    // serverConfig supplies the node's cert/key and the CA it trusts; isServer is
    // forced on. authorize gates accepted peers by SAN URI (empty = authN only).
    NodeServer(psx::runtime::Reactor& reactor,
               psx::os::Socket listener,
               psx::os::TlsConfig serverConfig,
               std::function<bool(std::string_view)> authorize = {},
               NodeStageRunner::CommandValidator validateCommand = {});
    ~NodeServer();
    NodeServer(const NodeServer&) = delete;
    NodeServer& operator=(const NodeServer&) = delete;

    // Begins accepting connections.
    psx::Result<void> start();

    std::size_t connectionCount() const noexcept { return connections_.size(); }

    // A snapshot of the daemon's activity for the local metrics endpoint.
    struct Metrics {
        std::uint64_t acceptedTotal = 0;   // connections accepted since start
        std::size_t activeConnections = 0; // currently open
        std::size_t activeStages = 0;      // stages running now, across all connections
        std::uint64_t lamportClock = 0;    // this node's Lamport clock (shared by all connections)
    };
    Metrics metrics() const;

private:
    friend struct NodeServerLifecycleProbe;

    struct Connection {
        std::unique_ptr<NodeStageRunner> runner;
        std::unique_ptr<NativeTransport> transport; // references runner; destroyed first
    };

    void onAccept();
    void acceptOne(psx::os::Socket socket);
    void dropConnection(std::uint64_t id);
    void reap();

    psx::runtime::Reactor& reactor_;
    psx::os::Socket listener_;
    psx::os::TlsConfig serverConfig_;
    std::function<bool(std::string_view)> authorize_;
    NodeStageRunner::CommandValidator validateCommand_;
    psx::runtime::Token listenerToken_ = 0;
    // One Lamport clock for the whole node process, shared by every
    // connection's runner (see NodeStageRunner's shared-clock constructor).
    // Declared BEFORE connections_ on purpose: members are destroyed in
    // reverse order, so the clock outlives every runner that points at it.
    psx::runtime::LamportClock clock_;
    std::unordered_map<std::uint64_t, Connection> connections_;
    std::vector<std::uint64_t> pendingRemoval_;
    psx::runtime::TimerId reapTimer_ = 0;
    std::shared_ptr<void> lifetime_ = std::make_shared<int>(0);
    std::uint64_t nextConnId_ = 1;
    std::uint64_t acceptedTotal_ = 0;
    bool reapScheduled_ = false;
};

} // namespace psx::transport
