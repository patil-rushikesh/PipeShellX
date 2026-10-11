#include "psx/transport/node_stage_runner.hpp"

#include "psx/os/io.hpp"
#include "psx/os/pipe.hpp"

#include <array>
#include <span>
#include <utility>

namespace psx::transport {

using psx::os::ExitStatus;
using psx::os::Interest;
using psx::os::Pipe;
using psx::os::Readiness;
using psx::os::SpawnSpec;

NodeStageRunner::NodeStageRunner(psx::runtime::Reactor& reactor, CommandValidator validator)
    : reactor_(reactor), validator_(std::move(validator)) {}

NodeStageRunner::NodeStageRunner(psx::runtime::Reactor& reactor,
                                 CommandValidator validator,
                                 psx::runtime::LamportClock& sharedClock)
    : reactor_(reactor), validator_(std::move(validator)), clock_(&sharedClock) {}

NodeStageRunner::~NodeStageRunner() {
    // Fencing: the controller connection dropping (peer close, kill -9, or a
    // torn-down NodeServer connection) destroys this runner while stages may
    // still be running. Kill each running stage's process group explicitly so
    // it is never orphaned -- rather than leaning on ~Process's implicit
    // teardown, so the guarantee survives changes to how the Process is held.
    for (auto& [id, stage] : stages_) {
        if (stage.stdoutToken != 0) {
            (void)reactor_.unwatch(stage.stdoutToken);
        }
        if (stage.stderrToken != 0) {
            (void)reactor_.unwatch(stage.stderrToken);
        }
        if (stage.stdinToken != 0) {
            (void)reactor_.unwatch(stage.stdinToken);
        }
        if (!stage.exited) {
            (void)stage.process.signal(psx::os::StopSignal::Kill);
            (void)reactor_.unwatchChild(stage.process.id());
        }
    }
}

void NodeStageRunner::onOpen(StreamId id, const OpenRequest& request) {
    // Lamport clock: adopt the controller's timestamp when it sent one (OPEN
    // v2), otherwise this is still a local event worth ordering, so tick
    // regardless. See docs/ds-project/01-lamport-clocks.md.
    if (request.lamportTs != 0) {
        clock_->observe(request.lamportTs);
    } else {
        clock_->tick();
    }

    if (request.argv.empty()) {
        session_->sendExit(id, {ExitStatus::Kind::Exited, 127});
        return;
    }
    if (validator_) {
        if (const auto rejection = validator_(request)) {
            const std::string diagnostic = "pipeshellx node: command rejected by policy: " + *rejection + "\n";
            session_->sendData(id, diagnostic, /*endStream=*/false, Channel::Stderr);
            session_->sendExit(id, {ExitStatus::Kind::Exited, 126});
            return;
        }
    }

    auto stdoutPipe = Pipe::create();
    auto stderrPipe = Pipe::create();
    auto stdinPipe = Pipe::create();
    if (!stdoutPipe.ok() || !stderrPipe.ok() || !stdinPipe.ok()) {
        session_->sendExit(id, {ExitStatus::Kind::Exited, 127});
        return;
    }
    // We write stdin without watching until the pipe backs up, so it must be
    // non-blocking or a full pipe would stall the reactor thread.
    (void)stdinPipe.value().writer.setNonBlocking(true);

    SpawnSpec spec;
    spec.program = request.argv.front(); // PATH lookup when it has no '/'
    spec.argv = request.argv;
    spec.cwd = request.cwd;
    spec.in = SpawnSpec::Stdio::from(stdinPipe.value().reader);
    spec.out = SpawnSpec::Stdio::from(stdoutPipe.value().writer);
    spec.err = SpawnSpec::Stdio::from(stderrPipe.value().writer);

    auto process = psx::os::Process::spawn(spec);
    if (!process.ok()) {
        session_->sendExit(id, {ExitStatus::Kind::Exited, 127}); // could not start
        return;
    }

    Stage stage;
    stage.process = std::move(process.value());
    stage.stdoutReader = std::move(stdoutPipe.value().reader);
    stage.stderrReader = std::move(stderrPipe.value().reader);
    stage.stdinWriter = std::move(stdinPipe.value().writer);
    auto [it, inserted] = stages_.emplace(id, std::move(stage));
    // The pipe write ends close here (only the child holds them now).
    arm(id, it->second);
}

void NodeStageRunner::arm(StreamId id, Stage& stage) {
    auto watchOut = reactor_.watch(stage.stdoutReader, Interest::Readable,
                                   [this, id](Readiness) { onReadable(id, /*isStdout=*/true); });
    if (watchOut.ok()) {
        stage.stdoutToken = watchOut.value();
    }
    auto watchErr = reactor_.watch(stage.stderrReader, Interest::Readable,
                                   [this, id](Readiness) { onReadable(id, /*isStdout=*/false); });
    if (watchErr.ok()) {
        stage.stderrToken = watchErr.value();
    }
    auto watchStdin = reactor_.watch(stage.stdinWriter, Interest::None, [this, id](Readiness) { onStdinWritable(id); });
    if (watchStdin.ok()) {
        stage.stdinToken = watchStdin.value();
    }
    if (auto watched = reactor_.watchChild(stage.process.id(), [this, id](psx::os::ProcessId) { onStageExit(id); });
        !watched.ok()) {
        // The child may have exited before we watched it (fast commands); the exit
        // source then reports NoSuchProcess. Treat it as already-exited.
        if (watched.error().cls == psx::ErrorClass::NoSuchProcess) {
            onStageExit(id);
        }
    }
}

void NodeStageRunner::onReadable(StreamId id, bool isStdout) {
    auto it = stages_.find(id);
    if (it == stages_.end()) {
        return;
    }
    Stage& stage = it->second;
    psx::os::Handle& reader = isStdout ? stage.stdoutReader : stage.stderrReader;

    std::array<char, 16 * 1024> buffer{};
    while (true) { // edge-triggered: drain to WouldBlock
        if (!session_->streamWritable(id)) {
            // The peer isn't granting credit fast enough; stop pulling so the
            // stage's own write(2) blocks (kernel backpressure). Resume on
            // WINDOW_UPDATE via resumeReads().
            setReadInterest(stage, false);
            stage.paused = true;
            return;
        }
        auto got = psx::os::read(reader, std::span<char>(buffer.data(), buffer.size()));
        if (got.ok()) {
            if (got.value() == 0) {
                closeReader(isStdout, stage);
                break;
            }
            session_->sendData(id, std::string_view(buffer.data(), got.value()), /*endStream=*/false,
                               isStdout ? Channel::Stdout : Channel::Stderr);
            continue;
        }
        if (got.error().cls == psx::ErrorClass::WouldBlock) {
            break;
        }
        closeReader(isStdout, stage); // EBADF/EIO: nothing more will arrive
        break;
    }
    finishIfDone(id);
}

void NodeStageRunner::closeReader(bool isStdout, Stage& stage) {
    psx::runtime::Token& token = isStdout ? stage.stdoutToken : stage.stderrToken;
    bool& open = isStdout ? stage.stdoutOpen : stage.stderrOpen;
    if (!open) {
        return;
    }
    if (token != 0) {
        (void)reactor_.unwatch(token);
        token = 0;
    }
    (isStdout ? stage.stdoutReader : stage.stderrReader).close();
    open = false;
}
void NodeStageRunner::onData(StreamId id, std::string_view bytes, bool endStream, Channel /*channel*/) {
    auto it = stages_.find(id);
    if (it == stages_.end()) {
        return;
    }
    Stage& stage = it->second;
    if (!bytes.empty()) {
        stage.stdinBuffer.append(bytes);
    }
    if (endStream) {
        stage.stdinEndPending = true;
    }
    drainStdin(id, stage);
}
void NodeStageRunner::drainStdin(StreamId id, Stage& stage) {
    while (stage.stdinOpen && !stage.stdinBuffer.empty()) {
        auto wrote = psx::os::write(stage.stdinWriter,
                                    std::span<const char>(stage.stdinBuffer.data(), stage.stdinBuffer.size()));
        if (wrote.ok() && wrote.value() > 0) {
            session_->consume(id, static_cast<std::uint32_t>(wrote.value())); // credit as it drains
            stage.stdinBuffer.erase(0, wrote.value());
        } else if (wrote.ok() || wrote.error().cls == psx::ErrorClass::WouldBlock) {
            break; // pipe full: wait for it to drain
        } else {
            // The stage closed its stdin (exited/EPIPE): drop the rest, free credit.
            session_->consume(id, static_cast<std::uint32_t>(stage.stdinBuffer.size()));
            stage.stdinBuffer.clear();
            closeStdin(stage);
            return;
        }
    }
    if (stage.stdinBuffer.empty()) {
        if (stage.stdinEndPending) {
            closeStdin(stage); // EOF to the stage
        } else if (stage.stdinToken != 0) {
            (void)reactor_.modify(stage.stdinToken, Interest::None);
        }
    } else if (stage.stdinToken != 0) {
        (void)reactor_.modify(stage.stdinToken, Interest::Writable); // resume when there is space
    }
}
void NodeStageRunner::onStdinWritable(StreamId id) {
    auto it = stages_.find(id);
    if (it != stages_.end()) {
        drainStdin(id, it->second);
    }
}
void NodeStageRunner::closeStdin(Stage& stage) {
    if (!stage.stdinOpen) {
        return;
    }
    if (stage.stdinToken != 0) {
        (void)reactor_.unwatch(stage.stdinToken);
        stage.stdinToken = 0;
    }
    stage.stdinWriter.close();
    stage.stdinOpen = false;
}

void NodeStageRunner::onStageExit(StreamId id) {
    auto it = stages_.find(id);
    if (it == stages_.end()) {
        return;
    }
    Stage& stage = it->second;
    if (auto status = stage.process.tryWait(); status.ok() && status.value().has_value()) {
        stage.status = *status.value();
    } else if (auto blocking = stage.process.wait(); blocking.ok()) {
        stage.status = blocking.value();
    }
    stage.exited = true;
    finishIfDone(id);
}

void NodeStageRunner::setReadInterest(Stage& stage, bool enabled) {
    const psx::os::Interest interest = enabled ? psx::os::Interest::Readable : psx::os::Interest::None;
    if (stage.stdoutOpen && stage.stdoutToken != 0) {
        (void)reactor_.modify(stage.stdoutToken, interest);
    }
    if (stage.stderrOpen && stage.stderrToken != 0) {
        (void)reactor_.modify(stage.stderrToken, interest);
    }
}

void NodeStageRunner::resumeReads(StreamId id) {
    auto it = stages_.find(id);
    if (it == stages_.end() || !it->second.paused) {
        return;
    }
    it->second.paused = false;
    setReadInterest(it->second, true);
    // Edge-triggered: bytes may already be buffered in the pipe, so drain now
    // rather than waiting for the next readable edge.
    onReadable(id, /*isStdout=*/true);
    onReadable(id, /*isStdout=*/false);
}

void NodeStageRunner::finishIfDone(StreamId id) {
    auto it = stages_.find(id);
    if (it == stages_.end()) {
        return;
    }
    const Stage& stage = it->second;
    if (stage.exited && !stage.stdoutOpen && !stage.stderrOpen) {
        session_->sendExit(id, stage.status);
        stages_.erase(it);
    }
}

} // namespace psx::transport
