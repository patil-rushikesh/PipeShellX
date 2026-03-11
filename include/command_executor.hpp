#pragma once

#include <string>
#include <vector>
#include <functional>

#include "client_config.hpp"
#include "logger.hpp"

struct CommandResult {
    int exitCode;
    std::string stdoutData;
    std::string stderrData;
    bool timedOut;
};

class CommandExecutor {
public:
    using OutputCallback = std::function<void(const std::string&, bool isStdout)>;

    CommandExecutor();
    ~CommandExecutor();

    // Non-copyable, movable
    CommandExecutor(const CommandExecutor&) = delete;
    CommandExecutor& operator=(const CommandExecutor&) = delete;
    CommandExecutor(CommandExecutor&&) noexcept;
    CommandExecutor& operator=(CommandExecutor&&) noexcept;

    // Execute command with streaming output
    CommandResult execute(const std::string& command,
                          const std::string& sessionId = "-",
                          OutputCallback streamCallback = nullptr,
                          int timeoutSec = 0);

private:
    void validateCommand(const std::vector<std::string>& args);
    std::vector<std::string> parseCommand(const std::string& command);
    std::string resolveExecutablePath(const std::string& commandName) const;
    bool isAllowedArgument(const std::string& argument) const;
    std::string buildRemoteCommand(const std::vector<std::string>& args) const;
    std::vector<ClientEntry> loadConfiguredClients() const;

    // Internal helpers
    CommandResult runCommand(const std::vector<std::string>& args,
                             const LogContext& context,
                             OutputCallback streamCallback,
                             int timeoutSec);
};
