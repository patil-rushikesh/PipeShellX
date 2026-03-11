#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct ClientEntry {
    std::string raw;
    std::string user;
    std::string host;
    std::uint16_t port{22};
    std::string identityFile;
    std::string password;

    std::string clientId() const;
    std::string sshTarget() const;
    std::string serialize() const;
};

class ClientConfig {
public:
    ClientConfig() = default;

    void loadFromFile(const std::string& path);
    void saveToFile(const std::string& path) const;
    void setClients(std::vector<ClientEntry> clients);

    const std::vector<ClientEntry>& clients() const noexcept;
    bool empty() const noexcept;

    static ClientEntry parseEntry(const std::string& line);
    static bool isValidEntry(const std::string& line) noexcept;

private:
    std::vector<ClientEntry> clients_;
};
