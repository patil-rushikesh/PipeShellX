#include "terminal_client.hpp"

#include "command_executor.hpp"
#include "logger.hpp"

#include <unistd.h>

#include <atomic>
#include <chrono>
#include <iostream>
#include <optional>
#include <sstream>
#include <thread>

namespace {

std::string trim(const std::string& value) {
    const auto begin = value.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) {
        return "";
    }
    const auto end = value.find_last_not_of(" \t\r\n");
    return value.substr(begin, end - begin + 1);
}

bool startsWith(const std::string& text, const std::string& prefix) {
    return text.rfind(prefix, 0) == 0;
}

std::string stripPrefix(const std::string& text, const std::string& prefix) {
    return trim(text.substr(prefix.size()));
}

std::string formatTerminalError(const std::string& message) {
    if (message.rfind("ERROR:", 0) == 0) {
        return message;
    }
    if (message.find("fork() failed") != std::string::npos) {
        return "ERROR: failed to create worker process: " + message;
    }
    if (message.find("pipe creation failed") != std::string::npos ||
        message.find("read from pipe failed") != std::string::npos ||
        message.find("write to child stdin failed") != std::string::npos ||
        message.find("poll failed") != std::string::npos ||
        message.find("invalid pipe state") != std::string::npos ||
        message.find("fcntl(") != std::string::npos) {
        return "ERROR: IPC pipe failure: " + message;
    }
    if (message.find("execvp failed") != std::string::npos) {
        return "ERROR: command execution failed: " + message;
    }
    return "ERROR: " + message;
}

} // namespace

TerminalClient::TerminalClient()
    : running(true), clientManager("clients.txt") {
    clientManager.load();
}

TerminalClient::~TerminalClient() {}

void TerminalClient::printPrompt() {
    printColored("PipeShell > ", COLOR_GREEN);
}

void TerminalClient::printColored(const std::string& msg, const std::string& color) {
    std::lock_guard<std::mutex> lock(outputMutex);
    std::cout << color << msg << COLOR_RESET;
    std::cout.flush();
}

void TerminalClient::printError(const std::string& msg) {
    printColored("[ERROR] ", COLOR_RED);
    std::cout << msg << std::endl;
}

void TerminalClient::printHistory() {
    printColored("Command History:\n", COLOR_BLUE);
    for (size_t i = 0; i < history.size(); ++i) {
        std::cout << i + 1 << ": " << history[i] << std::endl;
    }
}

void TerminalClient::printHelp() {
    printColored("Commands:\n", COLOR_BLUE);
    std::cout
        << "  add-client <ssh-url|user@host>\n"
        << "  remove-client <client-id>\n"
        << "  list-clients\n"
        << "  status\n"
        << "  run <command>\n"
        << "  run-one <client-id> <command>\n"
        << "  history\n"
        << "  help\n"
        << "  exit\n";
}

void TerminalClient::printClients() {
    printColored("Configured Clients:\n", COLOR_BLUE);
    if (clientManager.empty()) {
        std::cout << "  (none)\n";
        return;
    }

    for (const auto& client : clientManager.clients()) {
        std::cout << "  " << client.client.id << ": " << client.client.ssh_url
                  << " [" << ClientManager::statusToString(client.status) << "]";
        if (!client.lastError.empty()) {
            std::cout << " - " << client.lastError;
        }
        std::cout << '\n';
    }
}

void TerminalClient::printStatusTable() {
    printColored("ID HOST STATUS\n", COLOR_BLUE);
    if (clientManager.empty()) {
        return;
    }

    for (const auto& client : clientManager.clients()) {
        std::cout << client.client.id << ' '
                  << client.entry.clientId() << ' '
                  << ClientManager::statusToString(client.status) << '\n';
    }
}

void TerminalClient::refreshClientStatuses(const std::vector<ProcessManager::ClientResult>& clientResults) {
    for (const auto& result : clientResults) {
        if (result.exitCode == 0 && result.errorMessage.empty() && !result.timedOut) {
            clientManager.updateClientStatus(result.clientId, ClientStatus::ONLINE);
        } else {
            const std::string error = result.errorMessage.empty()
                ? "ERROR: command failed with exit code " + std::to_string(result.exitCode)
                : result.errorMessage;
            clientManager.updateClientStatus(result.clientId, ClientStatus::OFFLINE, error);
        }
    }
}

void TerminalClient::handleExit() {
    printColored("\nExiting PipeShellX. Goodbye!\n", COLOR_YELLOW);
    running = false;
}

bool TerminalClient::handleClientCommand(const std::string& command) {
    if (command == "help") {
        printHelp();
        return true;
    }
    if (command == "list-clients") {
        printClients();
        return true;
    }
    if (startsWith(command, "add-client ")) {
        const std::string specification = stripPrefix(command, "add-client ");
        if (specification.empty()) {
            printError("Usage: add-client <ssh-url|user@host>");
            return true;
        }
        clientManager.addClient(specification);
        printColored("Client added.\n", COLOR_BLUE);
        return true;
    }
    if (startsWith(command, "remove-client ")) {
        const std::string identifier = stripPrefix(command, "remove-client ");
        if (identifier.empty()) {
            printError("Usage: remove-client <client-id>");
            return true;
        }
        bool removed = false;
        try {
            const int id = std::stoi(identifier);
            removed = clientManager.removeClient(id);
        } catch (...) {
            removed = clientManager.removeClient(identifier);
        }
        if (!removed) {
            printError("Unknown client: " + identifier);
            return true;
        }
        printColored("Client removed.\n", COLOR_BLUE);
        return true;
    }
    if (command == "status") {
        const auto clients = clientManager.selectClients(std::nullopt);
        if (clients.empty()) {
            printStatusTable();
            return true;
        }
        CommandExecutor executor;
        const std::string sessionId = "interactive-" + std::to_string(getpid());
        auto result = executor.executeOnClients("echo alive", clients, sessionId, nullptr, 10);
        refreshClientStatuses(result.clientResults);
        printStatusTable();
        return true;
    }
    if (startsWith(command, "run-one ")) {
        const std::string rest = stripPrefix(command, "run-one ");
        const std::size_t separator = rest.find(' ');
        if (separator == std::string::npos) {
            printError("Usage: run-one <client-id> <command>");
            return true;
        }
        const std::string identifier = trim(rest.substr(0, separator));
        const std::string remoteCommand = trim(rest.substr(separator + 1));
        if (identifier.empty() || remoteCommand.empty()) {
            printError("Usage: run-one <client-id> <command>");
            return true;
        }

        const auto clients = clientManager.selectClients(std::make_optional(identifier));
        const std::string sessionId = "interactive-" + std::to_string(getpid());
        std::atomic<bool> done(false);
        CommandExecutor executor;
        auto streamCallback = [&](const std::string& line, bool isStdout) {
            printColored(line + "\n", isStdout ? COLOR_RESET : COLOR_RED);
        };

        std::thread execThread([&, clients, remoteCommand, sessionId]() {
            try {
                auto result = executor.executeOnClients(remoteCommand, clients, sessionId, streamCallback, 0);
                refreshClientStatuses(result.clientResults);
                if (result.exitCode != 0) {
                    printError("Command failed with exit code " + std::to_string(result.exitCode));
                }
            } catch (const std::exception& ex) {
                const std::string userMessage = formatTerminalError(ex.what());
                Logger::getInstance().log(
                    LogLevel::ERROR,
                    LogContext{getpid(), sessionId, identifier, remoteCommand},
                    std::string("Targeted client execution failed: ") + ex.what()
                );
                printError(userMessage);
            }
            done = true;
        });

        while (!done) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        execThread.join();
        return true;
    }
    if (startsWith(command, "run ")) {
        const std::string remoteCommand = stripPrefix(command, "run ");
        if (remoteCommand.empty()) {
            printError("Usage: run <command>");
            return true;
        }
        const auto clients = clientManager.selectClients(std::nullopt);
        if (clients.empty()) {
            printError("No configured clients");
            return true;
        }

        const std::string sessionId = "interactive-" + std::to_string(getpid());
        std::atomic<bool> done(false);
        CommandExecutor executor;
        auto streamCallback = [&](const std::string& line, bool isStdout) {
            printColored(line + "\n", isStdout ? COLOR_RESET : COLOR_RED);
        };

        std::thread execThread([&, clients, remoteCommand, sessionId]() {
            try {
                auto result = executor.executeOnClients(remoteCommand, clients, sessionId, streamCallback, 0);
                refreshClientStatuses(result.clientResults);
                if (result.exitCode != 0) {
                    printError("Command failed with exit code " + std::to_string(result.exitCode));
                }
            } catch (const std::exception& ex) {
                const std::string userMessage = formatTerminalError(ex.what());
                Logger::getInstance().log(
                    LogLevel::ERROR,
                    LogContext{getpid(), sessionId, "-", remoteCommand},
                    std::string("Broadcast client execution failed: ") + ex.what()
                );
                printError(userMessage);
            }
            done = true;
        });

        while (!done) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        execThread.join();
        return true;
    }

    return false;
}

void TerminalClient::handleCommand(const std::string& command) {
    if (command == "exit" || command == "quit") {
        handleExit();
        return;
    }
    if (command == "history") {
        printHistory();
        return;
    }
    if (handleClientCommand(command)) {
        return;
    }

    history.push_back(command);
    const std::string sessionId = "interactive-" + std::to_string(getpid());

    std::atomic<bool> done(false);
    CommandExecutor executor;
    auto streamCallback = [&](const std::string& line, bool isStdout) {
        printColored(line + "\n", isStdout ? COLOR_RESET : COLOR_RED);
    };

    std::thread execThread([&]() {
        try {
            auto result = executor.execute(command, sessionId, streamCallback, 0);
            if (!result.clientResults.empty()) {
                refreshClientStatuses(result.clientResults);
            }
            if (result.exitCode != 0) {
                printError("Command failed with exit code " + std::to_string(result.exitCode));
            }
        } catch (const std::exception& ex) {
            const std::string userMessage = formatTerminalError(ex.what());
            Logger::getInstance().log(
                LogLevel::ERROR,
                LogContext{getpid(), sessionId, "-", command},
                std::string("Interactive command failed: ") + ex.what()
            );
            printError(userMessage);
        }
        done = true;
    });

    while (!done) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    execThread.join();
}

void TerminalClient::run() {
    std::string command;
    while (running) {
        printPrompt();
        std::getline(std::cin, command);
        if (std::cin.eof()) {
            handleExit();
            break;
        }
        if (trim(command).empty()) {
            continue;
        }
        try {
            handleCommand(trim(command));
        } catch (const std::exception& ex) {
            printError(formatTerminalError(ex.what()));
        }
    }
}
