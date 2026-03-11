#include "client_config.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <stdexcept>
#include <string>
#include <unordered_set>

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

} // namespace

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

        if (!seen.insert(entry.raw).second) {
            throw std::runtime_error(
                "Duplicate client entry at line " + std::to_string(lineNumber) + ": " + entry.raw
            );
        }

        loadedClients.push_back(std::move(entry));
    }

    clients_ = std::move(loadedClients);
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

    const std::size_t atPos = trimmed.find('@');
    if (atPos == std::string::npos || atPos == 0 || atPos == trimmed.size() - 1) {
        throw std::runtime_error("entry must be in the form user@host");
    }
    if (trimmed.find('@', atPos + 1) != std::string::npos) {
        throw std::runtime_error("entry contains multiple '@' separators");
    }

    ClientEntry entry;
    entry.raw = trimmed;
    entry.user = trimmed.substr(0, atPos);
    entry.host = trimmed.substr(atPos + 1);

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
