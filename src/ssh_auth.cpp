#include "ssh_auth.hpp"

#include <cctype>
#include <string>
#include <vector>

namespace {

std::string toLowerCopy(const std::string& value) {
    std::string lowered;
    lowered.reserve(value.size());
    for (char c : value) {
        lowered.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    return lowered;
}

bool containsInsensitive(const std::string& text, const std::string& needle) {
    return toLowerCopy(text).find(toLowerCopy(needle)) != std::string::npos;
}

} // namespace

std::vector<std::string> buildSshBaseArguments(const ClientEntry& client) {
    std::vector<std::string> args{
        "/usr/bin/ssh",
        "-o",
        "StrictHostKeyChecking=no",
        "-o",
        "ConnectTimeout=5"
    };

    if (client.port != 22) {
        args.push_back("-p");
        args.push_back(std::to_string(client.port));
    }
    if (!client.identityFile.empty()) {
        args.push_back("-i");
        args.push_back(client.identityFile);
    }

    args.push_back(client.sshTarget());
    return args;
}

std::vector<std::string> buildSshCommandArguments(const ClientEntry& client, const std::string& remoteCommand) {
    std::vector<std::string> args;
    if (!client.password.empty()) {
        args.push_back("sshpass");
        args.push_back("-p");
        args.push_back(client.password);
    }

    auto sshArgs = buildSshBaseArguments(client);
    args.insert(args.end(), sshArgs.begin(), sshArgs.end());
    args.push_back(remoteCommand);
    return args;
}

bool isSshAuthenticationFailure(const std::string& stderrText) {
    return containsInsensitive(stderrText, "permission denied") ||
           containsInsensitive(stderrText, "authentication failed") ||
           containsInsensitive(stderrText, "no more authentication methods");
}
