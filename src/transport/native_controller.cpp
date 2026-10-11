#include "psx/transport/native_controller.hpp"

#include "psx/os/socket.hpp"
#include "psx/stream/spool_buffer.hpp"
#include "psx/transport/session.hpp"

#include <algorithm>
#include <cstdint>
#include <span>
#include <utility>

namespace psx::transport {

struct NativeController::Conn : SessionHandler {
    NativeController* owner = nullptr;
    std::size_t index = 0;
    Target target;
    Session* session = nullptr; // bound after the transport is built (for consume/open)
    HostResult result;
    std::string stdoutRing;
    std::string stderrRing;
    psx::stream::SpoolBuffer stdoutSpool;
    psx::stream::SpoolBuffer stderrSpool;
    bool active = false;
    bool done = false;
    std::unique_ptr<NativeTransport> transport; // declared last: destroyed first

    void onData(StreamId id, std::string_view data, bool /*endStream*/, Channel channel) override {
        if (session != nullptr) {
            session->consume(id, static_cast<std::uint32_t>(data.size())); // grant credit
        }
        if (done) {
            return;
        }
        std::string& ring = channel == Channel::Stderr ? stderrRing : stdoutRing;
        psx::stream::SpoolBuffer& spool = channel == Channel::Stderr ? stderrSpool : stdoutSpool;
        const auto& options = owner->options_;
        const std::size_t cap = options.policy == psx::stream::OverflowPolicy::Block ? 0 : options.ringBytes;
        if (cap == 0) {
            ring.append(data);
        } else if (options.policy == psx::stream::OverflowPolicy::DropNewest) {
            const std::size_t available = cap - ring.size();
            const std::size_t retained = std::min(available, data.size());
            ring.append(data.data(), retained);
            result.droppedBytes += data.size() - retained;
        } else {
            const std::size_t available = cap - ring.size();
            const std::size_t overflow = data.size() > available ? data.size() - available : 0;
            const std::size_t fromRing = std::min(overflow, ring.size());
            const std::size_t fromData = overflow - fromRing;
            if (options.policy == psx::stream::OverflowPolicy::Spool) {
                if (fromRing != 0 && !spool.append(std::string_view(ring.data(), fromRing))) {
                    result.droppedBytes += fromRing;
                }
                if (fromData != 0 && !spool.append(data.substr(0, fromData))) {
                    result.droppedBytes += fromData;
                }
            } else {
                result.droppedBytes += overflow;
            }
            ring.erase(0, fromRing);
            ring.append(data.substr(fromData));
        }
        if (owner->onOutput_) {
            owner->onOutput_(result.host, data, channel);
        }
    }
    void onExit(StreamId /*id*/, const psx::os::ExitStatus& status) override {
        if (done) {
            return;
        }
        result.ok = true;
        result.exitCode = (status.kind == psx::os::ExitStatus::Kind::Exited) ? status.code : -1;
        finish();
    }
    void fail(const std::string& message, CancelKind kind = CancelKind::Other) {
        if (!done) {
            result.error = message;
            result.ok = false;
            result.timedOut = kind == CancelKind::Timeout;
            result.cancelled = kind == CancelKind::Interrupt;
            result.aborted = kind == CancelKind::FailFast;
        }
        finish();
    }
    void finish() {
        if (done) {
            return;
        }
        result.stdoutData = stdoutSpool.empty() ? std::move(stdoutRing) : stdoutSpool.readAll() + stdoutRing;
        result.stderrData = stderrSpool.empty() ? std::move(stderrRing) : stderrSpool.readAll() + stderrRing;
        result.output = result.stdoutData + result.stderrData;
        done = true;
        owner->onConnDone(index);
    }
};

NativeController::NativeController(psx::runtime::Reactor& reactor,
                                   psx::os::TlsConfig controllerConfig,
                                   OnOutput onOutput)
    : reactor_(reactor), config_(std::move(controllerConfig)), onOutput_(std::move(onOutput)) {
    config_.isServer = false;
}

NativeController::~NativeController() {
    for (const auto timer : retireTimers_) {
        reactor_.cancel(timer);
    }
    if (snapshotTimer_ != 0) {
        reactor_.cancel(snapshotTimer_); // the callback captures `this`
    }
}

psx::Result<void> NativeController::start(const std::vector<Target>& targets,
                                          const std::vector<std::string>& command,
                                          std::function<void(std::vector<HostResult>)> onComplete) {
    return start(targets, command, std::move(onComplete), Options{});
}

psx::Result<void> NativeController::start(const std::vector<Target>& targets,
                                          const std::vector<std::string>& command,
                                          std::function<void(std::vector<HostResult>)> onComplete,
                                          Options options) {
    if (started_) {
        return psx::Error{psx::ErrorClass::InvalidArgument, 0, "native_controller.start.already_started"};
    }
    if (command.empty() || command.front().empty()) {
        return psx::Error{psx::ErrorClass::InvalidArgument, 0, "native_controller.start.command"};
    }
    for (const auto& target : targets) {
        if (target.host.empty() || target.port == 0) {
            return psx::Error{psx::ErrorClass::InvalidArgument, 0, "native_controller.start.target"};
        }
    }
    switch (options.policy) {
        case psx::stream::OverflowPolicy::Block:
        case psx::stream::OverflowPolicy::DropNewest:
        case psx::stream::OverflowPolicy::DropOldest:
        case psx::stream::OverflowPolicy::Spool:
            break;
        default:
            return psx::Error{psx::ErrorClass::InvalidArgument, 0, "native_controller.start.overflow_policy"};
    }
    started_ = true;
    command_ = command;
    onComplete_ = std::move(onComplete);
    options_ = options;
    nextToStart_ = 0;
    active_ = 0;
    remaining_ = targets.size();
    filling_ = false;
    cancelling_ = false;
    completed_ = false;
    conns_.reserve(targets.size());

    if (targets.empty()) {
        completed_ = true;
        if (onComplete_) {
            onComplete_({});
        }
        return {};
    }

    for (std::size_t i = 0; i < targets.size(); ++i) {
        auto conn = std::make_unique<Conn>();
        conn->owner = this;
        conn->index = i;
        conn->target = targets[i];
        conn->result.host = targets[i].host;
        conns_.push_back(std::move(conn));
    }
    fillSlots();
    return {};
}

void NativeController::fillSlots() {
    if (completed_ || cancelling_ || filling_) {
        return;
    }
    filling_ = true;
    const std::size_t limit = options_.concurrency == 0 ? conns_.size() : options_.concurrency;
    const std::size_t prevStarted = nextToStart_;
    while (nextToStart_ < conns_.size() && active_ < limit && !cancelling_) {
        launch(nextToStart_++);
    }
    filling_ = false;
    if (nextToStart_ > prevStarted) {
        takeSnapshot();
    }
}

void NativeController::launch(std::size_t index) {
    Conn* raw = conns_[index].get();
    const Target& target = raw->target;
    raw->active = true;
    ++active_;

    auto socket = psx::os::Socket::connect(target.host, target.port);
    if (!socket.ok()) {
        raw->fail("connect: " + socket.error().message());
        return;
    }
    auto tls = psx::os::Tls::create(config_);
    if (!tls.ok()) {
        raw->fail("tls: " + tls.error().message());
        return;
    }

    std::function<bool(std::string_view)> authorize;
    if (!target.expectedSan.empty()) {
        authorize = [san = target.expectedSan](std::string_view value) { return value == san; };
    }
    raw->transport = std::make_unique<NativeTransport>(
        reactor_, std::move(socket.value()), std::move(tls.value()), Role::Controller, *raw,
        NativeTransport::Callbacks{.authorize = std::move(authorize),
                                   .onReady =
                                       [this, raw] {
                                           if (!raw->done && raw->session != nullptr) {
                                               // Lamport tick: dispatching this stage is a local
                                               // event, ordered relative to every other OPEN this
                                               // controller sends and whatever the receiving node
                                               // observes off it. See
                                               // docs/ds-project/01-lamport-clocks.md.
                                               raw->result.lamportTs = clock_.tick();
                                               raw->session->open(OpenRequest{
                                                   .argv = command_, .cwd = {}, .lamportTs = raw->result.lamportTs});
                                           }
                                       },
                                   .onError = [raw](const psx::Error& error) { raw->fail(error.message()); }},
        kDefaultStreamWindow, kDefaultLease);
    raw->session = &raw->transport->session();
    if (auto started = raw->transport->start(); !started.ok()) {
        raw->fail("start: " + started.error().message());
    }
}

void NativeController::cancel(const std::string& reason, CancelKind kind) {
    if (completed_) {
        return;
    }
    if (kind == CancelKind::Other) {
        if (reason == "timed out") {
            kind = CancelKind::Timeout;
        } else if (reason == "fail-fast") {
            kind = CancelKind::FailFast;
        } else if (reason == "cancelled" || reason == "interrupted") {
            kind = CancelKind::Interrupt;
        }
    }
    cancelling_ = true;
    for (auto& conn : conns_) {
        if (!conn->done) {
            conn->fail(reason, kind);
        }
    }
    completeIfReady();
}

void NativeController::onConnDone(std::size_t index) {
    if (remaining_ > 0) {
        --remaining_;
    }
    Conn& conn = *conns_[index];
    if (options_.failFast && !cancelling_ &&
        (!conn.result.ok || conn.result.exitCode != 0 || !conn.result.error.empty())) {
        cancel("fail-fast", CancelKind::FailFast);
    }

    if (conn.active) {
        // A NativeTransport can report completion from one of its own callbacks.
        // Retire it on the next reactor turn so its callback stack has unwound.
        retireTimers_.push_back(reactor_.after(std::chrono::milliseconds(0), [this, index] { retire(index); }));
    }
    takeSnapshot();
    completeIfReady();
}

void NativeController::retire(std::size_t index) {
    Conn& conn = *conns_[index];
    if (conn.active) {
        conn.session = nullptr;
        conn.transport.reset();
        conn.active = false;
        if (active_ > 0) {
            --active_;
        }
    }
    if (!cancelling_) {
        fillSlots();
    }
    completeIfReady();
}

void NativeController::completeIfReady() {
    if (remaining_ != 0 || active_ != 0 || completed_) {
        return;
    }
    flushSnapshot(); // final, consistent state of every target
    completed_ = true;
    std::vector<HostResult> results;
    results.reserve(conns_.size());
    for (auto& conn : conns_) {
        results.push_back(std::move(conn->result));
    }
    if (onComplete_) {
        auto callback = std::move(onComplete_);
        callback(std::move(results)); // may destroy this controller
    }
}

psx::runtime::ClusterSnapshot NativeController::captureSnapshot() const {
    const std::string runId = options_.runId.empty() ? "pipeshellx-run" : options_.runId;
    psx::runtime::ClusterSnapshot snapshot(runId);
    for (const auto& conn : conns_) {
        psx::runtime::NodeSnapshot node;
        node.host = conn->target.host;
        node.stageId = "stage-" + std::to_string(conn->index);
        if (conn->done) {
            if (conn->result.ok) {
                node.status = "exited";
            } else if (conn->result.timedOut) {
                node.status = "timed_out";
            } else if (conn->result.cancelled) {
                node.status = "cancelled";
            } else {
                node.status = "lost";
            }
        } else if (conn->active) {
            node.status = "running";
        } else {
            node.status = "connecting";
        }
        node.exitCode = conn->result.exitCode;
        // The timestamp of this node's dispatch event, or 0 if it was never
        // dispatched. Never substitute the controller's current clock: that
        // would stamp an event that did not happen with a logical time.
        node.lamportTs = conn->result.lamportTs;
        snapshot.record(std::move(node));
    }
    return snapshot;
}

void NativeController::takeSnapshot() {
    if (options_.snapshotPath.empty() || completed_) {
        return;
    }
    // Coalesce. Every snapshot records every target, and launches/completions
    // happen once per target, so writing on every event is O(N^2) bytes of
    // synchronous disk I/O on the reactor thread (~100 MB for 1000 hosts).
    // Instead, mark dirty and write at most once per kSnapshotInterval; the
    // final state is always written by flushSnapshot() from completeIfReady().
    if (snapshotTimer_ != 0) {
        return; // a write is already scheduled and will see this change
    }
    snapshotTimer_ = reactor_.after(kSnapshotInterval, [this] {
        snapshotTimer_ = 0;
        writeSnapshot();
    });
}

void NativeController::flushSnapshot() {
    if (snapshotTimer_ != 0) {
        reactor_.cancel(snapshotTimer_);
        snapshotTimer_ = 0;
    }
    if (!options_.snapshotPath.empty()) {
        writeSnapshot();
    }
}

void NativeController::writeSnapshot() {
    // An unwritable path degrades to "no snapshots", like the audit log; the
    // run itself must never fail because observability could not be written.
    (void)captureSnapshot().appendToFile(options_.snapshotPath);
}

} // namespace psx::transport
