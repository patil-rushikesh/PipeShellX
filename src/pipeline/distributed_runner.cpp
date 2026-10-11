#include "psx/pipeline/distributed_runner.hpp"

#include "psx/transport/session.hpp"

#include <utility>

namespace psx::pipeline {

using psx::os::ExitStatus;
using psx::transport::Channel;
using psx::transport::NativeTransport;
using psx::transport::Role;
using psx::transport::Session;
using psx::transport::StreamId;

namespace {
constexpr int kFencedExitCode = 137; // node connection teardown uses SIGKILL

int toExitCode(const ExitStatus& status) {
    return status.kind == ExitStatus::Kind::Exited ? status.code : 128 + status.code;
}
} // namespace

struct DistributedRunner::Conn : psx::transport::SessionHandler {
    DistributedRunner* owner = nullptr;
    std::size_t index = 0;
    Session* session = nullptr;
    StreamId streamId = 0;
    bool exited = false;
    int exitCode = 0;
    std::uint64_t lamportTs = 0;
    std::unique_ptr<NativeTransport> transport; // declared last: destroyed first

    void onData(StreamId /*id*/, std::string_view data, bool /*endStream*/, Channel channel) override {
        if (session != nullptr) {
            session->consume(streamId, static_cast<std::uint32_t>(data.size())); // first cut: grant immediately
        }
        if (channel == Channel::Stderr) {
            if (owner->onStderr_) {
                owner->onStderr_(data);
            }
            return;
        }
        owner->forward(index, data);
    }
    void onExit(StreamId /*id*/, const ExitStatus& status) override {
        exitCode = toExitCode(status);
        exited = true;
        owner->onStageExit(index);
    }
};

DistributedRunner::DistributedRunner(psx::runtime::Reactor& reactor,
                                     psx::os::TlsConfig controllerConfig,
                                     OnOutput onOutput,
                                     OnOutput onStderr)
    : reactor_(reactor), config_(std::move(controllerConfig)), onOutput_(std::move(onOutput)),
      onStderr_(std::move(onStderr)) {
    config_.isServer = false;
}

DistributedRunner::~DistributedRunner() = default;

psx::Result<void> DistributedRunner::run(const std::vector<RemoteStage>& stages,
                                         std::function<void(Outcome)> onComplete,
                                         bool externalStdin) {
    if (stages.empty()) {
        return psx::Error{psx::ErrorClass::InvalidArgument, 0, "empty pipeline"};
    }
    onComplete_ = std::move(onComplete);
    externalStdin_ = externalStdin;
    conns_.reserve(stages.size());
    argvs_.reserve(stages.size());
    for (const RemoteStage& stage : stages) {
        argvs_.push_back(stage.argv);
    }

    for (std::size_t i = 0; i < stages.size(); ++i) {
        const RemoteStage& stage = stages[i];
        auto conn = std::make_unique<Conn>();
        conn->owner = this;
        conn->index = i;
        Conn* raw = conn.get();
        conns_.push_back(std::move(conn));

        auto socket = psx::os::Socket::connect(stage.host, stage.port);
        if (!socket.ok()) {
            onConnError(i, "connect: " + socket.error().message());
            return {};
        }
        auto tls = psx::os::Tls::create(config_);
        if (!tls.ok()) {
            onConnError(i, "tls: " + tls.error().message());
            return {};
        }
        std::function<bool(std::string_view)> authorize;
        if (!stage.expectedSan.empty()) {
            authorize = [san = stage.expectedSan](std::string_view s) { return std::string(s) == san; };
        }
        raw->transport = std::make_unique<NativeTransport>(
            reactor_, std::move(socket.value()), std::move(tls.value()), Role::Controller, *raw,
            NativeTransport::Callbacks{.authorize = std::move(authorize),
                                       .onReady = [this, i] { onConnReady(i); },
                                       .onError = [this, i](const psx::Error& e) { onConnError(i, e.message()); }},
            psx::transport::kDefaultStreamWindow, psx::transport::kDefaultLease);
        raw->session = &raw->transport->session();
        if (auto started = raw->transport->start(); !started.ok()) {
            onConnError(i, "start: " + started.error().message());
            return {};
        }
    }
    return {};
}

void DistributedRunner::onConnReady(std::size_t index) {
    if (done_) {
        return;
    }
    conns_[index]->exited = false;
    ++readyCount_;
    if (readyCount_ != conns_.size()) {
        return; // wait until every node is up before any stage starts producing
    }
    // All connections are secured: open every stream so routing always has a
    // live downstream (all OPEN frames precede any DATA on the reactor).
    // Tick the Lamport clock for each stage dispatch to maintain happens-before order.
    for (std::size_t j = 0; j < conns_.size(); ++j) {
        conns_[j]->lamportTs = clock_.tick();
        conns_[j]->streamId =
            conns_[j]->session->open({.argv = argvs_[j], .cwd = {}, .lamportTs = conns_[j]->lamportTs});
    }
    streamsOpen_ = true;
    Conn& first = *conns_.front();
    if (!externalStdin_) {
        // No upstream, so close the first stage's stdin immediately -- a stage
        // that reads stdin (cat, grep) would otherwise block waiting for input.
        first.session->sendData(first.streamId, {}, /*endStream=*/true);
    } else {
        // Flush stdin fed before the streams were open.
        if (!stdinBuffer_.empty()) {
            first.session->sendData(first.streamId, stdinBuffer_, /*endStream=*/false);
            stdinBuffer_.clear();
        }
        if (stdinEndPending_) {
            first.session->sendData(first.streamId, {}, /*endStream=*/true);
        }
    }
}

void DistributedRunner::writeStdin(std::string_view bytes) {
    if (!externalStdin_ || done_) {
        return;
    }
    if (streamsOpen_) {
        Conn& first = *conns_.front();
        first.session->sendData(first.streamId, bytes, /*endStream=*/false);
    } else {
        stdinBuffer_.append(bytes); // buffered until the streams open
    }
}
void DistributedRunner::closeStdin() {
    if (!externalStdin_ || done_) {
        return;
    }
    stdinEndPending_ = true;
    if (streamsOpen_) {
        Conn& first = *conns_.front();
        first.session->sendData(first.streamId, {}, /*endStream=*/true);
    }
}
void DistributedRunner::cancel() {
    if (done_) {
        return;
    }
    fenceBefore(conns_.size());
    finish(outcome()); // may destroy this; touch nothing afterwards
}
void DistributedRunner::forward(std::size_t index, std::string_view data) {
    if (done_) {
        return;
    }
    if (index + 1 < conns_.size()) {
        Conn& next = *conns_[index + 1];
        next.session->sendData(next.streamId, data, /*endStream=*/false);
    } else if (onOutput_) {
        onOutput_(data); // the final stage's stdout is the pipeline output
    }
}

void DistributedRunner::onStageExit(std::size_t index) {
    if (done_) {
        return;
    }
    // Once a stage exits, no predecessor can contribute any more useful input
    // to it. Fence every predecessor that has not produced an EXIT rather than
    // continuing to consume and silently discard an infinite producer.
    fenceBefore(index);
    if (index + 1 < conns_.size()) {
        Conn& next = *conns_[index + 1];
        next.session->sendData(next.streamId, {}, /*endStream=*/true); // EOF to downstream stdin
        return;
    }
    finish(outcome());
}

void DistributedRunner::fenceBefore(std::size_t index) {
    for (std::size_t i = 0; i < index; ++i) {
        Conn& conn = *conns_[i];
        if (conn.exited) {
            continue;
        }
        conn.exited = true;
        conn.exitCode = kFencedExitCode;
        conn.session = nullptr;
        conn.transport.reset(); // closes the connection; the node kills/reaps its stage
    }
}

DistributedRunner::Outcome DistributedRunner::outcome() const {
    Outcome result;
    result.stageExitCodes.reserve(conns_.size());
    result.stageLamportTimestamps.reserve(conns_.size());
    for (const auto& conn : conns_) {
        const int code = conn->exitCode;
        result.stageExitCodes.push_back(code);
        result.stageLamportTimestamps.push_back(conn->lamportTs);
        if (code != 0) {
            result.exitCode = code; // pipefail: rightmost non-zero
        }
    }
    return result;
}

void DistributedRunner::onConnError(std::size_t index, const std::string& message) {
    if (done_) {
        return;
    }
    Outcome outcome;
    outcome.error = "stage " + std::to_string(index) + ": " + message;
    outcome.exitCode = 1;
    finish(std::move(outcome));
}

void DistributedRunner::finish(Outcome outcome) {
    if (done_) {
        return;
    }
    done_ = true;
    if (onComplete_) {
        auto callback = std::move(onComplete_);
        callback(std::move(outcome)); // may destroy `this`; touch nothing after
    }
}

} // namespace psx::pipeline
