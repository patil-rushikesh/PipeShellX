#pragma once

#include <ostream>
#include <string>
#include <vector>

namespace psx::cli {

// `pipeshellx sim election|mutex [options]` -- run the coordination algorithms
// (Bully election, Ricart-Agrawala mutual exclusion) over a simulated network
// with random delays, crashes and restarts, print the Lamport-stamped
// timeline, and check their invariants. Exit 0 when every invariant held,
// 1 when one was violated (e.g. --naive), 2 on a usage error.
int runSimCommand(const std::vector<std::string>& args, std::ostream& out, std::ostream& err);

} // namespace psx::cli
