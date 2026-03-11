#pragma once

#include <string>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <atomic>
#include <functional>

struct Session {
    std::string sessionId;
    pid_t processId;
    std::thread worker;
    std::atomic<bool> active;
    std::string output;
    std::string error;
    int exitCode;
    std::mutex mutex;
};

class SessionManager {
public:
    using OutputCallback = std::function<void(const std::string&, bool isStdout)>;

    SessionManager();
    ~SessionManager();

    std::string startSession(const std::string& command, OutputCallback cb, int timeoutSec = 0);
    bool endSession(const std::string& sessionId);
    bool isSessionActive(const std::string& sessionId);
    std::string getSessionOutput(const std::string& sessionId);
    int getSessionExitCode(const std::string& sessionId);

private:
    std::map<std::string, std::shared_ptr<Session>> sessions;
    std::mutex sessionMutex;
    std::atomic<uint64_t> sessionCounter;

    void sessionWorker(std::shared_ptr<Session> session, const std::string& command, OutputCallback cb, int timeoutSec);
    std::string generateSessionId();
};
