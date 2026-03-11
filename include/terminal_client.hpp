#pragma once

#include <string>
#include <vector>
#include <functional>
#include <mutex>
#include <optional>

#include "client_manager.hpp"
#include "process_manager.hpp"

class TerminalClient {
public:
    TerminalClient();
    ~TerminalClient();

    void run();

private:
    std::vector<std::string> history;
    bool running;
    std::mutex outputMutex;
    ClientManager clientManager;

    void printPrompt();
    void printColored(const std::string& msg, const std::string& color);
    void handleCommand(const std::string& command);
    bool handleClientCommand(const std::string& command);
    void printError(const std::string& msg);
    void printHistory();
    void printHelp();
    void printClients();
    void printStatusTable();
    void refreshClientStatuses(const std::vector<ProcessManager::ClientResult>& clientResults);
    void handleExit();
    bool promptPasswordRequired();
    std::optional<std::string> promptPasswordSecurely();

    // Color codes
    const std::string COLOR_RESET = "\033[0m";
    const std::string COLOR_GREEN = "\033[32m";
    const std::string COLOR_RED = "\033[31m";
    const std::string COLOR_YELLOW = "\033[33m";
    const std::string COLOR_BLUE = "\033[34m";
};
