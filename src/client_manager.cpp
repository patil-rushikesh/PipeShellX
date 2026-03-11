#include "client_manager.hpp"

#include "logger.hpp"
#include "process_manager.hpp"

#include <algorithm>
#include <optional>
#include <stdexcept>
#include <string>
#include <unistd.h>

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
            Client{nextClientId_++, entry.serialize(), entry.password, false},
            entry,
            ClientStatus::UNKNOWN,
            {},
            true
        });
    }
}

void ClientManager::save() const {
    ClientConfig config;
    std::vector<ClientEntry> entriesToPersist;
    entriesToPersist.reserve(clients_.size());
    for (const auto& client : clients_) {
        ClientEntry persistedEntry = client.entry;
        if (!client.persistPassword) {
            persistedEntry.password.clear();
            persistedEntry.raw = persistedEntry.serialize();
        }
        entriesToPersist.push_back(std::move(persistedEntry));
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

void ClientManager::addClient(const std::string& specification, const std::optional<std::string>& password) {
    ClientEntry entry = ClientConfig::parseEntry(specification);
    if (password.has_value()) {
        entry.password = *password;
    }
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
        Client{nextClientId_++, entry.serialize(), entry.password, false},
        entry,
        ClientStatus::UNKNOWN,
        {},
        false
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
    it->client.password = it->entry.password;
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
    it->client.password = it->entry.password;
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
    const std::string remoteCommand = "echo connected";
    const std::string sessionId = "client-verify-" + std::to_string(client.client.id);
    const LogContext context{getpid(), sessionId, client.entry.clientId(), "ssh " + client.entry.clientId() + " " + remoteCommand};

    Logger::getInstance().log(LogLevel::INFO, context, "Verifying SSH connectivity");

    ProcessManager processManager;
    const auto result = processManager.executeRemote({client.entry}, remoteCommand, context, 10);
    const auto& clientResult = result.clientResults.front();
    const bool connected =
        clientResult.exitCode == 0 &&
        !clientResult.timedOut &&
        clientResult.stdoutData.find("connected") != std::string::npos;

    client.status = connected ? ClientStatus::ONLINE : ClientStatus::OFFLINE;
    client.client.online = connected;
    client.client.ssh_url = client.entry.serialize();
    client.client.password = client.entry.password;
    client.lastError = connected ? std::string() : (clientResult.errorMessage.empty() ? clientResult.stderrData : clientResult.errorMessage);

    Logger::getInstance().log(
        connected ? LogLevel::INFO : LogLevel::ERROR,
        context,
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
