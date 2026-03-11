#include "terminal_client.hpp"
#include "command_executor.hpp"
#include "logger.hpp"
#include <unistd.h>
#include <chrono>
#include <iostream>
#include <thread>
#include <atomic>

TerminalClient::TerminalClient() : running(true) {}

TerminalClient::~TerminalClient() {}

void TerminalClient::printPrompt() {
    printColored("cmd> ", COLOR_GREEN);
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

void TerminalClient::handleExit() {
    printColored("\nExiting remote shell. Goodbye!\n", COLOR_YELLOW);
    running = false;
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
    history.push_back(command);
    const std::string sessionId = "interactive-" + std::to_string(getpid());

    std::atomic<bool> done(false);
    CommandExecutor executor;
    auto streamCallback = [&](const std::string& line, bool isStdout) {
        if (isStdout)
            printColored(line + "\n", COLOR_RESET);
        else
            printColored(line + "\n", COLOR_RED);
    };

    std::thread execThread([&]() {
        try {
            auto result = executor.execute(command, sessionId, streamCallback, 0);
            if (result.exitCode != 0) {
                printError("Command failed with exit code " + std::to_string(result.exitCode));
            }
            done = true;
        } catch (const std::exception& ex) {
            Logger::getInstance().log(
                LogLevel::ERROR,
                LogContext{getpid(), sessionId, "-", command},
                std::string("Interactive command failed: ") + ex.what()
            );
            printError(ex.what());
            done = true;
        }
    });

    // Wait for command to finish, but allow prompt to be responsive
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
        if (command.empty()) continue;
        handleCommand(command);
    }
}
