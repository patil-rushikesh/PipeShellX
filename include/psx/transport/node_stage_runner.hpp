#pragma once

#include "psx/os/handle.hpp"
#include "psx/os/process.hpp"
#include "psx/runtime/lamport_clock.hpp"
#include "psx/runtime/reactor.hpp"
#include "psx/transport/session.hpp"

#include <functional>
#include <optional>
#include <string>
#include <unordered_map>

namespace psx::transport {

// The node side of the backplane: a SessionHandler that runs each opened stream
// as a local process, streaming its stdout/stderr back as DATA frames and an
// EXIT frame when it finishes. Stage stdout/stderr are read on the reactor.
// Not thread-safe (single reactor thread).
//
// When the controller withholds stream credit, stage pipe reads are suspended;
// kernel pipe backpressure then blocks the producer until WINDOW_UPDATE makes
// the stream writable again.
class NodeStageRunner : public SessionHandler {
public:
    // A daemon-side authorization hook. Returning a reason rejects the OPEN
    // before any pipes or process are created.
    using CommandValidator = std::function<std::optional<std::string>(const OpenRequest&)>;

    // The Session this runner sends stage output/exit on must be bound() before
    // any stream is opened (it is the Session that dispatches onOpen to us).
    explicit NodeStageRunner(psx::runtime::Reactor& reactor, CommandValidator validator = {});
    // As above, but advances `sharedClock` instead of a private one. A Lamport
    // clock belongs to a *process*: NodeServer runs one runner per controller
    // connection, so it passes the same clock to all of them; otherwise events
    // from two controllers on one node would never be ordered against each
    // other. `sharedClock` must outlive this runner (single reactor thread).
    NodeStageRunner(psx::runtime::Reactor& reactor,
                    CommandValidator validator,
                    psx::runtime::LamportClock& sharedClock);
    void bind(Session& session) {
        session_ = &session;
        session_->onStreamWritable([this](StreamId id) { resumeReads(id); });
    }
    ~NodeStageRunner() override;
    NodeStageRunner(const NodeStageRunner&) = delete;
    NodeStageRunner& operator=(const NodeStageRunner&) = delete;

    void onOpen(StreamId id, const OpenRequest& request) override;
    // Controller-sent stdin for a stage: written to the process, with credit
    // granted only as it drains (backpressure). endStream closes the stage stdin.
    void onData(StreamId id, std::string_view bytes, bool endStream, Channel channel) override;

    std::size_t runningStages() const noexcept { return stages_.size(); }

    // This node's current Lamport clock value (see
    // docs/ds-project/01-lamport-clocks.md). Advances on every OPEN received
    // from a v2-speaking controller (observe()) or, for a v1 controller with
    // no timestamp, on a plain local tick() so the node's own event ordering
    // still advances.
    std::uint64_t lamportClockValue() const noexcept { return clock_->value(); }

private:
    struct Stage {
        psx::os::Process process;
        psx::os::Handle stdoutReader;
        psx::os::Handle stderrReader;
        psx::os::Handle stdinWriter; // controller -> stage stdin (non-blocking)
        std::string stdinBuffer;     // stdin awaiting the stage (pipe full)
        psx::runtime::Token stdoutToken = 0;
        psx::runtime::Token stderrToken = 0;
        psx::runtime::Token stdinToken = 0;
        bool stdoutOpen = true;
        bool stderrOpen = true;
        bool stdinOpen = true;
        bool stdinEndPending = false; // endStream seen: close stdin once drained
        bool exited = false;
        bool paused = false; // reads suspended for backpressure (send window full)
        psx::os::ExitStatus status{};
    };

    void arm(StreamId id, Stage& stage);
    void onReadable(StreamId id, bool isStdout);
    void onStageExit(StreamId id);
    void closeReader(bool isStdout, Stage& stage);
    void drainStdin(StreamId id, Stage& stage);
    void onStdinWritable(StreamId id);
    void closeStdin(Stage& stage);
    void setReadInterest(Stage& stage, bool enabled);
    void resumeReads(StreamId id);
    void finishIfDone(StreamId id);

    psx::runtime::Reactor& reactor_;
    CommandValidator validator_;
    Session* session_ = nullptr;
    std::unordered_map<StreamId, Stage> stages_;
    psx::runtime::LamportClock ownClock_;            // used when no shared clock is given
    psx::runtime::LamportClock* clock_ = &ownClock_; // never null
};

} // namespace psx::transport
