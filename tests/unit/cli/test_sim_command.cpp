#include "psx/cli/sim_command.hpp"

#include <gtest/gtest.h>

#include <sstream>
#include <string>
#include <vector>

using psx::cli::runSimCommand;

namespace {
struct Run {
    int code = -1;
    std::string out;
    std::string err;
};
Run sim(std::vector<std::string> args) {
    std::ostringstream out;
    std::ostringstream err;
    Run r;
    r.code = runSimCommand(args, out, err);
    r.out = out.str();
    r.err = err.str();
    return r;
}
} // namespace

TEST(SimCommandTest, UsageAndUnknownModes) {
    EXPECT_EQ(sim({}).code, 2);
    EXPECT_EQ(sim({"--help"}).code, 0);
    EXPECT_EQ(sim({"paxos"}).code, 2);
}

TEST(SimCommandTest, RejectsMalformedAndOutOfRangeArguments) {
    for (const std::vector<std::string>& bad : std::vector<std::vector<std::string>>{
             {"election", "--nodes", "0"},
             {"election", "--nodes", "65"},
             {"election", "--nodes", "3x"},
             {"election", "--nodes", "-1"},
             {"election", "--nodes"},
             {"election", "--seed", "99999999999"},
             {"election", "--delay", "60-5"},
             {"election", "--delay", "5"},
             {"election", "--kill-leader-at", "abc"},
             {"election", "--revive-at", "100"},
             {"election", "--naive"},
             {"mutex", "--kill", "9@10"},
             {"mutex", "--kill", "2"},
             {"mutex", "--requests", "0"},
             {"mutex", "--kill-leader-at", "5"},
         }) {
        EXPECT_EQ(sim(bad).code, 2) << "accepted: " << bad.back();
    }
}

TEST(SimCommandTest, ElectionWithLeaderCrashPrintsRecoveryAndSucceeds) {
    const auto r = sim({"election", "--nodes", "4", "--seed", "3", "--kill-leader-at", "1500"});
    EXPECT_EQ(r.code, 0) << r.out << r.err;
    EXPECT_NE(r.out.find("becomes LEADER"), std::string::npos);
    EXPECT_NE(r.out.find("CRASHED"), std::string::npos);
    EXPECT_NE(r.out.find("[ok]"), std::string::npos);
    EXPECT_NE(r.out.find("leader is n3"), std::string::npos);
}

TEST(SimCommandTest, MutexSucceedsAndNaiveBaselineReportsViolation) {
    const auto good = sim({"mutex", "--quiet"});
    EXPECT_EQ(good.code, 0) << good.out;
    EXPECT_NE(good.out.find("theory 2(N-1) = 4"), std::string::npos) << good.out;

    const auto naive = sim({"mutex", "--naive"});
    EXPECT_EQ(naive.code, 1) << naive.out;
    EXPECT_NE(naive.out.find("SAFETY VIOLATION"), std::string::npos);
    EXPECT_NE(naive.out.find("[FAIL]"), std::string::npos);
}
