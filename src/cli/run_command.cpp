#include "psx/cli/run_command.hpp"

#include "cli_options.hpp"
#include "client_config.hpp"
#include "logger.hpp"
#include "process_manager.hpp"
#include "psx/audit/audit_log.hpp"
#include "psx/cli/selection.hpp"
#include "psx/os/paths.hpp"
#include "psx/os/system.hpp"
#include "psx/policy/policy.hpp"
#include "psx/runtime/ids.hpp"
#include "psx/sink/consensus_sink.hpp"
#include "psx/sink/group_sink.hpp"
#include "psx/sink/json_sink.hpp"
#include "psx/sink/ordered_sink.hpp"
#include "psx/sink/stream_sink.hpp"

#if defined(PIPESHELLX_HAVE_TLS)
#include "psx/os/tls.hpp"
#include "psx/runtime/reactor.hpp"
#include "psx/stream/line_framer.hpp"
#include "psx/transport/native_controller.hpp"

#include <algorithm>
#include <chrono>
#include <span>
#include <unordered_map>
#endif
#include "ssh_auth.hpp"

#include <charconv>
#include <filesystem>
#include <limits>
#include <memory>
#include <system_error>

namespace psx::cli {

namespace {

std::vector<std::string> splitCsv(const std::string& value) {
    std::vector<std::string> out;
    std::size_t start = 0;
    while (start <= value.size()) {
        const std::size_t comma = value.find(',', start);
        const std::string item = value.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
        if (!item.empty()) {
            out.push_back(item);
        }
        if (comma == std::string::npos) {
            break;
        }
        start = comma + 1;
    }
    return out;
}

int parseIntArg(const std::string& value, const char* flag) {
    int result = 0;
    const auto* end = value.data() + value.size();
    const auto [ptr, ec] = std::from_chars(value.data(), end, result);
    if (ec != std::errc{} || ptr != end || result < 0) {
        throw CliError(std::string(flag) + " expects a non-negative integer, got '" + value + "'");
    }
    return result;
}

void setSelector(Selector& selector, SelectorKind kind, const std::string& raw) {
    if (selector.kind != SelectorKind::All) {
        throw CliError("selectors -g/-t/-H are mutually exclusive");
    }
    selector.kind = kind;
    if (kind == SelectorKind::Hosts) {
        selector.hosts = splitCsv(raw);
    } else {
        selector.value = raw;
    }
}

psx::stream::OverflowPolicy parsePolicy(const std::string& value) {
    if (value == "block") {
        return psx::stream::OverflowPolicy::Block;
    }
    if (value == "drop-oldest") {
        return psx::stream::OverflowPolicy::DropOldest;
    }
    if (value == "drop-newest") {
        return psx::stream::OverflowPolicy::DropNewest;
    }
    if (value == "spool") {
        return psx::stream::OverflowPolicy::Spool;
    }
    throw CliError("--overflow must be block, drop-oldest, drop-newest or spool, got '" + value + "'");
}

RemoteShell parseShell(const std::string& value) {
    if (value == "posix") {
        return RemoteShell::Posix;
    }
    if (value == "cmd") {
        return RemoteShell::Cmd;
    }
    if (value == "powershell" || value == "pwsh") {
        return RemoteShell::PowerShell;
    }
    throw CliError("--shell must be posix, cmd or powershell, got '" + value + "'");
}

// Parses a byte size: a decimal count with an optional K/M/G or KiB/MiB/GiB
// (binary) suffix. "1MiB" == "1M" == 1048576.
std::size_t parseSize(const std::string& value) {
    std::size_t i = 0;
    while (i < value.size() && (value[i] >= '0' && value[i] <= '9')) {
        ++i;
    }
    if (i == 0) {
        throw CliError("--ring expects a size like 1MiB, got '" + value + "'");
    }
    std::size_t count = 0;
    const auto* digitsEnd = value.data() + i;
    const auto [ptr, ec] = std::from_chars(value.data(), digitsEnd, count);
    if (ec != std::errc{} || ptr != digitsEnd) {
        throw CliError("--ring size is out of range: '" + value + "'");
    }
    std::string suffix = value.substr(i);
    std::size_t multiplier = 1;
    if (suffix.empty() || suffix == "B") {
        multiplier = 1;
    } else if (suffix == "K" || suffix == "KiB") {
        multiplier = 1024;
    } else if (suffix == "M" || suffix == "MiB") {
        multiplier = 1024ULL * 1024;
    } else if (suffix == "G" || suffix == "GiB") {
        multiplier = 1024ULL * 1024 * 1024;
    } else {
        throw CliError("--ring: unknown size suffix '" + suffix + "'");
    }
    if (count > std::numeric_limits<std::size_t>::max() / multiplier) {
        throw CliError("--ring size is out of range: '" + value + "'");
    }
    return count * multiplier;
}

void validateCanarySpec(const std::string& value) {
    const bool percent = !value.empty() && value.back() == '%';
    const std::string_view digits(value.data(), value.size() - (percent ? 1U : 0U));
    std::uint64_t parsed = 0;
    const auto* end = digits.data() + digits.size();
    const auto [ptr, ec] = std::from_chars(digits.data(), end, parsed);
    if (digits.empty() || ec != std::errc{} || ptr != end || parsed == 0) {
        throw CliError("--canary expects a positive integer or positive integer percentage, got '" + value + "'");
    }
}

void setSink(RunInvocation& invocation, SinkMode mode, bool& sinkSet) {
    if (sinkSet) {
        throw CliError("only one primary sink mode may be given");
    }
    invocation.sink = mode;
    sinkSet = true;
}

} // namespace

std::size_t canaryCount(const std::string& spec, std::size_t total) {
    if (total == 0) {
        return 0;
    }
    const bool percent = !spec.empty() && spec.back() == '%';
    const std::string_view number(spec.data(), spec.size() - (percent ? 1U : 0U));
    std::uint64_t value = 1;
    const auto* end = number.data() + number.size();
    const auto [ptr, ec] = std::from_chars(number.data(), end, value);
    if (number.empty() || ec != std::errc{} || ptr != end || value == 0) {
        value = 1; // direct callers still get a conservative one-host fallback
    }
    std::size_t count = 0;
    if (percent) {
        if (value >= 100) {
            count = total;
        } else {
            // Overflow-safe ceil(total * value / 100), with value in [1, 99].
            count = (total / 100) * static_cast<std::size_t>(value);
            const std::size_t remainder = (total % 100) * static_cast<std::size_t>(value);
            count += remainder / 100 + (remainder % 100 == 0 ? 0 : 1);
        }
    } else {
        count = value >= total ? total : static_cast<std::size_t>(value);
    }
    count = std::clamp<std::size_t>(count, 1, total);
    return count;
}

RunInvocation parseRun(const std::vector<std::string>& args) {
    RunInvocation invocation;
    bool sinkSet = false;
    bool sawDashDash = false;

    auto valueFor = [&](std::size_t& i, const std::string& flag) -> std::string {
        if (i + 1 >= args.size()) {
            throw CliError(flag + " requires a value");
        }
        return args[++i];
    };

    std::size_t i = 0;
    for (; i < args.size(); ++i) {
        const std::string& arg = args[i];
        if (arg == "--") {
            sawDashDash = true;
            ++i;
            break;
        }
        if (arg == "-g" || arg == "--group-name") {
            setSelector(invocation.selector, SelectorKind::Group, valueFor(i, arg));
        } else if (arg == "-t" || arg == "--tag") {
            setSelector(invocation.selector, SelectorKind::Tag, valueFor(i, arg));
        } else if (arg == "-H" || arg == "--hosts") {
            setSelector(invocation.selector, SelectorKind::Hosts, valueFor(i, arg));
        } else if (arg == "-i" || arg == "--inventory") {
            invocation.inventoryPath = valueFor(i, arg);
        } else if (arg == "--timeout") {
            invocation.timeoutSec = parseIntArg(valueFor(i, arg), "--timeout");
        } else if (arg == "-c" || arg == "--concurrency") {
            invocation.concurrency = parseIntArg(valueFor(i, arg), "--concurrency");
        } else if (arg == "--overflow") {
            invocation.policy = parsePolicy(valueFor(i, arg));
        } else if (arg == "--ring") {
            invocation.ringBytes = parseSize(valueFor(i, arg));
        } else if (arg == "--policy") {
            invocation.policyPath = valueFor(i, arg);
        } else if (arg == "--stream") {
            setSink(invocation, SinkMode::Stream, sinkSet);
        } else if (arg == "--group") {
            setSink(invocation, SinkMode::Group, sinkSet);
        } else if (arg == "--json") {
            if (sinkSet && invocation.sink == SinkMode::Consensus) {
                invocation.consensusJson = true;
            } else {
                setSink(invocation, SinkMode::Json, sinkSet);
            }
        } else if (arg == "--consensus") {
            if (sinkSet && invocation.sink != SinkMode::Json) {
                throw CliError("only one primary sink mode may be given");
            }
            invocation.consensusJson = sinkSet && invocation.sink == SinkMode::Json;
            invocation.sink = SinkMode::Consensus;
            sinkSet = true;
        } else if (arg == "--ordered") {
            invocation.ordered = true;
        } else if (arg == "--reuse") {
            invocation.reuse = true;
        } else if (arg == "--retries") {
            invocation.retriesExplicit = true;
            invocation.retries = parseIntArg(valueFor(i, arg), "--retries");
        } else if (arg == "--fail-fast") {
            invocation.failFast = true;
        } else if (arg == "--idempotent") {
            invocation.idempotent = true;
        } else if (arg == "--canary") {
            invocation.canaryExplicit = true;
            invocation.canary = valueFor(i, arg);
            validateCanarySpec(invocation.canary);
        } else if (arg == "--shell") {
            invocation.shellExplicit = true;
            invocation.shell = parseShell(valueFor(i, arg));
        } else if (arg == "--audit-log") {
            invocation.auditPath = valueFor(i, arg);
        } else if (arg == "--snapshot-file") {
            invocation.snapshotPath = valueFor(i, arg);
        } else if (arg == "--transport") {
            invocation.transportExplicit = true;
            const std::string value = valueFor(i, arg);
            if (value == "native") {
                invocation.native = true;
            } else if (value == "ssh") {
                invocation.native = false;
            } else {
                throw CliError("--transport must be ssh or native, got '" + value + "'");
            }
        } else if (arg == "--cert") {
            invocation.certExplicit = true;
            invocation.certPath = valueFor(i, arg);
        } else if (arg == "--key") {
            invocation.keyExplicit = true;
            invocation.keyPath = valueFor(i, arg);
        } else if (arg == "--ca") {
            invocation.caExplicit = true;
            invocation.caPath = valueFor(i, arg);
        } else if (arg == "--crl") {
            invocation.crlExplicit = true;
            invocation.crlPath = valueFor(i, arg);
        } else if (arg == "--native-port") {
            invocation.nativePortExplicit = true;
            invocation.nativePort = parseIntArg(valueFor(i, arg), "--native-port");
        } else if (arg == "--no-color" || arg == "--no-colour") {
            invocation.colour = false;
        } else {
            throw CliError("unknown option: " + arg);
        }
    }

    if (!sawDashDash) {
        throw CliError("expected `-- <command>` (a `--` before the command to run)");
    }
    for (; i < args.size(); ++i) {
        invocation.command.push_back(args[i]);
    }
    if (invocation.command.empty()) {
        throw CliError("no command given after `--`");
    }
    if (invocation.native) {
        if (invocation.nativePort <= 0 || invocation.nativePort > 65535) {
            throw CliError("--native-port must be in the range 1..65535");
        }
        if (invocation.reuse) {
            throw CliError("--reuse is only supported with --transport ssh");
        }
        if (invocation.idempotent) {
            throw CliError("--idempotent is only supported with --transport ssh");
        }
        if (invocation.retriesExplicit) {
            throw CliError("--retries is only supported with --transport ssh");
        }
        if (invocation.shellExplicit) {
            throw CliError("--shell is only supported with --transport ssh");
        }
    } else if (invocation.transportExplicit) {
        if (invocation.canaryExplicit) {
            throw CliError("--canary is only supported with --transport native");
        }
        if (invocation.certExplicit) {
            throw CliError("--cert is only supported with --transport native");
        }
        if (invocation.keyExplicit) {
            throw CliError("--key is only supported with --transport native");
        }
        if (invocation.caExplicit) {
            throw CliError("--ca is only supported with --transport native");
        }
        if (invocation.crlExplicit) {
            throw CliError("--crl is only supported with --transport native");
        }
        if (invocation.nativePortExplicit) {
            throw CliError("--native-port is only supported with --transport native");
        }
    }
    return invocation;
}

namespace {

std::unique_ptr<psx::sink::Sink>
makeSink(const RunInvocation& invocation, std::ostream& out, std::ostream& err, bool colourTty) {
    std::unique_ptr<psx::sink::Sink> sink;
    switch (invocation.sink) {
        case SinkMode::Stream:
            sink = std::make_unique<psx::sink::StreamSink>(out, err, invocation.colour && colourTty);
            break;
        case SinkMode::Json:
            sink = std::make_unique<psx::sink::JsonSink>(out);
            break;
        case SinkMode::Consensus:
            sink = std::make_unique<psx::sink::ConsensusSink>(out, invocation.consensusJson);
            break;
        case SinkMode::Group:
        default:
            sink = std::make_unique<psx::sink::GroupSink>(out);
            break;
    }
    if (invocation.ordered) {
        sink = std::make_unique<psx::sink::OrderedSink>(std::move(sink));
    }
    return sink;
}

} // namespace

#if defined(PIPESHELLX_HAVE_TLS)
namespace {

std::optional<std::string> slurpFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return std::nullopt;
    }
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// Runs the command on each host over the psx/1 native mTLS backplane.
int runNative(const RunInvocation& invocation,
              const std::vector<ClientEntry>& clients,
              psx::sink::Sink* sink,
              std::ostream& err) {
    if (invocation.certPath.empty() || invocation.keyPath.empty() || invocation.caPath.empty()) {
        err << "pipeshellx run: --transport native requires --cert, --key and --ca\n";
        return 2;
    }
    const auto cert = slurpFile(invocation.certPath);
    const auto key = slurpFile(invocation.keyPath);
    const auto ca = slurpFile(invocation.caPath);
    if (!cert || !key || !ca) {
        err << "pipeshellx run: cannot read --cert/--key/--ca\n";
        return 2;
    }
    std::string crl;
    if (!invocation.crlPath.empty()) {
        const auto pem = slurpFile(invocation.crlPath);
        if (!pem) {
            err << "pipeshellx run: cannot read --crl " << invocation.crlPath << "\n";
            return 2;
        }
        crl = *pem;
    }

    auto reactor = psx::runtime::Reactor::create({.backend = psx::os::Poller::Backend::Auto,
                                                  .childExit = psx::os::ChildExitMode::Auto,
                                                  .signals = {psx::os::Signal::Interrupt, psx::os::Signal::Terminate}});
    if (!reactor.ok()) {
        err << "pipeshellx run: " << reactor.error().message() << "\n";
        return 2;
    }
    psx::runtime::Reactor& r = *reactor.value();

    // A line framer per host and channel feeds the sink whole lines from the raw
    // stream bytes, keeping stdout and stderr distinct as they are over SSH.
    struct HostFramers {
        psx::stream::LineFramer out;
        psx::stream::LineFramer err;
    };
    auto framers = std::make_shared<std::unordered_map<std::string, HostFramers>>();
    const bool streamLive = sink != nullptr && sink->streamsLive();
    auto emit = [sink](const std::string& host, psx::sink::Channel channel, std::string_view line) {
        if (sink != nullptr) {
            sink->line(host, channel, line);
        }
    };
    psx::transport::NativeController::OnOutput onOutput =
        [framers, emit, streamLive](const std::string& host, std::string_view bytes, psx::transport::Channel channel) {
            if (!streamLive) {
                return;
            }
            const bool err = channel == psx::transport::Channel::Stderr;
            HostFramers& hf = (*framers)[host];
            const auto sinkChannel = err ? psx::sink::Channel::Stderr : psx::sink::Channel::Stdout;
            (err ? hf.err : hf.out)
                .push(std::span<const char>(bytes.data(), bytes.size()),
                      [&](std::string_view line, bool) { emit(host, sinkChannel, line); });
        };

    std::vector<psx::transport::NativeController::Target> targets;
    targets.reserve(clients.size());
    for (const auto& client : clients) {
        targets.push_back(
            {.host = client.host,
             .port = static_cast<std::uint16_t>(client.nativePort != 0 ? client.nativePort : invocation.nativePort),
             .expectedSan = client.expectedSan});
        if (sink != nullptr) {
            sink->stageStarted(client.host);
        }
    }

    const std::string runId = psx::runtime::newRunId();
    std::unique_ptr<psx::audit::AuditLog> audit;
    if (!invocation.auditPath.empty()) {
        audit = std::make_unique<psx::audit::AuditLog>(invocation.auditPath);
        if (!audit->ok()) {
            err << "pipeshellx run: warning: cannot open audit log " << invocation.auditPath << "\n";
            audit.reset();
        } else {
            audit->runStarted(runId, quoteRemoteCommand(invocation.command, RemoteShell::Posix), clients.size());
        }
    }

    enum class StopCause { None, Timeout, Interrupt };
    StopCause stopCause = StopCause::None;
    bool finalized = false;
    int exitCode = 1;
    psx::transport::NativeController* activeController = nullptr;

    auto finishResults = [&](std::vector<psx::transport::NativeController::HostResult> results) {
        if (finalized) {
            return;
        }
        finalized = true;
        std::size_t succeeded = 0;
        std::uint64_t droppedBytes = 0;
        bool anyCancelled = stopCause == StopCause::Interrupt;
        for (std::size_t i = 0; i < results.size(); ++i) {
            auto& res = results[i];
            if (streamLive) {
                HostFramers& hf = (*framers)[res.host];
                hf.out.flush([&](std::string_view line, bool) { emit(res.host, psx::sink::Channel::Stdout, line); });
                hf.err.flush([&](std::string_view line, bool) { emit(res.host, psx::sink::Channel::Stderr, line); });
            } else if (sink != nullptr) {
                const auto replay = [&](psx::sink::Channel channel, std::string_view bytes) {
                    psx::stream::LineFramer framer;
                    const auto emitLine = [&](std::string_view line, bool) { emit(res.host, channel, line); };
                    framer.push(std::span<const char>(bytes.data(), bytes.size()), emitLine);
                    framer.flush(emitLine);
                };
                replay(psx::sink::Channel::Stdout, res.stdoutData);
                replay(psx::sink::Channel::Stderr, res.stderrData);
            }
            const bool ok =
                res.ok && res.exitCode == 0 && res.error.empty() && !res.timedOut && !res.cancelled && !res.aborted;
            succeeded += ok ? 1 : 0;
            droppedBytes += res.droppedBytes;
            anyCancelled = anyCancelled || res.cancelled;
            if (sink != nullptr) {
                sink->stageFinished(res.host, psx::sink::StageResult{.exitCode = res.exitCode,
                                                                     .timedOut = res.timedOut,
                                                                     .errorMessage = res.error,
                                                                     .droppedBytes = res.droppedBytes});
            }
            if (audit) {
                audit->stageFinished(runId, psx::audit::StageRecord{.host = res.host,
                                                                    .stageId = "s" + std::to_string(i),
                                                                    .exitCode = res.exitCode,
                                                                    .attempts = 1,
                                                                    .timedOut = res.timedOut,
                                                                    .cancelled = res.cancelled,
                                                                    .aborted = res.aborted,
                                                                    .droppedBytes = res.droppedBytes,
                                                                    .error = res.error});
            }
        }
        exitCode = anyCancelled ? kExitCancelled : (succeeded == results.size() ? 0 : 1);
        if (sink != nullptr) {
            sink->runFinished(psx::sink::RunSummary{.stages = results.size(),
                                                    .succeeded = succeeded,
                                                    .failed = results.size() - succeeded,
                                                    .droppedBytes = droppedBytes,
                                                    .cancelled = anyCancelled});
        }
        if (audit) {
            audit->runFinished(runId, psx::audit::RunRecord{.total = results.size(),
                                                            .succeeded = succeeded,
                                                            .failed = results.size() - succeeded,
                                                            .cancelled = anyCancelled,
                                                            .exitCode = exitCode});
        }
        r.stop();
    };

    auto cancellationResult = [&](const psx::transport::NativeController::Target& target, std::string message,
                                  bool aborted) {
        psx::transport::NativeController::HostResult result;
        result.host = target.host;
        result.error = std::move(message);
        result.timedOut = stopCause == StopCause::Timeout;
        result.cancelled = stopCause == StopCause::Interrupt;
        result.aborted = aborted && stopCause == StopCause::None;
        return result;
    };

    const psx::os::TlsConfig tlsConfig{
        .certificatePem = *cert, .privateKeyPem = *key, .caPem = *ca, .crlPem = crl, .isServer = false};
    const psx::transport::NativeController::Options controllerOptions{
        .concurrency = static_cast<std::size_t>(invocation.concurrency),
        .policy = invocation.policy,
        .ringBytes = invocation.ringBytes,
        .failFast = invocation.failFast,
        .snapshotPath = invocation.snapshotPath,
        .runId = runId};

    auto signalRegistration = r.onSignal([&](psx::os::Signal) {
        if (finalized || stopCause == StopCause::Interrupt) {
            return;
        }
        stopCause = StopCause::Interrupt;
        if (activeController != nullptr) {
            activeController->cancel("cancelled", psx::transport::NativeController::CancelKind::Interrupt);
        }
    });
    if (!signalRegistration.ok()) {
        err << "pipeshellx run: cannot register interrupt handling: " << signalRegistration.error().message() << "\n";
        return 2;
    }

    std::unique_ptr<psx::transport::NativeController> firstController;
    std::unique_ptr<psx::transport::NativeController> restController;
    std::vector<psx::transport::NativeController::Target> canaryTargets;
    std::vector<psx::transport::NativeController::Target> restTargets;
    if (!invocation.canary.empty()) {
        const std::size_t count = canaryCount(invocation.canary, targets.size());
        canaryTargets.assign(targets.begin(), targets.begin() + static_cast<std::ptrdiff_t>(count));
        restTargets.assign(targets.begin() + static_cast<std::ptrdiff_t>(count), targets.end());
        firstController = std::make_unique<psx::transport::NativeController>(r, tlsConfig, onOutput);
        activeController = firstController.get();
        auto started = firstController->start(
            canaryTargets, invocation.command,
            [&](std::vector<psx::transport::NativeController::HostResult> canaryResults) {
                const bool allOk = std::all_of(canaryResults.begin(), canaryResults.end(), [](const auto& result) {
                    return result.ok && result.exitCode == 0 && result.error.empty() && !result.timedOut &&
                           !result.cancelled && !result.aborted;
                });
                if (!allOk || restTargets.empty() || stopCause != StopCause::None) {
                    const std::string message =
                        stopCause == StopCause::Timeout
                            ? "timed out"
                            : (stopCause == StopCause::Interrupt ? "cancelled" : "skipped: canary failed");
                    for (const auto& target : restTargets) {
                        canaryResults.push_back(cancellationResult(target, message, true));
                    }
                    finishResults(std::move(canaryResults));
                    return;
                }
                restController = std::make_unique<psx::transport::NativeController>(r, tlsConfig, onOutput);
                activeController = restController.get();
                auto combined = std::make_shared<std::vector<psx::transport::NativeController::HostResult>>(
                    std::move(canaryResults));
                auto restStarted = restController->start(
                    restTargets, invocation.command,
                    [&, combined](std::vector<psx::transport::NativeController::HostResult> restResults) mutable {
                        combined->insert(combined->end(), std::make_move_iterator(restResults.begin()),
                                         std::make_move_iterator(restResults.end()));
                        finishResults(std::move(*combined));
                    },
                    controllerOptions);
                if (!restStarted.ok()) {
                    for (const auto& target : restTargets) {
                        combined->push_back(cancellationResult(target, restStarted.error().message(), false));
                    }
                    finishResults(std::move(*combined));
                }
            },
            controllerOptions);
        if (!started.ok()) {
            std::vector<psx::transport::NativeController::HostResult> results;
            results.reserve(targets.size());
            for (const auto& target : targets) {
                results.push_back(cancellationResult(target, started.error().message(), false));
            }
            finishResults(std::move(results));
        }
    } else {
        firstController = std::make_unique<psx::transport::NativeController>(r, tlsConfig, onOutput);
        activeController = firstController.get();
        auto started = firstController->start(
            targets, invocation.command,
            [&](std::vector<psx::transport::NativeController::HostResult> results) {
                finishResults(std::move(results));
            },
            controllerOptions);
        if (!started.ok()) {
            std::vector<psx::transport::NativeController::HostResult> results;
            results.reserve(targets.size());
            for (const auto& target : targets) {
                results.push_back(cancellationResult(target, started.error().message(), false));
            }
            finishResults(std::move(results));
        }
    }

    if (invocation.timeoutSec > 0) {
        r.after(std::chrono::seconds(invocation.timeoutSec), [&] {
            if (finalized || stopCause != StopCause::None) {
                return;
            }
            stopCause = StopCause::Timeout;
            if (activeController != nullptr) {
                activeController->cancel("timed out", psx::transport::NativeController::CancelKind::Timeout);
            }
        });
    }
    if (auto ran = r.run(); !ran.ok() && !finalized) {
        err << "pipeshellx run: " << ran.error().message() << "\n";
        return 2;
    }
    return exitCode;
}

} // namespace
#endif // PIPESHELLX_HAVE_TLS

int runSubcommand(const RunInvocation& invocation, std::ostream& out, std::ostream& err, bool colourTty) {
    if (invocation.command.empty() || invocation.command.front().empty()) {
        err << "pipeshellx run: no command given\n";
        return 2;
    }
    auto validateNativeOptions = [&]() {
        const char* unsupported = nullptr;
        if (invocation.reuse) {
            unsupported = "--reuse";
        } else if (invocation.idempotent) {
            unsupported = "--idempotent";
        } else if (invocation.retriesExplicit || invocation.retries != 0) {
            unsupported = "--retries";
        } else if (invocation.shellExplicit || invocation.shell != RemoteShell::Posix) {
            unsupported = "--shell";
        }
        if (unsupported != nullptr) {
            err << "pipeshellx run: " << unsupported << " is only supported with --transport ssh\n";
            return false;
        }
        if (invocation.nativePort <= 0 || invocation.nativePort > 65535) {
            err << "pipeshellx run: --native-port must be in the range 1..65535\n";
            return false;
        }
        return true;
    };
    auto validateSshOptions = [&]() {
        const char* unsupported = nullptr;
        if (invocation.canaryExplicit || !invocation.canary.empty()) {
            unsupported = "--canary";
        } else if (invocation.certExplicit || !invocation.certPath.empty()) {
            unsupported = "--cert";
        } else if (invocation.keyExplicit || !invocation.keyPath.empty()) {
            unsupported = "--key";
        } else if (invocation.caExplicit || !invocation.caPath.empty()) {
            unsupported = "--ca";
        } else if (invocation.crlExplicit || !invocation.crlPath.empty()) {
            unsupported = "--crl";
        } else if (invocation.nativePortExplicit) {
            unsupported = "--native-port";
        }
        if (unsupported != nullptr) {
            err << "pipeshellx run: " << unsupported << " is only supported with --transport native\n";
            return false;
        }
        return true;
    };
    // Preserve the direct RunInvocation API: native=true is itself an explicit
    // native request even when an older caller did not know transportExplicit.
    if (invocation.native && !validateNativeOptions()) {
        return 2;
    }
    if (invocation.transportExplicit && !invocation.native && !validateSshOptions()) {
        return 2;
    }

    if (!invocation.policyPath.empty()) {
        try {
            const auto policy = psx::policy::Policy::loadFromFile(invocation.policyPath);
            if (auto rejected = policy.validate(invocation.command); rejected.has_value()) {
                err << "pipeshellx run: " << *rejected << "\n";
                return 2;
            }
        } catch (const std::exception& ex) {
            err << "pipeshellx run: " << ex.what() << "\n";
            return 2;
        }
    }

    const ResolvedHosts resolved = resolveHosts(invocation.inventoryPath, invocation.selector, err);
    if (!resolved.ok()) {
        return resolved.exitCode;
    }
    const std::vector<ClientEntry>& clients = resolved.clients;

    const bool hasTransportOverride = invocation.transportExplicit || invocation.native;
    bool useNative = invocation.native;
    if (!hasTransportOverride) {
        const std::string& selectedTransport = clients.front().transport;
        for (const auto& client : clients) {
            if (client.transport != selectedTransport) {
                err << "pipeshellx run: selected inventory contains mixed ssh/native transports; "
                       "pass --transport ssh or --transport native to override\n";
                return 2;
            }
        }
        useNative = selectedTransport == "native";
    }
    if (useNative) {
        if (!invocation.native && !validateNativeOptions()) {
            return 2;
        }
    } else if (!validateSshOptions()) {
        return 2;
    }

    if (useNative) {
#if defined(PIPESHELLX_HAVE_TLS)
        auto nativeSink = makeSink(invocation, out, err, colourTty);
        return runNative(invocation, clients, nativeSink.get(), err);
#else
        err << "pipeshellx run: this build has no native transport support (OpenSSL)\n";
        return 2;
#endif
    }

    // Quote the remote command line for the target shell (POSIX by default).
    const std::string remoteCommand = quoteRemoteCommand(invocation.command, invocation.shell);

    // ControlMaster reuse: a persisted master socket per user@host:port under
    // the state dir lets repeated runs skip the TCP+KEX handshake.
    std::string controlPath;
    if (invocation.reuse) {
        const std::filesystem::path dir = std::filesystem::path(psx::os::stateDirectory("pipeshellx")) / "control";
        std::error_code ignored;
        std::filesystem::create_directories(dir, ignored);
        controlPath = (dir / "cm-%r@%h:%p").string();
    }

    const std::string runId = psx::runtime::newRunId();

    // Optional JSONL audit trail (opt-in via --audit-log). An unwritable path
    // degrades to no audit with a warning; it never aborts the run.
    std::unique_ptr<psx::audit::AuditLog> audit;
    if (!invocation.auditPath.empty()) {
        audit = std::make_unique<psx::audit::AuditLog>(invocation.auditPath);
        if (!audit->ok()) {
            err << "pipeshellx run: warning: cannot open audit log " << invocation.auditPath << "\n";
            audit.reset();
        } else {
            audit->runStarted(runId, remoteCommand, clients.size());
        }
    }

    auto sink = makeSink(invocation, out, err, colourTty);
    ProcessManager manager;
    const LogContext context{.pid = psx::os::currentProcessId(),
                             .sessionId = "run",
                             .clientId = {},
                             .command = remoteCommand,
                             .runId = runId,
                             .stageId = {}};
    const auto result = manager.executeRemote(clients, remoteCommand, context,
                                              {.timeoutSec = invocation.timeoutSec,
                                               .sink = sink.get(),
                                               .concurrency = static_cast<std::size_t>(invocation.concurrency),
                                               .policy = invocation.policy,
                                               .ringBytes = invocation.ringBytes,
                                               .controlPath = controlPath,
                                               .cancellable = true,
                                               .failFast = invocation.failFast,
                                               .maxRetries = invocation.idempotent ? invocation.retries : 0});
    const int exitCode = result.cancelled ? kExitCancelled : (result.exitCode == 0 ? 0 : 1);

    if (audit) {
        std::size_t succeeded = 0;
        for (std::size_t i = 0; i < result.clientResults.size(); ++i) {
            const auto& stage = result.clientResults[i];
            const bool ok = stage.exitCode == 0 && !stage.timedOut && !stage.cancelled && !stage.aborted &&
                            stage.errorMessage.empty();
            succeeded += ok ? 1 : 0;
            audit->stageFinished(runId, psx::audit::StageRecord{.host = stage.clientId,
                                                                .stageId = "s" + std::to_string(i),
                                                                .exitCode = stage.exitCode,
                                                                .attempts = stage.attempts,
                                                                .timedOut = stage.timedOut,
                                                                .cancelled = stage.cancelled,
                                                                .aborted = stage.aborted,
                                                                .droppedBytes = stage.droppedBytes,
                                                                .error = stage.errorMessage});
        }
        audit->runFinished(runId, psx::audit::RunRecord{.total = result.clientResults.size(),
                                                        .succeeded = succeeded,
                                                        .failed = result.clientResults.size() - succeeded,
                                                        .cancelled = result.cancelled,
                                                        .exitCode = exitCode});
    }
    return exitCode;
}

} // namespace psx::cli
