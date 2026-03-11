#include "client_config.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <vector>

namespace {

std::string trim(const std::string& value) {
    const auto begin = std::find_if_not(value.begin(), value.end(), [](unsigned char c) {
        return std::isspace(c) != 0;
    });
    const auto end = std::find_if_not(value.rbegin(), value.rend(), [](unsigned char c) {
        return std::isspace(c) != 0;
    }).base();

    if (begin >= end) {
        return "";
    }

    return std::string(begin, end);
}

bool isValidUserChar(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_' || c == '-' || c == '.';
}

bool isValidHostChar(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '.' || c == '-' || c == '_';
}

bool isValidPathChar(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '/' || c == '_' || c == '-' || c == '.' || c == '~';
}

std::pair<std::string, std::string> splitOnce(const std::string& value, char separator) {
    const std::size_t position = value.find(separator);
    if (position == std::string::npos) {
        return {value, ""};
    }
    return {value.substr(0, position), value.substr(position + 1)};
}

std::vector<std::string> splitAll(const std::string& value, char separator) {
    std::vector<std::string> parts;
    std::size_t start = 0;
    while (start <= value.size()) {
        const std::size_t end = value.find(separator, start);
        if (end == std::string::npos) {
            parts.push_back(value.substr(start));
            break;
        }
        parts.push_back(value.substr(start, end - start));
        start = end + 1;
    }
    return parts;
}

ClientEntry parseLegacyEntry(const std::string& trimmed) {
    const std::size_t atPos = trimmed.find('@');
    if (atPos == std::string::npos || atPos == 0 || atPos == trimmed.size() - 1) {
        throw std::runtime_error("entry must be in the form user@host");
    }
    if (trimmed.find('@', atPos + 1) != std::string::npos) {
        throw std::runtime_error("entry contains multiple '@' separators");
    }

    ClientEntry entry;
    entry.user = trimmed.substr(0, atPos);
    entry.host = trimmed.substr(atPos + 1);
    return entry;
}

ClientEntry parseUrlEntry(const std::string& trimmed) {
    constexpr std::string_view prefix = "ssh://";
    if (trimmed.rfind(prefix, 0) != 0) {
        throw std::runtime_error("unsupported URL scheme");
    }

    const std::string remainder = trimmed.substr(prefix.size());
    const auto [authority, query] = splitOnce(remainder, '?');
    ClientEntry entry = parseLegacyEntry(authority);

    const auto [hostPart, portPart] = splitOnce(entry.host, ':');
    entry.host = hostPart;
    if (!portPart.empty()) {
        try {
            const unsigned long port = std::stoul(portPart);
            if (port == 0 || port > 65535) {
                throw std::runtime_error("port out of range");
            }
            entry.port = static_cast<std::uint16_t>(port);
        } catch (const std::exception&) {
            throw std::runtime_error("invalid port");
        }
    }

    if (!query.empty()) {
        for (const auto& parameter : splitAll(query, '&')) {
            const auto [key, value] = splitOnce(parameter, '=');
            if (key.empty()) {
                throw std::runtime_error("unsupported SSH URL query parameter");
            }

            if (key == "identity" || key == "key") {
                if (value.empty()) {
                    throw std::runtime_error("identity file path is empty");
                }
                if (!std::all_of(value.begin(), value.end(), isValidPathChar)) {
                    throw std::runtime_error("identity file contains invalid characters");
                }
                entry.identityFile = value;
                continue;
            }

            if (key == "password") {
                throw std::runtime_error("passwords are not allowed in client configuration; use interactive prompt");
            }

            throw std::runtime_error("unsupported SSH URL query parameter");
        }
    }

    return entry;
}

void validateEntryFields(const ClientEntry& entry) {
    if (entry.user.empty() || entry.host.empty()) {
        throw std::runtime_error("entry must include both user and host");
    }
    if (!std::all_of(entry.user.begin(), entry.user.end(), isValidUserChar)) {
        throw std::runtime_error("user contains invalid characters");
    }
    if (!std::all_of(entry.host.begin(), entry.host.end(), isValidHostChar)) {
        throw std::runtime_error("host contains invalid characters");
    }
    if (entry.host.front() == '.' || entry.host.back() == '.' ||
        entry.host.front() == '-' || entry.host.back() == '-') {
        throw std::runtime_error("host has an invalid boundary character");
    }
}

} // namespace

std::string ClientEntry::clientId() const {
    if (port == 22) {
        return user + "@" + host;
    }
    return user + "@" + host + ":" + std::to_string(port);
}

std::string ClientEntry::sshTarget() const {
    return user + "@" + host;
}

std::string ClientEntry::serialize() const {
    if (port == 22 && identityFile.empty()) {
        return user + "@" + host;
    }

    std::string serialized = "ssh://" + user + "@" + host;
    if (port != 22) {
        serialized += ":" + std::to_string(port);
    }

    std::vector<std::string> queryParameters;
    if (!identityFile.empty()) {
        queryParameters.push_back("identity=" + identityFile);
    }
    if (!queryParameters.empty()) {
        serialized += "?";
        for (std::size_t index = 0; index < queryParameters.size(); ++index) {
            if (index != 0) {
                serialized += "&";
            }
            serialized += queryParameters[index];
        }
    }
    return serialized;
}

void ClientConfig::loadFromFile(const std::string& path) {
    std::ifstream input(path);
    if (!input.is_open()) {
        throw std::runtime_error("Failed to open client configuration file: " + path);
    }

    std::vector<ClientEntry> loadedClients;
    std::unordered_set<std::string> seen;
    std::string line;
    std::size_t lineNumber = 0;

    while (std::getline(input, line)) {
        ++lineNumber;
        const std::string trimmed = trim(line);
        if (trimmed.empty() || trimmed.front() == '#') {
            continue;
        }

        ClientEntry entry;
        try {
            entry = parseEntry(trimmed);
        } catch (const std::exception& ex) {
            throw std::runtime_error(
                "Invalid client entry at line " + std::to_string(lineNumber) + ": " + ex.what()
            );
        }

        if (!seen.insert(entry.clientId()).second) {
            throw std::runtime_error(
                "Duplicate client entry at line " + std::to_string(lineNumber) + ": " + entry.clientId()
            );
        }

        loadedClients.push_back(std::move(entry));
    }

    clients_ = std::move(loadedClients);
}

void ClientConfig::saveToFile(const std::string& path) const {
    std::ofstream output(path, std::ios::trunc);
    if (!output.is_open()) {
        throw std::runtime_error("Failed to write client configuration file: " + path);
    }

    for (const auto& client : clients_) {
        output << client.serialize() << '\n';
    }
}

void ClientConfig::setClients(std::vector<ClientEntry> clients) {
    clients_ = std::move(clients);
}

const std::vector<ClientEntry>& ClientConfig::clients() const noexcept {
    return clients_;
}

bool ClientConfig::empty() const noexcept {
    return clients_.empty();
}

ClientEntry ClientConfig::parseEntry(const std::string& line) {
    const std::string trimmed = trim(line);
    if (trimmed.empty()) {
        throw std::runtime_error("entry is empty");
    }

    ClientEntry entry = trimmed.rfind("ssh://", 0) == 0 ? parseUrlEntry(trimmed) : parseLegacyEntry(trimmed);
    validateEntryFields(entry);
    entry.raw = entry.serialize();
    return entry;
}

bool ClientConfig::isValidEntry(const std::string& line) noexcept {
    try {
        static_cast<void>(parseEntry(line));
        return true;
    } catch (...) {
        return false;
    }
}
