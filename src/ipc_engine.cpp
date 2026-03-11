#include "ipc_engine.h"
#include <unistd.h>
#include <fcntl.h>
#include <cstring>
#include <cerrno>

namespace {

void closeFd(int& fd) noexcept {
    if (fd == -1) {
        return;
    }

    while (close(fd) == -1 && errno == EINTR) {
    }
    fd = -1;
}

} // namespace

Pipe::Pipe(std::shared_ptr<PipeLogger> logger, bool nonBlocking)
    : fds{-1, -1}, logger(logger), nonBlocking(nonBlocking) {
    if (pipe(fds.data()) == -1) {
        if (logger) logger->log("Pipe creation failed: " + std::string(strerror(errno)));
        throw std::runtime_error("Pipe creation failed");
    }
    if (nonBlocking) {
        setFdNonBlocking(fds[0], true);
        setFdNonBlocking(fds[1], true);
    }
    if (logger) logger->log("Pipe created: readFd=" + std::to_string(fds[0]) + ", writeFd=" + std::to_string(fds[1]));
}

Pipe::~Pipe() {
    closePipe();
    if (logger) logger->log("Pipe destroyed");
}

Pipe::Pipe(Pipe&& other) noexcept
    : logger(other.logger), nonBlocking(other.nonBlocking) {
    fds[0] = other.fds[0];
    fds[1] = other.fds[1];
    other.fds[0] = -1;
    other.fds[1] = -1;
}

Pipe& Pipe::operator=(Pipe&& other) noexcept {
    if (this != &other) {
        closePipe();
        fds[0] = other.fds[0];
        fds[1] = other.fds[1];
        logger = other.logger;
        nonBlocking = other.nonBlocking;
        other.fds[0] = -1;
        other.fds[1] = -1;
    }
    return *this;
}

int Pipe::getReadFd() const { return fds[0]; }
int Pipe::getWriteFd() const { return fds[1]; }

ssize_t Pipe::write(const void* buf, size_t count) {
    ssize_t ret = ::write(fds[1], buf, count);
    if (ret == -1 && logger) logger->log("Pipe write failed: " + std::string(strerror(errno)));
    return ret;
}

ssize_t Pipe::read(void* buf, size_t count) {
    ssize_t ret = ::read(fds[0], buf, count);
    if (ret == -1 && logger) logger->log("Pipe read failed: " + std::string(strerror(errno)));
    return ret;
}

void Pipe::setNonBlocking(bool enable) {
    setFdNonBlocking(fds[0], enable);
    setFdNonBlocking(fds[1], enable);
    nonBlocking = enable;
    if (logger) logger->log("Pipe non-blocking set to " + std::string(enable ? "true" : "false"));
}

void Pipe::setFdNonBlocking(int fd, bool enable) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) {
        if (logger) logger->log("fcntl F_GETFL failed: " + std::string(strerror(errno)));
        throw std::runtime_error("fcntl F_GETFL failed");
    }
    if (enable)
        flags |= O_NONBLOCK;
    else
        flags &= ~O_NONBLOCK;
    if (fcntl(fd, F_SETFL, flags) == -1) {
        if (logger) logger->log("fcntl F_SETFL failed: " + std::string(strerror(errno)));
        throw std::runtime_error("fcntl F_SETFL failed");
    }
}

void Pipe::closePipe() {
    closeFd(fds[0]);
    closeFd(fds[1]);
}
