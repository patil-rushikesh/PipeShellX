#pragma once

#include <string>
#include <vector>

struct ClientEntry {
    std::string raw;
    std::string user;
    std::string host;
};

class ClientConfig {
public:
    ClientConfig() = default;

    void loadFromFile(const std::string& path);

    const std::vector<ClientEntry>& clients() const noexcept;
    bool empty() const noexcept;

    static ClientEntry parseEntry(const std::string& line);
    static bool isValidEntry(const std::string& line) noexcept;

private:
    std::vector<ClientEntry> clients_;
};
