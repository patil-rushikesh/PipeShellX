#pragma once

#include "client_config.hpp"

#include <string>
#include <vector>

std::vector<std::string> buildSshBaseArguments(const ClientEntry& client);
std::vector<std::string> buildSshCommandArguments(const ClientEntry& client, const std::string& remoteCommand);
bool isSshAuthenticationFailure(const std::string& stderrText);
