#include "process_manager.hpp"

#include "logger.hpp"

#include <unistd.h>
#include <sys/wait.h>
#include <sys/resource.h>
#include <poll.h>
#include <fcntl.h>
#include <signal.h>

#include <array>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <exception>
#include <stdexcept>
#include <string>
#include <vector>
#include <algorithm>
#include <mutex>

namespace {

void closeFd(int& fd) {
    if (fd != -1) {
        while (close(fd) == -1 && errno == EINTR) {
        }
        fd = -1;
    }
}

[[noreturn]] void childExitWithError(const std::string& message, int code) {
    const auto* data = message.c_str();
    std::size_t remaining = message.size();
    while (remaining > 0) {
        const ssize_t written = ::write(STDERR_FILENO, data, remaining);
        if (written > 0) {
            data += written;
            remaining -= static_cast<std::size_t>(written);
            continue;
        }
        if (written == -1 && errno == EINTR) {
            continue;
        }
        break;
    }
    _exit(code);
}

int calculatePollTimeoutMs(const std::chrono::steady_clock::time_point deadline) {
    if (deadline == std::chrono::steady_clock::time_point::max()) {
        return -1;
    }

    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline) {
        return 0;
    }

    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
    return static_cast<int>(remaining.count());
}

} // namespace

ProcessManager::ProcessManager()
    : childPid(-1), stdinPipe{-1, -1}, stdoutPipe{-1, -1}, stderrPipe{-1, -1} {
    installSigChldHandler();
}

ProcessManager::~ProcessManager() {
    try {
        reapChild(true);
        closePipes();
    } catch (...) {
    }
}

ProcessManager::ProcessManager(ProcessManager&& other) noexcept
    : childPid(other.childPid) {
    stdinPipe[0] = other.stdinPipe[0];
    stdinPipe[1] = other.stdinPipe[1];
    stdoutPipe[0] = other.stdoutPipe[0];
    stdoutPipe[1] = other.stdoutPipe[1];
    stderrPipe[0] = other.stderrPipe[0];
    stderrPipe[1] = other.stderrPipe[1];

    other.childPid = -1;
    other.stdinPipe[0] = other.stdinPipe[1] = -1;
    other.stdoutPipe[0] = other.stdoutPipe[1] = -1;
    other.stderrPipe[0] = other.stderrPipe[1] = -1;
}

ProcessManager& ProcessManager::operator=(ProcessManager&& other) noexcept {
    if (this != &other) {
        closePipes();

        childPid = other.childPid;
        stdinPipe[0] = other.stdinPipe[0];
        stdinPipe[1] = other.stdinPipe[1];
        stdoutPipe[0] = other.stdoutPipe[0];
        stdoutPipe[1] = other.stdoutPipe[1];
        stderrPipe[0] = other.stderrPipe[0];
        stderrPipe[1] = other.stderrPipe[1];

        other.childPid = -1;
        other.stdinPipe[0] = other.stdinPipe[1] = -1;
        other.stdoutPipe[0] = other.stdoutPipe[1] = -1;
        other.stderrPipe[0] = other.stderrPipe[1] = -1;
    }

    return *this;
}

void ProcessManager::setupPipes() {
    if (pipe(stdinPipe.data()) == -1) {
        throw std::runtime_error("stdin pipe creation failed: " + std::string(std::strerror(errno)));
    }
    if (pipe(stdoutPipe.data()) == -1) {
        closePipes();
        throw std::runtime_error("stdout pipe creation failed: " + std::string(std::strerror(errno)));
    }
    if (pipe(stderrPipe.data()) == -1) {
        closePipes();
        throw std::runtime_error("stderr pipe creation failed: " + std::string(std::strerror(errno)));
    }
}

void ProcessManager::closePipes() {
    closeFd(stdinPipe[0]);
    closeFd(stdinPipe[1]);
    closeFd(stdoutPipe[0]);
    closeFd(stdoutPipe[1]);
    closeFd(stderrPipe[0]);
    closeFd(stderrPipe[1]);
}

void ProcessManager::setNonBlocking(int fd) {
    const int flags = fcntl(fd, F_GETFL);
    if (flags == -1) {
        throw std::runtime_error("fcntl(F_GETFL) failed: " + std::string(std::strerror(errno)));
    }
    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1) {
        throw std::runtime_error("fcntl(F_SETFL) failed: " + std::string(std::strerror(errno)));
    }
}

void ProcessManager::installSigChldHandler() {
    static std::once_flag once;
    static std::exception_ptr installError;

    std::call_once(once, [] {
        struct sigaction action {};
        action.sa_handler = &ProcessManager::sigChldHandler;
        sigemptyset(&action.sa_mask);
        action.sa_flags = SA_NOCLDSTOP;

        if (sigaction(SIGCHLD, &action, nullptr) == -1) {
            installError = std::make_exception_ptr(
                std::runtime_error("sigaction(SIGCHLD) failed: " + std::string(std::strerror(errno)))
            );
        }
    });

    if (installError) {
        std::rethrow_exception(installError);
    }
}

void ProcessManager::sigChldHandler(int signo) {
    (void)signo;
}

void ProcessManager::reapChild(bool terminateProcessGroup) noexcept {
    if (childPid <= 0) {
        return;
    }

    if (terminateProcessGroup) {
        (void)kill(-childPid, SIGKILL);
    }

    int status = 0;
    while (waitpid(childPid, &status, 0) == -1) {
        if (errno == EINTR) {
            continue;
        }
        break;
    }

    childPid = -1;
}

bool ProcessManager::readAvailableData(int fd, std::string& output, bool& closed) {
    std::array<char, 4096> buffer{};
    bool madeProgress = false;

    while (true) {
        const ssize_t readCount = ::read(fd, buffer.data(), buffer.size());
        if (readCount > 0) {
            output.append(buffer.data(), static_cast<std::size_t>(readCount));
            madeProgress = true;
            continue;
        }
        if (readCount == 0) {
            closed = true;
            return madeProgress;
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return madeProgress;
        }
        throw std::runtime_error("read from pipe failed: " + std::string(std::strerror(errno)));
    }
}

bool ProcessManager::writeAvailableData(int fd,
                                        const std::string& input,
                                        std::size_t& written,
                                        bool& closed) {
    bool madeProgress = false;

    while (written < input.size()) {
        const ssize_t writeCount = ::write(fd, input.data() + written, input.size() - written);
        if (writeCount > 0) {
            written += static_cast<std::size_t>(writeCount);
            madeProgress = true;
            continue;
        }
        if (writeCount == -1 && errno == EINTR) {
            continue;
        }
        if (writeCount == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return madeProgress;
        }
        if (writeCount == -1 && errno == EPIPE) {
            closed = true;
            return madeProgress;
        }
        throw std::runtime_error("write to child stdin failed: " + std::string(std::strerror(errno)));
    }

    closed = true;
    return true;
}

ProcessManager::Result ProcessManager::execute(const std::vector<std::string>& args,
                                               const LogContext& context,
                                               const std::string& input,
                                               int timeoutSec) {
    if (args.empty()) {
        throw std::runtime_error("cannot execute empty command");
    }

    Logger::getInstance().log(LogLevel::DEBUG, context, "Creating IPC pipes");
    setupPipes();

    childPid = fork();
    if (childPid == -1) {
        closePipes();
        Logger::getInstance().log(LogLevel::ERROR, context, "fork() failed: " + std::string(std::strerror(errno)));
        throw std::runtime_error("fork() failed: " + std::string(std::strerror(errno)));
    }

    if (childPid == 0) {
        if (setpgid(0, 0) == -1) {
            childExitWithError("setpgid failed: " + std::string(std::strerror(errno)) + "\n", 126);
        }

        if (dup2(stdinPipe[0], STDIN_FILENO) == -1 ||
            dup2(stdoutPipe[1], STDOUT_FILENO) == -1 ||
            dup2(stderrPipe[1], STDERR_FILENO) == -1) {
            childExitWithError("dup2 failed: " + std::string(std::strerror(errno)) + "\n", 126);
        }

        closePipes();

        const rlimit cpuLimit{5, 5};
        const rlimit memLimit{64 * 1024 * 1024, 64 * 1024 * 1024};
        (void)setrlimit(RLIMIT_CPU, &cpuLimit);
        (void)setrlimit(RLIMIT_AS, &memLimit);

        std::vector<char*> argv;
        argv.reserve(args.size() + 1);
        for (const auto& arg : args) {
            argv.push_back(const_cast<char*>(arg.c_str()));
        }
        argv.push_back(nullptr);

        execvp(argv[0], argv.data());
        childExitWithError(
            "execvp failed for '" + args[0] + "': " + std::string(std::strerror(errno)) + "\n",
            127
        );
    }

    Logger::getInstance().log(
        LogLevel::INFO,
        LogContext{childPid, context.sessionId, context.command},
        "Child process created"
    );

    closeFd(stdinPipe[0]);
    closeFd(stdoutPipe[1]);
    closeFd(stderrPipe[1]);

    try {
        try {
            setNonBlocking(stdoutPipe[0]);
            setNonBlocking(stderrPipe[0]);
            if (stdinPipe[1] != -1) {
                setNonBlocking(stdinPipe[1]);
            }
        } catch (...) {
            reapChild(true);
            closePipes();
            throw;
        }

        std::string stdoutData;
        std::string stderrData;
        std::size_t inputWritten = 0;
        bool stdinClosed = input.empty();
        bool stdoutClosed = false;
        bool stderrClosed = false;
        bool timedOut = false;
        bool childExited = false;
        int status = 0;

        if (stdinClosed) {
            closeFd(stdinPipe[1]);
            Logger::getInstance().log(LogLevel::DEBUG, context, "Closed stdin pipe after sending input");
        }

        const auto deadline = timeoutSec > 0
            ? std::chrono::steady_clock::now() + std::chrono::seconds(timeoutSec)
            : std::chrono::steady_clock::time_point::max();

        while (!childExited || !stdoutClosed || !stderrClosed) {
            std::vector<pollfd> pollFds;
            pollFds.reserve(3);

            if (!stdinClosed && stdinPipe[1] != -1) {
                pollFds.push_back(pollfd{stdinPipe[1], POLLOUT, 0});
            }
            if (!stdoutClosed && stdoutPipe[0] != -1) {
                pollFds.push_back(pollfd{stdoutPipe[0], POLLIN | POLLHUP, 0});
            }
            if (!stderrClosed && stderrPipe[0] != -1) {
                pollFds.push_back(pollfd{stderrPipe[0], POLLIN | POLLHUP, 0});
            }

            int pollTimeoutMs = calculatePollTimeoutMs(deadline);
            if (pollFds.empty()) {
                pollTimeoutMs = pollTimeoutMs == -1 ? 50 : std::min(pollTimeoutMs, 50);
            }
            const int pollResult = poll(pollFds.data(), static_cast<nfds_t>(pollFds.size()), pollTimeoutMs);
            if (pollResult == -1) {
                if (errno == EINTR) {
                    continue;
                }
                throw std::runtime_error("poll failed: " + std::string(std::strerror(errno)));
            }

            if (pollResult == 0 && deadline != std::chrono::steady_clock::time_point::max()) {
                timedOut = true;
                (void)kill(-childPid, SIGKILL);
                Logger::getInstance().log(LogLevel::ERROR, context, "Command timed out; sent SIGKILL to process group");
            }

            for (const auto& pollFd : pollFds) {
                if ((pollFd.revents & (POLLERR | POLLNVAL)) != 0) {
                    throw std::runtime_error("poll reported invalid pipe state");
                }

                if (pollFd.fd == stdinPipe[1] && (pollFd.revents & POLLOUT) != 0) {
                    if (writeAvailableData(stdinPipe[1], input, inputWritten, stdinClosed) && stdinClosed) {
                        closeFd(stdinPipe[1]);
                        Logger::getInstance().log(LogLevel::DEBUG, context, "Finished writing command input to child stdin");
                    }
                }

                if (pollFd.fd == stdoutPipe[0] && (pollFd.revents & (POLLIN | POLLHUP)) != 0) {
                    const std::size_t before = stdoutData.size();
                    if (readAvailableData(stdoutPipe[0], stdoutData, stdoutClosed) && stdoutClosed) {
                        closeFd(stdoutPipe[0]);
                    } else if (stdoutClosed) {
                        closeFd(stdoutPipe[0]);
                    }
                    if (stdoutData.size() > before) {
                        Logger::getInstance().log(
                            LogLevel::DEBUG,
                            context,
                            "Read " + std::to_string(stdoutData.size() - before) + " bytes from child stdout"
                        );
                    }
                }

                if (pollFd.fd == stderrPipe[0] && (pollFd.revents & (POLLIN | POLLHUP)) != 0) {
                    const std::size_t before = stderrData.size();
                    if (readAvailableData(stderrPipe[0], stderrData, stderrClosed) && stderrClosed) {
                        closeFd(stderrPipe[0]);
                    } else if (stderrClosed) {
                        closeFd(stderrPipe[0]);
                    }
                    if (stderrData.size() > before) {
                        Logger::getInstance().log(
                            LogLevel::DEBUG,
                            context,
                            "Read " + std::to_string(stderrData.size() - before) + " bytes from child stderr"
                        );
                    }
                }
            }

            if (!childExited && childPid > 0) {
                const pid_t waitResult = waitpid(childPid, &status, WNOHANG);
                if (waitResult == childPid) {
                    childExited = true;
                    Logger::getInstance().log(
                        LogLevel::DEBUG,
                        LogContext{waitResult, context.sessionId, context.command},
                        "Child process reaped with waitpid"
                    );
                    childPid = -1;
                } else if (waitResult == -1) {
                    Logger::getInstance().log(LogLevel::ERROR, context, "waitpid failed: " + std::string(std::strerror(errno)));
                    throw std::runtime_error("waitpid failed: " + std::string(std::strerror(errno)));
                }
            }
        }

        const int exitCode = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
        Logger::getInstance().log(
            exitCode == 0 ? LogLevel::INFO : LogLevel::ERROR,
            context,
            "IPC collection completed: stdout=" + std::to_string(stdoutData.size()) +
                " bytes, stderr=" + std::to_string(stderrData.size()) + " bytes"
        );
        return Result{exitCode, std::move(stdoutData), std::move(stderrData), timedOut};
    } catch (...) {
        Logger::getInstance().log(LogLevel::ERROR, context, "Process execution failed; cleaning up child and pipes");
        reapChild(true);
        closePipes();
        throw;
    }
}
