#pragma once

#include "client_config.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

enum class ClientStatus {
    UNKNOWN,
    ONLINE,
    OFFLINE
};

struct Client {
    int id;
    std::string ssh_url;
    std::string password;
    bool online;
};

struct ManagedClient {
    Client client;
    ClientEntry entry;
    ClientStatus status{ClientStatus::UNKNOWN};
    std::string lastError;
    bool persistPassword{true};
};

class ClientManager {
public:
    explicit ClientManager(std::string configPath = "clients.txt");

    void load();
    void save() const;

    bool empty() const noexcept;
    const std::vector<ManagedClient>& clients() const noexcept;
    std::vector<ClientEntry> entries() const;
    std::vector<Client> listClients() const;

    void addClient(const std::string& specification, const std::optional<std::string>& password = std::nullopt);
    bool removeClient(const std::string& identifier);
    bool removeClient(int id);
    bool checkClientStatus(const std::string& identifier) const;
    bool checkClientStatus(int id) const;

    std::vector<ClientEntry> selectClients(const std::optional<std::string>& identifier) const;
    void updateClientStatus(const std::string& identifier, ClientStatus status, std::string errorMessage = {});
    void updateClientStatus(int id, ClientStatus status, std::string errorMessage = {});
    void resetStatuses() noexcept;

    static std::string statusToString(ClientStatus status);

private:
    std::string configPath_;
    std::vector<ManagedClient> clients_;
    int nextClientId_{1};

    void verifyClientConnectivity(ManagedClient& client);
    std::vector<ManagedClient>::iterator findClient(const std::string& identifier);
    std::vector<ManagedClient>::const_iterator findClient(const std::string& identifier) const;
    std::vector<ManagedClient>::iterator findClient(int id);
    std::vector<ManagedClient>::const_iterator findClient(int id) const;
    static bool matchesIdentifier(const ManagedClient& client, const std::string& identifier);
};
