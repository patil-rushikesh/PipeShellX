#include "client_manager.hpp"

#include "logger.hpp"

#include <cerrno>
#include <algorithm>
#include <array>
#include <cstring>
#include <fcntl.h>
#include <sstream>
#include <stdexcept>
#include <sys/wait.h>
#include <unistd.h>

namespace {

class ScopedFd {
public:
    explicit ScopedFd(int fd = -1) noexcept
        : fd_(fd) {}

    ScopedFd(const ScopedFd&) = delete;
    ScopedFd& operator=(const ScopedFd&) = delete;

    ScopedFd(ScopedFd&& other) noexcept
        : fd_(other.release()) {}

    ScopedFd& operator=(ScopedFd&& other) noexcept {
        if (this != &other) {
            reset(other.release());
        }
        return *this;
    }

    ~ScopedFd() {
        reset();
    }

    int get() const noexcept {
        return fd_;
    }

    int release() noexcept {
        const int fd = fd_;
        fd_ = -1;
        return fd;
    }

    void reset(int fd = -1) noexcept {
        if (fd_ >= 0) {
            while (close(fd_) == -1 && errno == EINTR) {
            }
        }
        fd_ = fd;
    }

private:
    int fd_;
};

std::string readAllFromFd(int fd) {
    std::string output;
    std::array<char, 512> buffer{};
    for (;;) {
        const ssize_t bytesRead = read(fd, buffer.data(), buffer.size());
        if (bytesRead > 0) {
            output.append(buffer.data(), static_cast<std::size_t>(bytesRead));
            continue;
        }
        if (bytesRead == 0) {
            break;
        }
        if (errno == EINTR) {
            continue;
        }
        throw std::runtime_error(std::string("Failed to read SSH verification output: ") + std::strerror(errno));
    }
    return output;
}

int waitForChild(pid_t pid) {
    int status = 0;
    for (;;) {
        const pid_t result = waitpid(pid, &status, 0);
        if (result == pid) {
            return status;
        }
        if (result == -1 && errno == EINTR) {
            continue;
        }
        throw std::runtime_error(std::string("Failed to wait for SSH verification process: ") + std::strerror(errno));
    }
}

std::string trimWhitespace(std::string value) {
    const auto begin = value.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) {
        return {};
    }
    const auto end = value.find_last_not_of(" \t\r\n");
    return value.substr(begin, end - begin + 1);
}

} // namespace

ClientManager::ClientManager(std::string configPath)
    : configPath_(std::move(configPath)) {}

void ClientManager::load() {
    ClientConfig config;
    try {
        config.loadFromFile(configPath_);
    } catch (const std::exception& ex) {
        if (configPath_ == "clients.txt" && std::string(ex.what()).find("Failed to open client configuration file") != std::string::npos) {
            clients_.clear();
            return;
        }
        throw;
    }

    clients_.clear();
    clients_.reserve(config.clients().size());
    nextClientId_ = 1;
    for (const auto& entry : config.clients()) {
        clients_.push_back(ManagedClient{
            Client{nextClientId_++, entry.serialize(), false},
            entry,
            ClientStatus::UNKNOWN,
            {}
        });
    }
}

void ClientManager::save() const {
    ClientConfig config;
    std::vector<ClientEntry> entriesToPersist;
    entriesToPersist.reserve(clients_.size());
    for (const auto& client : clients_) {
        entriesToPersist.push_back(client.entry);
    }
    config.setClients(std::move(entriesToPersist));
    config.saveToFile(configPath_);
}

bool ClientManager::empty() const noexcept {
    return clients_.empty();
}

const std::vector<ManagedClient>& ClientManager::clients() const noexcept {
    return clients_;
}

std::vector<ClientEntry> ClientManager::entries() const {
    std::vector<ClientEntry> configuredClients;
    configuredClients.reserve(clients_.size());
    for (const auto& client : clients_) {
        configuredClients.push_back(client.entry);
    }
    return configuredClients;
}

std::vector<Client> ClientManager::listClients() const {
    std::vector<Client> listedClients;
    listedClients.reserve(clients_.size());
    for (const auto& managedClient : clients_) {
        listedClients.push_back(managedClient.client);
    }
    return listedClients;
}

void ClientManager::addClient(const std::string& specification) {
    const ClientEntry entry = ClientConfig::parseEntry(specification);
    if (findClient(entry.clientId()) != clients_.end()) {
        throw std::runtime_error("Client already exists: " + entry.clientId());
    }

    const LogContext addContext{
        getpid(),
        "client-manager",
        entry.clientId(),
        "add-client " + entry.serialize()
    };
    Logger::getInstance().log(LogLevel::INFO, addContext, "Adding client");

    clients_.push_back(ManagedClient{
        Client{nextClientId_++, entry.serialize(), false},
        entry,
        ClientStatus::UNKNOWN,
        {}
    });
    verifyClientConnectivity(clients_.back());
    save();
    Logger::getInstance().log(
        LogLevel::INFO,
        addContext,
        "Client added with status " + statusToString(clients_.back().status)
    );
}

bool ClientManager::removeClient(const std::string& identifier) {
    const auto it = findClient(identifier);
    if (it == clients_.end()) {
        return false;
    }

    const LogContext removeContext{
        getpid(),
        "client-manager",
        it->entry.clientId(),
        "remove-client " + identifier
    };
    Logger::getInstance().log(LogLevel::INFO, removeContext, "Removing client");

    clients_.erase(it);
    save();
    Logger::getInstance().log(LogLevel::INFO, removeContext, "Client removed");
    return true;
}

bool ClientManager::removeClient(int id) {
    const auto it = findClient(id);
    if (it == clients_.end()) {
        return false;
    }

    const LogContext removeContext{
        getpid(),
        "client-manager",
        it->entry.clientId(),
        "remove-client " + std::to_string(id)
    };
    Logger::getInstance().log(LogLevel::INFO, removeContext, "Removing client");

    clients_.erase(it);
    save();
    Logger::getInstance().log(LogLevel::INFO, removeContext, "Client removed");
    return true;
}

bool ClientManager::checkClientStatus(const std::string& identifier) const {
    const auto it = findClient(identifier);
    if (it == clients_.end()) {
        throw std::runtime_error("Unknown client: " + identifier);
    }
    return it->status == ClientStatus::ONLINE;
}

bool ClientManager::checkClientStatus(int id) const {
    const auto it = findClient(id);
    if (it == clients_.end()) {
        throw std::runtime_error("Unknown client id: " + std::to_string(id));
    }
    return it->status == ClientStatus::ONLINE;
}

std::vector<ClientEntry> ClientManager::selectClients(const std::optional<std::string>& identifier) const {
    if (!identifier.has_value()) {
        return entries();
    }

    const auto it = findClient(*identifier);
    if (it == clients_.end()) {
        throw std::runtime_error("Unknown client: " + *identifier);
    }

    return {it->entry};
}

void ClientManager::updateClientStatus(const std::string& identifier, ClientStatus status, std::string errorMessage) {
    const auto it = findClient(identifier);
    if (it == clients_.end()) {
        throw std::runtime_error("Unknown client: " + identifier);
    }

    it->status = status;
    it->client.online = status == ClientStatus::ONLINE;
    it->client.ssh_url = it->entry.serialize();
    it->lastError = std::move(errorMessage);
}

void ClientManager::updateClientStatus(int id, ClientStatus status, std::string errorMessage) {
    const auto it = findClient(id);
    if (it == clients_.end()) {
        throw std::runtime_error("Unknown client id: " + std::to_string(id));
    }

    it->status = status;
    it->client.online = status == ClientStatus::ONLINE;
    it->client.ssh_url = it->entry.serialize();
    it->lastError = std::move(errorMessage);
}

void ClientManager::resetStatuses() noexcept {
    for (auto& client : clients_) {
        client.status = ClientStatus::UNKNOWN;
        client.client.online = false;
        client.lastError.clear();
    }
}

void ClientManager::verifyClientConnectivity(ManagedClient& client) {
    std::array<int, 2> pipeFds{-1, -1};
    if (pipe(pipeFds.data()) == -1) {
        throw std::runtime_error(std::string("Failed to create SSH verification pipe: ") + std::strerror(errno));
    }

    ScopedFd readEnd(pipeFds[0]);
    ScopedFd writeEnd(pipeFds[1]);
    const std::string commandDescription = "ssh " + client.entry.clientId() + " \"echo connected\"";
    const std::string sessionId = "client-verify-" + std::to_string(client.client.id);

    Logger::getInstance().log(
        LogLevel::INFO,
        LogContext{getpid(), sessionId, client.entry.clientId(), commandDescription},
        "Verifying SSH connectivity"
    );

    const pid_t pid = fork();
    if (pid < 0) {
        throw std::runtime_error(std::string("Failed to fork SSH verification process: ") + std::strerror(errno));
    }

    if (pid == 0) {
        if (dup2(writeEnd.get(), STDOUT_FILENO) == -1 || dup2(writeEnd.get(), STDERR_FILENO) == -1) {
            _exit(127);
        }

        readEnd.reset();
        writeEnd.reset();

        std::vector<std::string> arguments{
            "/usr/bin/ssh",
            "-o", "BatchMode=yes",
            "-o", "ConnectTimeout=5"
        };
        if (client.entry.port != 22) {
            arguments.push_back("-p");
            arguments.push_back(std::to_string(client.entry.port));
        }
        if (!client.entry.identityFile.empty()) {
            arguments.push_back("-i");
            arguments.push_back(client.entry.identityFile);
        }
        arguments.push_back(client.entry.sshTarget());
        arguments.push_back("echo connected");

        std::vector<char*> argv;
        argv.reserve(arguments.size() + 1);
        for (auto& argument : arguments) {
            argv.push_back(argument.data());
        }
        argv.push_back(nullptr);

        execvp(argv[0], argv.data());
        _exit(errno == ENOENT ? 127 : 126);
    }

    writeEnd.reset();

    std::string output;
    int waitStatus = 0;
    try {
        output = readAllFromFd(readEnd.get());
        readEnd.reset();
        waitStatus = waitForChild(pid);
    } catch (...) {
        readEnd.reset();
        int ignoredStatus = 0;
        while (waitpid(pid, &ignoredStatus, 0) == -1 && errno == EINTR) {
        }
        throw;
    }

    const std::string trimmedOutput = trimWhitespace(output);
    const bool connected =
        WIFEXITED(waitStatus) &&
        WEXITSTATUS(waitStatus) == 0 &&
        trimmedOutput.find("connected") != std::string::npos;

    client.status = connected ? ClientStatus::ONLINE : ClientStatus::OFFLINE;
    client.client.online = connected;
    client.client.ssh_url = client.entry.serialize();
    client.lastError = connected ? "" : (trimmedOutput.empty() ? "ERROR: connection failed" : trimmedOutput);

    Logger::getInstance().log(
        connected ? LogLevel::INFO : LogLevel::ERROR,
        LogContext{getpid(), sessionId, client.entry.clientId(), commandDescription},
        connected ? "SSH connectivity verified" : client.lastError
    );
}

std::string ClientManager::statusToString(ClientStatus status) {
    switch (status) {
        case ClientStatus::UNKNOWN:
            return "UNKNOWN";
        case ClientStatus::ONLINE:
            return "ONLINE";
        case ClientStatus::OFFLINE:
            return "OFFLINE";
    }
    return "UNKNOWN";
}

std::vector<ManagedClient>::iterator ClientManager::findClient(const std::string& identifier) {
    return std::find_if(clients_.begin(), clients_.end(), [&](const ManagedClient& client) {
        return matchesIdentifier(client, identifier);
    });
}

std::vector<ManagedClient>::const_iterator ClientManager::findClient(const std::string& identifier) const {
    return std::find_if(clients_.begin(), clients_.end(), [&](const ManagedClient& client) {
        return matchesIdentifier(client, identifier);
    });
}

std::vector<ManagedClient>::iterator ClientManager::findClient(int id) {
    return std::find_if(clients_.begin(), clients_.end(), [&](const ManagedClient& client) {
        return client.client.id == id;
    });
}

std::vector<ManagedClient>::const_iterator ClientManager::findClient(int id) const {
    return std::find_if(clients_.begin(), clients_.end(), [&](const ManagedClient& client) {
        return client.client.id == id;
    });
}

bool ClientManager::matchesIdentifier(const ManagedClient& client, const std::string& identifier) {
    return identifier == std::to_string(client.client.id) ||
           identifier == client.entry.clientId() ||
           identifier == client.client.ssh_url ||
           identifier == client.entry.sshTarget() ||
           identifier == client.entry.host ||
           identifier == client.entry.raw;
}
