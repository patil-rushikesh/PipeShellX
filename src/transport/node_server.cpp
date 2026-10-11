#include "psx/transport/node_server.hpp"

#include <chrono>
#include <utility>

namespace psx::transport {

using psx::os::Interest;
using psx::os::Readiness;

NodeServer::NodeServer(psx::runtime::Reactor& reactor,
                       psx::os::Socket listener,
                       psx::os::TlsConfig serverConfig,
                       std::function<bool(std::string_view)> authorize,
                       NodeStageRunner::CommandValidator validateCommand)
    : reactor_(reactor), listener_(std::move(listener)), serverConfig_(std::move(serverConfig)),
      authorize_(std::move(authorize)), validateCommand_(std::move(validateCommand)) {
    serverConfig_.isServer = true;
}

NodeServer::~NodeServer() {
    lifetime_.reset();
    if (reapTimer_ != 0) {
        (void)reactor_.cancel(reapTimer_);
        reapTimer_ = 0;
    }
    if (listenerToken_ != 0) {
        (void)reactor_.unwatch(listenerToken_);
    }
}

psx::Result<void> NodeServer::start() {
    auto watched = reactor_.watch(listener_.handle(), Interest::Readable, [this](Readiness) { onAccept(); });
    if (!watched.ok()) {
        return watched.error();
    }
    listenerToken_ = watched.value();
    return {};
}

void NodeServer::onAccept() {
    while (true) { // edge-triggered: drain the backlog to WouldBlock
        auto accepted = listener_.accept();
        if (!accepted.ok()) {
            break;
        }
        acceptOne(std::move(accepted.value()));
    }
}

void NodeServer::acceptOne(psx::os::Socket socket) {
    auto tls = psx::os::Tls::create(serverConfig_);
    if (!tls.ok()) {
        return; // cannot secure this connection; the socket closes on scope exit
    }
    const std::uint64_t id = nextConnId_++;
    auto runner = std::make_unique<NodeStageRunner>(reactor_, validateCommand_, clock_);
    auto transport = std::make_unique<NativeTransport>(
        reactor_, std::move(socket), std::move(tls.value()), Role::Node, *runner,
        NativeTransport::Callbacks{
            .authorize = authorize_, .onReady = {}, .onError = [this, id](const psx::Error&) { dropConnection(id); }},
        kDefaultStreamWindow, kDefaultLease);
    runner->bind(transport->session());
    if (auto started = transport->start(); !started.ok()) {
        return; // drop this connection
    }
    connections_.emplace(id, Connection{std::move(runner), std::move(transport)});
    ++acceptedTotal_;
}

NodeServer::Metrics NodeServer::metrics() const {
    Metrics snapshot;
    snapshot.acceptedTotal = acceptedTotal_;
    snapshot.activeConnections = connections_.size();
    snapshot.lamportClock = clock_.value();
    for (const auto& [id, connection] : connections_) {
        snapshot.activeStages += connection.runner->runningStages();
    }
    return snapshot;
}

void NodeServer::dropConnection(std::uint64_t id) {
    // Deferred: dropConnection runs inside the connection's own error callback, up
    // its handler stack. Destroying the transport now would free objects the stack
    // still unwinds through. A zero-delay timer reaps after this dispatch returns
    // (runOnce fires due timers only after the I/O handlers complete).
    pendingRemoval_.push_back(id);
    if (!reapScheduled_) {
        reapScheduled_ = true;
        std::weak_ptr<void> lifetime = lifetime_;
        reapTimer_ = reactor_.after(std::chrono::milliseconds(0), [this, lifetime] {
            if (lifetime.lock()) {
                reap();
            }
        });
    }
}

void NodeServer::reap() {
    reapTimer_ = 0;
    reapScheduled_ = false;
    for (const std::uint64_t id : pendingRemoval_) {
        connections_.erase(id);
    }
    pendingRemoval_.clear();
}

} // namespace psx::transport
