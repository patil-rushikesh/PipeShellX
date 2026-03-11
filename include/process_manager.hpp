#pragma once

#include <array>
#include <string>
#include <vector>
#include <sys/types.h>
#include <signal.h>

#include "client_config.hpp"
#include "logger.hpp"
#include "ssh_auth.hpp"

class ProcessManager {
public:
    struct ClientResult {
        std::string clientId;
        int exitCode;
        std::string stdoutData;
        std::string stderrData;
        std::string errorMessage;
        bool timedOut;
    };

    struct Result {
        int exitCode;
        std::string stdoutData;
        std::string stderrData;
        bool timedOut;
        std::vector<ClientResult> clientResults;
    };

    ProcessManager();
    ~ProcessManager();

    // Non-copyable, movable
    ProcessManager(const ProcessManager&) = delete;
    ProcessManager& operator=(const ProcessManager&) = delete;
    ProcessManager(ProcessManager&&) noexcept;
    ProcessManager& operator=(ProcessManager&&) noexcept;

    // Execute command with arguments, optional input, timeout in seconds
    Result execute(const std::vector<std::string>& args,
                   const LogContext& context,
                   const std::string& input = "",
                   int timeoutSec = 0);
    Result executeRemote(const std::vector<ClientEntry>& clients,
                         const std::string& remoteCommand,
                         const LogContext& context,
                         int timeoutSec = 0);

private:
    pid_t childPid;
    std::array<int, 2> stdinPipe;
    std::array<int, 2> stdoutPipe;
    std::array<int, 2> stderrPipe;

    void setupPipes();
    void closePipes();
    void setNonBlocking(int fd);
    void reapChild(bool terminateProcessGroup) noexcept;
    static void installSigChldHandler();
    static void sigChldHandler(int signo);

    // Internal helpers
    bool readAvailableData(int fd, std::string& output, bool& closed);
    bool writeAvailableData(int fd, const std::string& input, std::size_t& written, bool& closed);
    std::string formatClientResults(const std::vector<ClientResult>& clientResults, bool useStdout) const;
    std::string classifyRemoteError(const ClientResult& clientResult) const;
};
