#include "terminal_client.hpp"
#include "logger.hpp"

#include <exception>
#include <iostream>

int main() {
    try {
        Logger::getInstance().log(LogLevel::INFO, "Starting PipeShellX terminal client");
        TerminalClient client;
        client.run();
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "Fatal error: " << ex.what() << '\n';
    } catch (...) {
        std::cerr << "Fatal error: unknown exception\n";
    }

    return 1;
}
