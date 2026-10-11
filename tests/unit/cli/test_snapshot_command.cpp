#include "psx/cli/snapshot_command.hpp"
#include "psx/runtime/cluster_snapshot.hpp"
#include "test_support.hpp"

#include <gtest/gtest.h>
#include <sstream>

using psx::cli::runSnapshotCommand;
using psx::runtime::ClusterSnapshot;
using psx::runtime::NodeSnapshot;

TEST(SnapshotCommandTest, EmptyArgsPrintsUsageAndReturnsUsageCode) {
    std::ostringstream out;
    std::ostringstream err;
    const int rc = runSnapshotCommand({}, out, err);
    EXPECT_EQ(rc, 2);
    EXPECT_NE(err.str().find("Usage: pipeshellx snapshot"), std::string::npos);
}

TEST(SnapshotCommandTest, HelpOptionPrintsUsageAndReturnsZero) {
    std::ostringstream out;
    std::ostringstream err;
    const int rc = runSnapshotCommand({"--help"}, out, err);
    EXPECT_EQ(rc, 0);
    EXPECT_NE(out.str().find("Usage: pipeshellx snapshot"), std::string::npos);
}

TEST(SnapshotCommandTest, NonExistentFileReportsError) {
    std::ostringstream out;
    std::ostringstream err;
    const int rc = runSnapshotCommand({"/path/does/not/exist/snapshots.jsonl"}, out, err);
    EXPECT_EQ(rc, 1);
    EXPECT_NE(err.str().find("error:"), std::string::npos);
}

TEST(SnapshotCommandTest, FormatsValidSnapshotFileAsTable) {
    test_support::ScopedTempCwd cwd("cli-snapshot-test");
    const std::string path = (cwd.path() / "test-snap.jsonl").string();

    ClusterSnapshot snap("demo-run-42", 1700000000123);
    snap.record(NodeSnapshot{.host = "node-alpha", .stageId = "stage-0", .status = "exited", .exitCode = 0, .lamportTs = 1});
    snap.record(NodeSnapshot{.host = "node-beta", .stageId = "stage-1", .status = "running", .exitCode = 0, .lamportTs = 2});
    ASSERT_TRUE(snap.appendToFile(path));

    std::ostringstream out;
    std::ostringstream err;
    const int rc = runSnapshotCommand({"dump", path}, out, err);
    EXPECT_EQ(rc, 0) << err.str();
    const std::string output = out.str();
    EXPECT_NE(output.find("Cluster Snapshot [Run: demo-run-42"), std::string::npos);
    EXPECT_NE(output.find("node-alpha"), std::string::npos);
    EXPECT_NE(output.find("node-beta"), std::string::npos);
    EXPECT_NE(output.find("exited"), std::string::npos);
    EXPECT_NE(output.find("running"), std::string::npos);
}

TEST(SnapshotCommandTest, LatestAndJsonFlagsEmitJsonForLastSnapshot) {
    test_support::ScopedTempCwd cwd("cli-snapshot-latest");
    const std::string path = (cwd.path() / "history.jsonl").string();

    ClusterSnapshot s1("run-hist", 100);
    s1.record(NodeSnapshot{.host = "h1", .status = "running", .lamportTs = 1});
    ASSERT_TRUE(s1.appendToFile(path));

    ClusterSnapshot s2("run-hist", 200);
    s2.record(NodeSnapshot{.host = "h1", .status = "exited", .exitCode = 0, .lamportTs = 3});
    ASSERT_TRUE(s2.appendToFile(path));

    std::ostringstream out;
    std::ostringstream err;
    const int rc = runSnapshotCommand({path, "--latest", "--json"}, out, err);
    EXPECT_EQ(rc, 0) << err.str();
    const std::string jsonOut = out.str();
    EXPECT_NE(jsonOut.find(R"("status":"exited")"), std::string::npos);
    EXPECT_NE(jsonOut.find(R"("lamport_ts":3)"), std::string::npos);
    // Earlier status should not be present with --latest
    EXPECT_EQ(jsonOut.find(R"("status":"running")"), std::string::npos);
}
