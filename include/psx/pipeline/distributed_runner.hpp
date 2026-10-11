#pragma once

#include "psx/os/tls.hpp"
#include "psx/result.hpp"
#include "psx/runtime/lamport_clock.hpp"
#include "psx/runtime/reactor.hpp"
#include "psx/transport/native_transport.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace psx::pipeline {

// One stage of a distributed pipeline: a command run on a node reachable at
// host:port, whose SAN identity is pinned when non-empty.
struct RemoteStage {
    std::vector<std::string> argv;
    std::string host;
    std::uint16_t port = 0;
    std::string expectedSan; // empty = trust the CA only
};

// Runs a linear chain of remote stages over the native backplane: connects to
// each node with mTLS, opens a stream per stage once all are up, bridges
// stage[i] stdout into stage[i+1] stdin, propagates EOF (upstream exit ->
// downstream stdin close) and exit codes, and delivers the final stage's stdout.
// stderr from every stage goes to onStderr. All on one reactor; not thread-safe.
class DistributedRunner {
public:
    struct Outcome {
        int exitCode = 0;                // pipefail: the rightmost non-zero stage, else 0
        std::vector<int> stageExitCodes; // per stage (empty on a connect/handshake failure)
        std::string error;               // non-empty: a connect/handshake failure aborted the run
        // Per-stage Lamport timestamp assigned at stage dispatch (see docs/ds-project/01-lamport-clocks.md).
        std::vector<std::uint64_t> stageLamportTimestamps;
    };
    using OnOutput = std::function<void(std::string_view)>;

    DistributedRunner(psx::runtime::Reactor& reactor,
                      psx::os::TlsConfig controllerConfig,
                      OnOutput onOutput = {},
                      OnOutput onStderr = {});
    ~DistributedRunner();
    DistributedRunner(const DistributedRunner&) = delete;
    DistributedRunner& operator=(const DistributedRunner&) = delete;

    // Connect the stages and run. Returns an error only for a synchronous setup
    // failure; a stage's non-zero exit or a per-connection failure completes via
    // onComplete. `stages` must be non-empty.
    psx::Result<void>
    run(const std::vector<RemoteStage>& stages, std::function<void(Outcome)> onComplete, bool externalStdin = false);

    // Feed the first remote stage's stdin (only when run(..., externalStdin=true)).
    // Buffered until the streams open; closeStdin() sends EOF.
    void writeStdin(std::string_view bytes);
    void closeStdin();

    // Closes every unfinished remote connection and completes with those
    // stages accounted as the node's forced-fencing status (137).
    void cancel();

    // Returns the current Lamport logical clock counter of this runner.
    std::uint64_t lamportClockValue() const noexcept { return clock_.value(); }

private:
    struct Conn;
    void onConnReady(std::size_t index);
    void onConnError(std::size_t index, const std::string& message);
    void forward(std::size_t index, std::string_view data);
    void onStageExit(std::size_t index);
    void fenceBefore(std::size_t index);
    Outcome outcome() const;
    void finish(Outcome outcome);

    psx::runtime::Reactor& reactor_;
    psx::os::TlsConfig config_;
    OnOutput onOutput_;
    OnOutput onStderr_;
    std::function<void(Outcome)> onComplete_;
    std::vector<std::unique_ptr<Conn>> conns_;
    std::vector<std::vector<std::string>> argvs_;
    std::size_t readyCount_ = 0;
    bool externalStdin_ = false;
    bool streamsOpen_ = false;
    std::string stdinBuffer_;      // stdin buffered before the streams open
    bool stdinEndPending_ = false; // closeStdin() before the streams open
    bool done_ = false;
    // Lamport clock for ordering stage dispatch events (see docs/ds-project/01-lamport-clocks.md).
    psx::runtime::LamportClock clock_;
};

} // namespace psx::pipeline
