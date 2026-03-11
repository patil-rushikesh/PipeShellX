#pragma once

#include <array>
#include <memory>
#include <string>
#include <stdexcept>

class PipeLogger {
public:
    virtual void log(const std::string& msg) = 0;
    virtual ~PipeLogger() = default;
};

class Pipe {
public:
    Pipe(std::shared_ptr<PipeLogger> logger = nullptr, bool nonBlocking = false);
    ~Pipe();

    // Disable copy, allow move
    Pipe(const Pipe&) = delete;
    Pipe& operator=(const Pipe&) = delete;
    Pipe(Pipe&&) noexcept;
    Pipe& operator=(Pipe&&) noexcept;

    int getReadFd() const;
    int getWriteFd() const;

    ssize_t write(const void* buf, size_t count);
    ssize_t read(void* buf, size_t count);

    void setNonBlocking(bool enable);

private:
    std::array<int, 2> fds;
    std::shared_ptr<PipeLogger> logger;
    bool nonBlocking;

    void closePipe();
    void setFdNonBlocking(int fd, bool enable);
};
