#pragma once

#include <string>
#include <fstream>
#include <mutex>
#include <memory>
#include <sys/types.h>

enum class LogLevel { INFO, DEBUG, ERROR };

struct LogContext {
    pid_t pid;
    std::string sessionId;
    std::string clientId;
    std::string command;
};

class Logger {
public:
    static Logger& getInstance();

    void log(LogLevel level, const std::string& msg);
    void log(LogLevel level, const LogContext& context, const std::string& msg);

    void setLogFile(const std::string& filename);

private:
    Logger();
    ~Logger();

    std::ofstream logFile;
    std::mutex logMutex;
    LogLevel currentLevel;

    std::string getTimestamp() const;
    std::string levelToString(LogLevel level) const;

    // Non-copyable, non-movable
    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;
};
