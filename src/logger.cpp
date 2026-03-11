
#include "logger.hpp"
#include <unistd.h>
#include <chrono>
#include <iomanip>
#include <iostream>
#include <sstream>

Logger::Logger() : currentLevel(LogLevel::INFO) {}

Logger::~Logger() {
    if (logFile.is_open()) logFile.close();
}

Logger& Logger::getInstance() {
    static Logger instance;
    return instance;
}

void Logger::setLogFile(const std::string& filename) {
    std::lock_guard<std::mutex> lock(logMutex);
    if (logFile.is_open()) logFile.close();
    logFile.open(filename, std::ios::app);
    if (!logFile.is_open()) {
        std::cerr << "[Logger] Failed to open log file: " << filename << std::endl;
    }
}

std::string Logger::getTimestamp() const {
    auto now = std::chrono::system_clock::now();
    auto t = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;
    std::tm tm;
#if defined(_WIN32)
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm);
    std::ostringstream oss;
    oss << buf << "." << std::setfill('0') << std::setw(3) << static_cast<int>(ms.count());
    return oss.str();
}

std::string Logger::levelToString(LogLevel level) const {
    switch (level) {
        case LogLevel::INFO:  return "INFO";
        case LogLevel::DEBUG: return "DEBUG";
        case LogLevel::ERROR: return "ERROR";
        default:              return "UNKNOWN";
    }
}

void Logger::log(LogLevel level, const std::string& msg) {
    log(level, LogContext{getpid(), "-", "-"}, msg);
}

void Logger::log(LogLevel level, const LogContext& context, const std::string& msg) {
    std::lock_guard<std::mutex> lock(logMutex);
    std::ostringstream formatted;
    formatted << "[" << getTimestamp() << "] "
              << "[" << levelToString(level) << "] "
              << "[pid=" << context.pid << "] "
              << "[session=" << (context.sessionId.empty() ? "-" : context.sessionId) << "] "
              << "[command=" << (context.command.empty() ? "-" : context.command) << "] "
              << msg;
    const std::string logMsg = formatted.str();
    if (logFile.is_open()) {
        logFile << logMsg << std::endl;
    } else {
        std::cout << logMsg << std::endl;
    }
}
