#include "command_executor.hpp"

#include <filesystem>
#include <sstream>
#include <array>
#include <cctype>
#include <stdexcept>
#include <string_view>
#include <unordered_set>
#include <unistd.h>

namespace {

constexpr std::size_t kMaxCommandLength = 1024;
constexpr std::size_t kMaxArgumentLength = 256;

const std::unordered_set<std::string_view>& allowedCommands() {
    static const std::unordered_set<std::string_view> commands = {
        "ls", "cat", "echo", "pwd", "whoami", "date", "uptime", "df", "du", "ps", "top", "id", "hostname"
    };
    return commands;
}

const std::array<std::string_view, 2>& trustedExecutableDirs() {
    static const std::array<std::string_view, 2> dirs = {"/bin", "/usr/bin"};
    return dirs;
}

bool isVisibleAscii(char c) {
    return c >= 0x20 && c <= 0x7e;
}

std::string shellQuote(const std::string& value) {
    std::string quoted = "'";
    for (char c : value) {
        if (c == '\'') {
            quoted += "'\\''";
        } else {
            quoted += c;
        }
    }
    quoted += "'";
    return quoted;
}

} // namespace

namespace {

std::string joinArgs(const std::vector<std::string>& args) {
    std::ostringstream joined;
    for (std::size_t index = 0; index < args.size(); ++index) {
        if (index != 0) {
            joined << ' ';
        }
        joined << args[index];
    }
    return joined.str();
}

} // namespace

CommandExecutor::CommandExecutor() {}
CommandExecutor::~CommandExecutor() {}

CommandExecutor::CommandExecutor(CommandExecutor&&) noexcept {}
CommandExecutor& CommandExecutor::operator=(CommandExecutor&&) noexcept { return *this; }

void CommandExecutor::validateCommand(const std::vector<std::string>& args) {
    if (args.empty()) {
        throw std::runtime_error("No command provided");
    }

    const std::string& commandName = args.front();
    if (commandName.empty() || commandName.size() > kMaxArgumentLength) {
        throw std::runtime_error("Invalid command name");
    }
    if (commandName.find('/') != std::string::npos) {
        throw std::runtime_error("Explicit paths are not allowed");
    }
    if (!allowedCommands().contains(commandName)) {
        throw std::runtime_error("Command is not in the allowlist");
    }
    for (char c : commandName) {
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_' && c != '-' && c != '.') {
            throw std::runtime_error("Command name contains invalid characters");
        }
    }

    for (std::size_t index = 1; index < args.size(); ++index) {
        if (!isAllowedArgument(args[index])) {
            throw std::runtime_error("Command argument contains unsupported characters");
        }
    }
}

std::vector<std::string> CommandExecutor::parseCommand(const std::string& command) {
    if (command.empty() || command.size() > kMaxCommandLength) {
        throw std::runtime_error("Command length is invalid");
    }

    std::vector<std::string> args;
    std::string current;
    bool inQuotes = false;

    for (char c : command) {
        if (c == '"') {
            inQuotes = !inQuotes;
            continue;
        }

        if (!isVisibleAscii(c) && !std::isspace(static_cast<unsigned char>(c))) {
            throw std::runtime_error("Command contains non-printable characters");
        }

        if (std::isspace(static_cast<unsigned char>(c)) && !inQuotes) {
            if (!current.empty()) {
                args.push_back(current);
                current.clear();
            }
            continue;
        }

        current.push_back(c);
    }

    if (inQuotes) {
        throw std::runtime_error("Unterminated quoted argument");
    }
    if (!current.empty()) {
        args.push_back(current);
    }

    return args;
}

std::string CommandExecutor::resolveExecutablePath(const std::string& commandName) const {
    for (const std::string_view directory : trustedExecutableDirs()) {
        const std::string candidate = std::string(directory) + "/" + commandName;
        if (access(candidate.c_str(), X_OK) == 0) {
            return candidate;
        }
    }

    throw std::runtime_error("Allowed command was not found in trusted system paths");
}

bool CommandExecutor::isAllowedArgument(const std::string& argument) const {
    if (argument.size() > kMaxArgumentLength) {
        return false;
    }

    for (char c : argument) {
        if (!isVisibleAscii(c)) {
            return false;
        }

        switch (c) {
            case ';':
            case '&':
            case '|':
            case '`':
            case '$':
            case '<':
            case '>':
            case '\\':
                return false;
            default:
                break;
        }
    }

    return true;
}

std::string CommandExecutor::buildRemoteCommand(const std::vector<std::string>& args) const {
    std::ostringstream remoteCommand;
    for (std::size_t index = 0; index < args.size(); ++index) {
        if (index != 0) {
            remoteCommand << ' ';
        }
        remoteCommand << shellQuote(args[index]);
    }
    return remoteCommand.str();
}

std::vector<ClientEntry> CommandExecutor::loadConfiguredClients() const {
    constexpr const char* kDefaultClientConfig = "clients.txt";
    if (!std::filesystem::exists(kDefaultClientConfig)) {
        return {};
    }

    ClientConfig config;
    config.loadFromFile(kDefaultClientConfig);
    return config.clients();
}

CommandResult CommandExecutor::executeRemoteCommand(const std::string& command,
                                                    const std::vector<ClientEntry>& clients,
                                                    const std::string& sessionId,
                                                    OutputCallback streamCallback,
                                                    int timeoutSec) {
    LogContext context{getpid(), sessionId, "-", command};
    Logger::getInstance().log(LogLevel::INFO, context, "Received command for execution");

    auto args = parseCommand(command);
    validateCommand(args);
    return executeRemoteCommand(args, clients, context, streamCallback, timeoutSec);
}

CommandResult CommandExecutor::executeRemoteCommand(const std::vector<std::string>& args,
                                                    const std::vector<ClientEntry>& clients,
                                                    LogContext context,
                                                    OutputCallback streamCallback,
                                                    int timeoutSec) {
    const std::string remoteCommand = buildRemoteCommand(args);
    context.command = remoteCommand;
    Logger::getInstance().log(
        LogLevel::DEBUG,
        context,
        "Validated command and loaded " + std::to_string(clients.size()) + " remote clients"
    );

    ProcessManager pm;
    Logger::getInstance().log(LogLevel::INFO, context, "Starting distributed SSH execution");
    auto result = pm.executeRemote(clients, remoteCommand, context, timeoutSec);

    if (streamCallback) {
        for (const auto& clientResult : result.clientResults) {
            const std::string header = "CLIENT " + clientResult.clientId;
            streamCallback(header, true);
            if (!clientResult.stdoutData.empty()) {
                std::size_t pos = 0;
                while (pos < clientResult.stdoutData.size()) {
                    const std::size_t end = clientResult.stdoutData.find('\n', pos);
                    if (end == std::string::npos) {
                        streamCallback(clientResult.stdoutData.substr(pos), true);
                        break;
                    }
                    streamCallback(clientResult.stdoutData.substr(pos, end - pos), true);
                    pos = end + 1;
                }
            }
            if (!clientResult.errorMessage.empty()) {
                streamCallback(clientResult.errorMessage, false);
            } else if (!clientResult.stderrData.empty()) {
                std::size_t pos = 0;
                while (pos < clientResult.stderrData.size()) {
                    const std::size_t end = clientResult.stderrData.find('\n', pos);
                    if (end == std::string::npos) {
                        streamCallback(clientResult.stderrData.substr(pos), false);
                        break;
                    }
                    streamCallback(clientResult.stderrData.substr(pos, end - pos), false);
                    pos = end + 1;
                }
            }
        }
    }

    return CommandResult{
        result.exitCode,
        std::move(result.stdoutData),
        std::move(result.stderrData),
        result.timedOut,
        std::move(result.clientResults)
    };
}

CommandResult CommandExecutor::execute(const std::string& command,
                                       const std::string& sessionId,
                                       OutputCallback streamCallback,
                                       int timeoutSec) {
    LogContext context{getpid(), sessionId, "-", command};
    Logger::getInstance().log(LogLevel::INFO, context, "Received command for execution");

    auto args = parseCommand(command);
    validateCommand(args);

    const auto clients = loadConfiguredClients();
    if (!clients.empty()) {
        return executeRemoteCommand(args, clients, context, streamCallback, timeoutSec);
    }

    args.front() = resolveExecutablePath(args.front());
    context.command = joinArgs(args);
    Logger::getInstance().log(LogLevel::DEBUG, context, "Validated command and resolved executable");
    return runCommand(args, context, streamCallback, timeoutSec);
}

CommandResult CommandExecutor::runCommand(const std::vector<std::string>& args,
                                          const LogContext& context,
                                          OutputCallback streamCallback,
                                          int timeoutSec) {
    if (args.empty()) {
        throw std::runtime_error("No command arguments provided");
    }

    ProcessManager pm;
    Logger::getInstance().log(LogLevel::INFO, context, "Starting process execution");
    auto result = pm.execute(args, context, "", timeoutSec);

    if (streamCallback) {
        auto streamLines = [&](const std::string& data, bool isStdout) {
            std::size_t pos = 0;
            while (pos < data.size()) {
                const std::size_t end = data.find('\n', pos);
                if (end == std::string::npos) {
                    streamCallback(data.substr(pos), isStdout);
                    return;
                }
                streamCallback(data.substr(pos, end - pos), isStdout);
                pos = end + 1;
            }
        };
        streamLines(result.stdoutData, true);
        streamLines(result.stderrData, false);
    }

    Logger::getInstance().log(
        result.exitCode == 0 ? LogLevel::INFO : LogLevel::ERROR,
        context,
        "Process execution finished with exit code " + std::to_string(result.exitCode) +
            (result.timedOut ? " (timed out)" : "")
    );

    return CommandResult{
        result.exitCode,
        std::move(result.stdoutData),
        std::move(result.stderrData),
        result.timedOut,
        {}
    };
}

CommandResult CommandExecutor::executeOnClients(const std::string& command,
                                                const std::vector<ClientEntry>& clients,
                                                const std::string& sessionId,
                                                OutputCallback streamCallback,
                                                int timeoutSec) {
    if (clients.empty()) {
        throw std::runtime_error("No clients selected for remote execution");
    }
    return executeRemoteCommand(command, clients, sessionId, streamCallback, timeoutSec);
}
