#include "session_manager.hpp"
#include "command_executor.hpp"
#include "logger.hpp"
#include <sstream>
#include <chrono>

SessionManager::SessionManager() : sessionCounter(0) {}

SessionManager::~SessionManager() {
    std::vector<std::shared_ptr<Session>> sessionsToJoin;
    {
        std::lock_guard<std::mutex> lock(sessionMutex);
        for (const auto& [id, session] : sessions) {
            sessionsToJoin.push_back(session);
        }
    }

    for (const auto& session : sessionsToJoin) {
        if (session->worker.joinable()) {
            session->worker.join();
        }
    }
}

std::string SessionManager::generateSessionId() {
    uint64_t id = ++sessionCounter;
    std::stringstream ss;
    ss << "sess-" << id << "-" << std::chrono::steady_clock::now().time_since_epoch().count();
    return ss.str();
}

std::string SessionManager::startSession(const std::string& command, OutputCallback cb, int timeoutSec) {
    auto session = std::make_shared<Session>();
    session->sessionId = generateSessionId();
    session->processId = -1;
    session->active = true;
    session->exitCode = -1;

    {
        std::lock_guard<std::mutex> lock(sessionMutex);
        sessions[session->sessionId] = session;
    }

    Logger::getInstance().log(
        LogLevel::INFO,
        LogContext{0, session->sessionId, command},
        "Session created"
    );

    session->worker = std::thread(&SessionManager::sessionWorker, this, session, command, cb, timeoutSec);
    return session->sessionId;
}

void SessionManager::sessionWorker(std::shared_ptr<Session> session, const std::string& command, OutputCallback cb, int timeoutSec) {
    CommandExecutor executor;
    try {
        auto result = executor.execute(command, session->sessionId, cb, timeoutSec);
        std::lock_guard<std::mutex> lock(session->mutex);
        session->output = result.stdoutData;
        session->error = result.stderrData;
        session->exitCode = result.exitCode;
    } catch (const std::exception& ex) {
        Logger::getInstance().log(
            LogLevel::ERROR,
            LogContext{0, session->sessionId, command},
            std::string("Session worker failed: ") + ex.what()
        );
        std::lock_guard<std::mutex> lock(session->mutex);
        session->error = ex.what();
        session->exitCode = -1;
    }
    session->active = false;
    Logger::getInstance().log(
        LogLevel::INFO,
        LogContext{0, session->sessionId, command},
        "Session completed"
    );
}

bool SessionManager::endSession(const std::string& sessionId) {
    std::shared_ptr<Session> session;
    {
        std::lock_guard<std::mutex> lock(sessionMutex);
        auto it = sessions.find(sessionId);
        if (it == sessions.end()) {
            return false;
        }
        session = it->second;
        sessions.erase(it);
    }

    if (session->worker.joinable()) {
        session->worker.join();
    }
    Logger::getInstance().log(
        LogLevel::INFO,
        LogContext{0, sessionId, "-"},
        "Session ended"
    );
    return true;
}

bool SessionManager::isSessionActive(const std::string& sessionId) {
    std::lock_guard<std::mutex> lock(sessionMutex);
    auto it = sessions.find(sessionId);
    return it != sessions.end() && it->second->active;
}

std::string SessionManager::getSessionOutput(const std::string& sessionId) {
    std::lock_guard<std::mutex> lock(sessionMutex);
    auto it = sessions.find(sessionId);
    if (it != sessions.end()) {
        std::lock_guard<std::mutex> sessionLock(it->second->mutex);
        return it->second->output;
    }
    return "";
}

int SessionManager::getSessionExitCode(const std::string& sessionId) {
    std::lock_guard<std::mutex> lock(sessionMutex);
    auto it = sessions.find(sessionId);
    if (it != sessions.end()) {
        std::lock_guard<std::mutex> sessionLock(it->second->mutex);
        return it->second->exitCode;
    }
    return -1;
}
