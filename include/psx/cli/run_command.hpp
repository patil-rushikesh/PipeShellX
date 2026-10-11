#pragma once

// psx::cli — parsing and execution of the `pipeshellx run` subcommand. Kept
// separate from execution so the argument grammar is unit-tested on its own.

#include "psx/stream/bounded_buffer.hpp"
#include "ssh_auth.hpp" // RemoteShell

#include <cstddef>
#include <ostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace psx::cli {

class CliError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

enum class SinkMode { Group, Stream, Json, Consensus };
enum class SelectorKind { All, Group, Tag, Hosts };

struct Selector {
    SelectorKind kind = SelectorKind::All;
    std::string value;              // group or tag name
    std::vector<std::string> hosts; // for SelectorKind::Hosts
};

struct RunInvocation {
    std::string inventoryPath; // -i / --inventory; empty = default lookup
    Selector selector;
    SinkMode sink = SinkMode::Group;
    bool consensusJson = false; // --json with --consensus
    bool ordered = false;       // --ordered: sort hosts before rendering
    int timeoutSec = 0;
    int concurrency = 64; // -c / --concurrency: workers in flight (0 = all at once)
    psx::stream::OverflowPolicy policy = psx::stream::OverflowPolicy::Block; // --policy
    std::size_t ringBytes = 0;              // --ring SIZE (0 = unbounded capture, the default)
    std::string policyPath;                 // --policy FILE: restrict the command (empty = unrestricted)
    bool reuse = false;                     // --reuse: ssh ControlMaster connection reuse
    int retries = 0;                        // --retries N: extra attempts on a transient transport failure
    bool retriesExplicit = false;           // --retries was supplied (including --retries 0)
    bool idempotent = false;                // --idempotent: allow retrying the command
    RemoteShell shell = RemoteShell::Posix; // --shell: remote command quoting (posix/cmd/powershell)
    bool shellExplicit = false;             // --shell was supplied (including --shell posix)
    bool failFast = false;                  // --fail-fast: abort the run on the first final failure
    std::string auditPath;                  // --audit-log FILE: append a JSONL audit trail (empty = off)
    std::string snapshotPath;               // --snapshot-file FILE: record cluster snapshots (empty = off)
    bool native = false;                    // --transport native: use the psx/1 mTLS backplane instead of ssh
    bool transportExplicit = false;         // --transport was supplied; overrides per-host inventory transport
    std::string certPath;                   // --cert: controller certificate (native)
    bool certExplicit = false;
    std::string keyPath; // --key: controller private key (native)
    bool keyExplicit = false;
    std::string caPath; // --ca: trusted CA (native)
    bool caExplicit = false;
    std::string crlPath; // --crl: optional CRL to reject revoked node certs (native)
    bool crlExplicit = false;
    int nativePort = 7433; // --native-port: the node listener port (native)
    bool nativePortExplicit = false;
    std::string canary; // --canary N|N%: roll out to this subset first (native)
    bool canaryExplicit = false;
    bool colour = true;               // --no-color turns it off
    std::vector<std::string> command; // the argv after `--`
};

// Parses the args following `run`. Throws CliError on any grammar violation.
RunInvocation parseRun(const std::vector<std::string>& args);

// Loads the inventory, selects hosts, runs `command` on each over SSH, renders
// through the chosen sink, and returns the process exit code (0 all succeeded,
// 1 some stage failed, 2 usage/config, 3 no hosts selected). `colourTty` is
// whether stdout is a terminal (the effective colour is colour && colourTty).
// The number of hosts in a canary subset: `spec` is a count ("3") or a
// percentage ("5%", rounded up); clamped to [1, total] (0 when total is 0).
std::size_t canaryCount(const std::string& spec, std::size_t total);

int runSubcommand(const RunInvocation& invocation, std::ostream& out, std::ostream& err, bool colourTty);

} // namespace psx::cli
