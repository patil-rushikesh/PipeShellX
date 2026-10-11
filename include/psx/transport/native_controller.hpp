#pragma once

#include "psx/os/tls.hpp"
#include "psx/result.hpp"
#include "psx/runtime/cluster_snapshot.hpp"
#include "psx/runtime/lamport_clock.hpp"
#include "psx/runtime/reactor.hpp"
#include "psx/stream/bounded_buffer.hpp"
#include "psx/transport/native_transport.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace psx::transport {

// The controller side of the backplane: connects to a set of nodes over mTLS,
// runs one command on each as a stream, and collects bounded per-channel output
// plus the exit status.
// All connections share one reactor. onComplete fires once every target has
// finished or failed. Not thread-safe.
class NativeController {
public:
    struct Target {
        std::string host;
        std::uint16_t port = 0;
        std::string expectedSan; // the node identity to authorise (empty = any authenticated)
    };
    struct HostResult {
        std::string host;
        bool ok = false;    // the stage ran to an EXIT (not a connect/auth failure)
        int exitCode = -1;  // the stage's exit code (Exited) or -1
        std::string error;  // non-empty on a transport/auth failure
        std::string output; // compatibility view: captured stdout followed by stderr
        std::string stdoutData;
        std::string stderrData;
        std::uint64_t droppedBytes = 0;
        bool timedOut = false;
        bool cancelled = false;
        bool aborted = false;
        // The Lamport timestamp this controller attached to the OPEN it sent for
        // this target (0 if the stage was never dispatched, e.g. a connect
        // failure before open()). See docs/ds-project/01-lamport-clocks.md.
        std::uint64_t lamportTs = 0;
    };

    struct Options {
        // 0 means all targets may connect at once.
        std::size_t concurrency = 0;
        psx::stream::OverflowPolicy policy = psx::stream::OverflowPolicy::Block;
        // A zero ring, or Block policy, captures without a size limit. Spool
        // keeps at most ringBytes per channel in memory and spills older bytes.
        std::size_t ringBytes = 0;
        bool failFast = false;
        // Optional path to record point-in-time cluster snapshots (JSONL format).
        // See docs/ds-project/02-cluster-snapshot.md.
        std::string snapshotPath;
        // Optional identifier for this run. Defaults to "pipeshellx-run".
        std::string runId;
    };

    enum class CancelKind : std::uint8_t { Other, Timeout, Interrupt, FailFast };
    // Live output callback: (host, bytes, channel) as each DATA chunk arrives.
    using OnOutput = std::function<void(const std::string&, std::string_view, Channel)>;

    NativeController(psx::runtime::Reactor& reactor, psx::os::TlsConfig controllerConfig, OnOutput onOutput = {});
    ~NativeController();
    NativeController(const NativeController&) = delete;
    NativeController& operator=(const NativeController&) = delete;

    // Connects to every target and runs `command`. onComplete gets the per-host
    // results (in target order) once all targets are done.
    psx::Result<void> start(const std::vector<Target>& targets,
                            const std::vector<std::string>& command,
                            std::function<void(std::vector<HostResult>)> onComplete);
    psx::Result<void> start(const std::vector<Target>& targets,
                            const std::vector<std::string>& command,
                            std::function<void(std::vector<HostResult>)> onComplete,
                            Options options);

    // Fail every still-running target with `reason` (e.g. a run deadline); this
    // completes the run via onComplete like any other finish.
    void cancel(const std::string& reason, CancelKind kind = CancelKind::Other);

    // Captures the current point-in-time state of all tracked nodes (simplified Chandy-Lamport).
    psx::runtime::ClusterSnapshot captureSnapshot() const;

private:
    struct Conn;
    void fillSlots();
    void launch(std::size_t index);
    void onConnDone(std::size_t index);
    void retire(std::size_t index);
    void completeIfReady();
    void takeSnapshot();  // schedule a coalesced snapshot write
    void flushSnapshot(); // write the current state now (final snapshot)
    void writeSnapshot();
    static constexpr std::chrono::milliseconds kSnapshotInterval{250};

    psx::runtime::Reactor& reactor_;
    psx::os::TlsConfig config_;
    OnOutput onOutput_;
    std::vector<std::string> command_;
    std::vector<std::unique_ptr<Conn>> conns_;
    std::function<void(std::vector<HostResult>)> onComplete_;
    Options options_;
    std::size_t nextToStart_ = 0;
    std::size_t active_ = 0;
    std::size_t remaining_ = 0;
    // One Lamport clock per controller run: ticked once per OPEN sent (see
    // docs/ds-project/01-lamport-clocks.md). A single shared clock, not one
    // per target, because all targets are dispatched by the same logical
    // controller process.
    psx::runtime::LamportClock clock_;
    bool filling_ = false;
    bool cancelling_ = false;
    bool completed_ = false;
    bool started_ = false;
    std::vector<psx::runtime::TimerId> retireTimers_;
    psx::runtime::TimerId snapshotTimer_ = 0;
};

} // namespace psx::transport
