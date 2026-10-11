#include "cli_options.hpp"
#include "logger.hpp"
#include "psx/cli/audit_command.hpp"
#include "psx/cli/ca_command.hpp"
#include "psx/cli/diff_command.hpp"
#include "psx/cli/hosts_command.hpp"
#include "psx/cli/node_command.hpp"
#include "psx/cli/ping_command.hpp"
#include "psx/cli/pipe_command.hpp"
#include "psx/cli/run_command.hpp"
#include "psx/cli/sim_command.hpp"
#include "psx/cli/snapshot_command.hpp"
#include "psx/os/console.hpp"
#include "psx/os/io.hpp"
#include "psx/os/system.hpp"
#include "terminal_client.hpp"

#include <exception>
#include <iostream>
#include <string>
#include <vector>

namespace {

void initLogging(const CliOptions& options) {
    // A child that exits before reading our stdin must not kill the controller
    // with SIGPIPE; write() then returns BrokenPipe instead.
    (void)psx::os::ignoreBrokenPipeSignal();
    (void)psx::os::raiseHandleLimit();

    auto& logger = Logger::getInstance();
    logger.setLevel(options.verbose ? LogLevel::DEBUG : LogLevel::INFO);
    logger.setConsoleMirror(options.verbose);
    logger.setRotation(10ULL * 1024 * 1024, 5); // 10 MiB per file, keep 5 generations
    const std::string logFile = options.logFile.empty() ? Logger::defaultLogFilePath() : options.logFile;
    // Arbitrary log paths are an intentional CLI capability. Ambient HOME/XDG
    // defaults reach this sink only with matching real/effective IDs.
    // codeql[cpp/path-injection]
    if (!logger.setLogFile(logFile)) {
        std::cerr << "PipeShellX: warning: cannot open " << logFile << "; logging to stderr instead\n";
    }
}

int runShell(const CliOptions& options) {
    initLogging(options);
    Logger::getInstance().log(LogLevel::INFO, "Starting " + cliVersionText() + " terminal client");
    TerminalClient client;
    client.run();
    return 0;
}

const char* kTopUsage = R"(Usage: pipeshellx <command> [options]

Commands:
  run    [-i FILE] [-g GROUP|-t TAG|-H h1,h2] [--stream|--group|--json]
         [--consensus [--json]] [--ordered] [-c N] [--timeout S]
         [--overflow P] [--ring SIZE] [--policy FILE] [--reuse]
         [--retries N --idempotent] [--fail-fast] [--shell S] [--audit-log FILE]
         [--transport ssh|native] [--cert F --key F --ca F [--native-port P]
          [--crl F] [--canary N|N%]] [--no-color]       (credentials: native only)
         -- <command...>                              run a command on hosts
  ping   [-i FILE] [-g GROUP|-t TAG|-H h1,h2] [--timeout S]   probe SSH hosts
  diff   [--json] [-i F] [-g G|-t T|-H h1,h2] --cert F --key F --ca F
         [--native-port P] -- <command...>             detect native-host drift
  pipe   [--check] [--file FILE | "'cmd'@place | ..."]
         [-i F --cert F --key F --ca F] [--native-port P]   run/validate a pipeline
  hosts  [list] [-i FILE]                              list inventory hosts
         add HOST [-g GROUP] [host options] -i FILE    add a host atomically
         remove HOST -i FILE | import clients.txt -i FILE   mutate/import inventory
  ca     init --cn NAME --dir DIR | issue --san URI --ca DIR --out PFX  (native transport)
         | revoke --ca DIR (--cert F|--serial HEX) | sign --ca DIR --csr F --san URI --out F
  node   --cert F --key F --ca F --listen HOST:PORT [--allow SANs] [--crl F]
         [--policy F] [--control PATH]                 run the native node daemon
  node   keygen --san URI --out PFX                    generate a node key + CSR (enroll)
  node   systemd-unit|launchd-plist --cert F --key F --ca F --listen H:P
         [--allow SANs] [--crl F] [--policy F] [--control PATH]   emit service definition
  node   status --control PATH                         query a running node daemon
  snapshot [dump] FILE [--latest] [--json]             inspect cluster snapshot records
  sim    election|mutex [--nodes N] [--seed S] ...     simulate Bully / Ricart-Agrawala
  audit  verify FILE [--expect-head HASH]              verify an --audit-log hash chain
  shell  [--verbose] [--log-file PATH]                 interactive REPL (default)

  --version   print the version and exit
  --help      show this help and exit
)";

} // namespace

int main(int argc, char** argv) {
    const std::vector<std::string> args(argv + 1, argv + argc);
    try {
        if (!args.empty() && (args[0] == "--help" || args[0] == "-h")) {
            std::cout << kTopUsage;
            return 0;
        }
        if (!args.empty() && args[0] == "--version") {
            std::cout << cliVersionText() << '\n';
            return 0;
        }

        if (!args.empty() && args[0] == "run") {
            initLogging(CliOptions{});
            try {
                const auto invocation = psx::cli::parseRun(std::vector<std::string>(args.begin() + 1, args.end()));
                return psx::cli::runSubcommand(invocation, std::cout, std::cerr,
                                               psx::os::isInteractive(psx::os::StandardStream::Output));
            } catch (const psx::cli::CliError& ex) {
                std::cerr << "pipeshellx run: " << ex.what() << "\n";
                return kExitUsage;
            }
        }

        if (!args.empty() && args[0] == "ping") {
            initLogging(CliOptions{});
            try {
                const auto invocation = psx::cli::parsePing(std::vector<std::string>(args.begin() + 1, args.end()));
                return psx::cli::pingSubcommand(invocation, std::cout, std::cerr);
            } catch (const psx::cli::CliError& ex) {
                std::cerr << "pipeshellx ping: " << ex.what() << "\n";
                return kExitUsage;
            }
        }

        if (!args.empty() && args[0] == "node") {
            std::vector<std::string> rest(args.begin() + 1, args.end());
#if defined(PIPESHELLX_HAVE_TLS)
            initLogging(CliOptions{});
            return psx::cli::nodeSubcommand(rest, std::cout, std::cerr);
#else
            std::cerr << "pipeshellx node: this build has no native transport support (OpenSSL)\n";
            return kExitUsage;
#endif
        }

        if (!args.empty() && args[0] == "ca") {
            std::vector<std::string> rest(args.begin() + 1, args.end());
#if defined(PIPESHELLX_HAVE_TLS)
            return psx::cli::caSubcommand(rest, std::cout, std::cerr);
#else
            std::cerr << "pipeshellx ca: this build has no native transport support (OpenSSL)\n";
            return kExitUsage;
#endif
        }

        if (!args.empty() && args[0] == "diff") {
            std::vector<std::string> rest(args.begin() + 1, args.end());
#if defined(PIPESHELLX_HAVE_TLS)
            initLogging(CliOptions{});
#endif
            return psx::cli::diffSubcommand(rest, std::cout, std::cerr);
        }

        if (!args.empty() && args[0] == "pipe") {
            std::vector<std::string> rest(args.begin() + 1, args.end());
            return psx::cli::pipeSubcommand(rest, std::cout, std::cerr);
        }

        if (!args.empty() && args[0] == "hosts") {
            std::vector<std::string> rest(args.begin() + 1, args.end());
            initLogging(CliOptions{});
            return psx::cli::hostsSubcommand(rest, std::cout, std::cerr);
        }

        if (!args.empty() && args[0] == "audit") {
            std::vector<std::string> rest(args.begin() + 1, args.end());
            return psx::cli::runAuditCommand(rest, std::cout, std::cerr);
        }

        if (!args.empty() && args[0] == "sim") {
            std::vector<std::string> rest(args.begin() + 1, args.end());
            return psx::cli::runSimCommand(rest, std::cout, std::cerr);
        }

        if (!args.empty() && args[0] == "snapshot") {
            std::vector<std::string> rest(args.begin() + 1, args.end());
            return psx::cli::runSnapshotCommand(rest, std::cout, std::cerr);
        }

        // `shell` or no subcommand: the interactive REPL, with its own flags.
        std::vector<std::string> shellArgs = args;
        if (!shellArgs.empty() && shellArgs[0] == "shell") {
            shellArgs.erase(shellArgs.begin());
        }
        CliOptions options;
        try {
            options = parseCliOptions(shellArgs);
        } catch (const CliParseError& ex) {
            std::cerr << "PipeShellX: " << ex.what() << "\n\n" << kTopUsage;
            return kExitUsage;
        }
        if (options.showHelp) {
            std::cout << cliUsageText();
            return 0;
        }
        if (options.showVersion) {
            std::cout << cliVersionText() << '\n';
            return 0;
        }
        return runShell(options);
    } catch (const std::exception& ex) {
        std::cerr << "Fatal error: " << ex.what() << '\n';
    } catch (...) {
        std::cerr << "Fatal error: unknown exception\n";
    }
    return 1;
}
