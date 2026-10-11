#include "psx/runtime/cluster_snapshot.hpp"

#include "test_support.hpp"

#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <string>
#include <vector>

using psx::runtime::ClusterSnapshot;
using psx::runtime::NodeSnapshot;

namespace {

std::vector<std::string> readLines(const std::string& path) {
    std::ifstream in(path);
    std::vector<std::string> lines;
    std::string line;
    while (std::getline(in, line)) {
        lines.push_back(line);
    }
    return lines;
}

} // namespace

TEST(ClusterSnapshotTest, EmptySnapshotSerializesToEmptyNodesArray) {
    ClusterSnapshot snap("run-1");
    const std::string line = snap.toJsonLine();
    EXPECT_NE(line.find(R"("type":"cluster_snapshot")"), std::string::npos) << line;
    EXPECT_NE(line.find(R"("run_id":"run-1")"), std::string::npos) << line;
    EXPECT_NE(line.find(R"("nodes":[])"), std::string::npos) << line;
}

TEST(ClusterSnapshotTest, RecordAccumulatesInCallOrder) {
    ClusterSnapshot snap("run-2");
    snap.record(NodeSnapshot{.host = "h1", .stageId = "s0", .status = "running", .exitCode = 0, .lamportTs = 3});
    snap.record(NodeSnapshot{.host = "h2", .stageId = "s1", .status = "exited", .exitCode = 1, .lamportTs = 7});

    ASSERT_EQ(snap.nodes().size(), 2U);
    EXPECT_EQ(snap.nodes()[0].host, "h1");
    EXPECT_EQ(snap.nodes()[1].host, "h2");

    const std::string line = snap.toJsonLine();
    EXPECT_NE(line.find(R"("host":"h1")"), std::string::npos) << line;
    EXPECT_NE(line.find(R"("host":"h2")"), std::string::npos) << line;
    EXPECT_NE(line.find(R"("status":"running")"), std::string::npos) << line;
    EXPECT_NE(line.find(R"("status":"exited")"), std::string::npos) << line;
    EXPECT_NE(line.find(R"("lamport_ts":3)"), std::string::npos) << line;
    EXPECT_NE(line.find(R"("lamport_ts":7)"), std::string::npos) << line;
    // h1 must appear before h2 in the serialized array (call order preserved).
    EXPECT_LT(line.find("\"h1\""), line.find("\"h2\""));
}

TEST(ClusterSnapshotTest, AppendToFileWritesOneLinePerCallAndCreatesParentDirs) {
    test_support::ScopedTempCwd cwd("cluster-snapshot");
    const std::string path = (cwd.path() / "nested" / "snapshots.jsonl").string();

    ClusterSnapshot first("run-a");
    first.record(NodeSnapshot{.host = "h1", .status = "running"});
    ASSERT_TRUE(first.appendToFile(path));

    ClusterSnapshot second("run-a");
    second.record(NodeSnapshot{.host = "h1", .status = "exited", .exitCode = 0});
    ASSERT_TRUE(second.appendToFile(path));

    const auto lines = readLines(path);
    ASSERT_EQ(lines.size(), 2U);
    EXPECT_NE(lines[0].find(R"("status":"running")"), std::string::npos) << lines[0];
    EXPECT_NE(lines[1].find(R"("status":"exited")"), std::string::npos) << lines[1];
}

TEST(ClusterSnapshotTest, UnwritablePathReturnsFalseWithoutThrowing) {
    ClusterSnapshot snap("run-x");
    snap.record(NodeSnapshot{.host = "h1", .status = "running"});
    EXPECT_FALSE(snap.appendToFile("/this/path/does/not/exist/and/cannot/be/made/s.jsonl"));
}

TEST(ClusterSnapshotTest, FromJsonLineRoundTripsToJsonLine) {
    ClusterSnapshot original("run-roundtrip", 1700000000000);
    original.record(NodeSnapshot{.host = "nodeA", .stageId = "stage-0", .status = "exited", .exitCode = 0, .lamportTs = 10});
    original.record(NodeSnapshot{.host = "nodeB", .stageId = "stage-1", .status = "running", .exitCode = 0, .lamportTs = 15});

    const std::string line = original.toJsonLine();
    auto parsed = ClusterSnapshot::fromJsonLine(line);
    ASSERT_TRUE(parsed.ok()) << parsed.error().message();
    EXPECT_EQ(parsed.value().runId(), "run-roundtrip");
    EXPECT_EQ(parsed.value().timestampEpochMs(), 1700000000000);
    ASSERT_EQ(parsed.value().nodes().size(), 2U);
    EXPECT_EQ(parsed.value().nodes()[0], original.nodes()[0]);
    EXPECT_EQ(parsed.value().nodes()[1], original.nodes()[1]);
}

TEST(ClusterSnapshotTest, FromJsonLineRejectsInvalidData) {
    EXPECT_FALSE(ClusterSnapshot::fromJsonLine("not json").ok());
    EXPECT_FALSE(ClusterSnapshot::fromJsonLine(R"({"type":"audit_log"})").ok());
    EXPECT_FALSE(ClusterSnapshot::fromJsonLine(R"({"type":"cluster_snapshot"})").ok());
}

TEST(ClusterSnapshotTest, ReadFromFileLoadsAllSnapshots) {
    test_support::ScopedTempCwd cwd("read-snapshots");
    const std::string path = (cwd.path() / "snapshots.jsonl").string();

    ClusterSnapshot s1("run-batch", 100);
    s1.record(NodeSnapshot{.host = "h1", .status = "connecting"});
    s1.appendToFile(path);

    ClusterSnapshot s2("run-batch", 200);
    s2.record(NodeSnapshot{.host = "h1", .status = "running"});
    s2.appendToFile(path);

    ClusterSnapshot s3("run-batch", 300);
    s3.record(NodeSnapshot{.host = "h1", .status = "exited", .exitCode = 0, .lamportTs = 5});
    s3.appendToFile(path);

    auto list = ClusterSnapshot::readFromFile(path);
    ASSERT_TRUE(list.ok());
    ASSERT_EQ(list.value().size(), 3U);
    EXPECT_EQ(list.value()[0].timestampEpochMs(), 100);
    EXPECT_EQ(list.value()[1].timestampEpochMs(), 200);
    EXPECT_EQ(list.value()[2].timestampEpochMs(), 300);
    EXPECT_EQ(list.value()[2].nodes()[0].status, "exited");
    EXPECT_EQ(list.value()[2].nodes()[0].lamportTs, 5U);
}

TEST(ClusterSnapshotTest, FormatTableRendersExpectedColumns) {
    ClusterSnapshot snap("run-table-test", 123456789);
    snap.record(NodeSnapshot{.host = "10.0.0.1", .stageId = "s0", .status = "exited", .exitCode = 0, .lamportTs = 1});
    snap.record(NodeSnapshot{.host = "10.0.0.2", .stageId = "s1", .status = "running", .exitCode = 0, .lamportTs = 2});

    const std::string table = snap.formatTable();
    EXPECT_NE(table.find("Cluster Snapshot [Run: run-table-test"), std::string::npos);
    EXPECT_NE(table.find("HOST"), std::string::npos);
    EXPECT_NE(table.find("STAGE"), std::string::npos);
    EXPECT_NE(table.find("STATUS"), std::string::npos);
    EXPECT_NE(table.find("EXIT"), std::string::npos);
    EXPECT_NE(table.find("LAMPORT_TS"), std::string::npos);
    EXPECT_NE(table.find("10.0.0.1"), std::string::npos);
    EXPECT_NE(table.find("10.0.0.2"), std::string::npos);
}

// --- Hardening regressions (see docs/ds-project/07-security-review.md) ---

TEST(ClusterSnapshotTest, HostileStringsRoundTripExactly) {
    ClusterSnapshot original("run-]}", 1);
    original.record(
        NodeSnapshot{.host = "a]}\"b\\", .stageId = "s{0}", .status = "running", .exitCode = 0, .lamportTs = 9});
    original.record(
        NodeSnapshot{.host = "second", .stageId = "s1", .status = "exited", .exitCode = 3, .lamportTs = 10});
    auto parsed = ClusterSnapshot::fromJsonLine(original.toJsonLine());
    ASSERT_TRUE(parsed.ok()) << parsed.error().message();
    EXPECT_EQ(parsed.value().runId(), "run-]}");
    ASSERT_EQ(parsed.value().nodes().size(), 2U) << "a ']' inside a host string truncated the node list";
    EXPECT_EQ(parsed.value().nodes()[0], original.nodes()[0]);
    EXPECT_EQ(parsed.value().nodes()[1], original.nodes()[1]);
}

TEST(ClusterSnapshotTest, RejectsNegativeLamportTimestampInsteadOfWrapping) {
    const std::string line =
        R"({"type":"cluster_snapshot","run_id":"r","ts_epoch_ms":1,"nodes":[{"host":"h","stage_id":"s","status":"exited","exit_code":0,"lamport_ts":-1}]})";
    EXPECT_FALSE(ClusterSnapshot::fromJsonLine(line).ok());
}

TEST(ClusterSnapshotTest, RejectsDuplicateFieldsAndWrongTypes) {
    EXPECT_FALSE(
        ClusterSnapshot::fromJsonLine(R"({"type":"cluster_snapshot","run_id":"r","run_id":"other","nodes":[]})").ok());
    EXPECT_FALSE(ClusterSnapshot::fromJsonLine(R"({"type":"cluster_snapshot","run_id":"r","nodes":{}})").ok());
    EXPECT_FALSE(ClusterSnapshot::fromJsonLine(R"({"type":"cluster_snapshot","run_id":7,"nodes":[]})").ok());
}

TEST(ClusterSnapshotTest, FormatTableNeutralisesTerminalEscapes) {
    ClusterSnapshot snap("run\x1b]0;pwned\x07", 1);
    snap.record(NodeSnapshot{.host = "evil\x1b[2J", .stageId = "s", .status = "running\r", .lamportTs = 1});
    const std::string table = snap.formatTable();
    EXPECT_EQ(table.find('\x1b'), std::string::npos) << "ESC reached the terminal";
    EXPECT_EQ(table.find('\x07'), std::string::npos);
    EXPECT_EQ(table.find('\r'), std::string::npos);
}

TEST(ClusterSnapshotTest, ReadFromFileSkipsOverlongLineWithoutBufferingIt) {
    test_support::ScopedTempCwd cwd("snap-overlong");
    const std::string path = (cwd.path() / "s.jsonl").string();
    ClusterSnapshot ok("ok", 1);
    ok.record(NodeSnapshot{.host = "h", .status = "exited"});
    ASSERT_TRUE(ok.appendToFile(path));
    {
        std::ofstream out(path, std::ios::app);
        out << std::string(5U * 1024U * 1024U, 'x') << '\n'; // > 4 MiB cap
    }
    ASSERT_TRUE(ok.appendToFile(path));
    auto list = ClusterSnapshot::readFromFile(path);
    ASSERT_TRUE(list.ok());
    EXPECT_EQ(list.value().size(), 2U);
}

TEST(ClusterSnapshotTest, NewSnapshotFileIsOwnerOnly) {
    test_support::ScopedTempCwd cwd("snap-perms");
    const auto path = cwd.path() / "s.jsonl";
    ClusterSnapshot snap("r", 1);
    ASSERT_TRUE(snap.appendToFile(path.string()));
    const auto perms = std::filesystem::status(path).permissions();
    using std::filesystem::perms;
    EXPECT_EQ(perms & (perms::group_all | perms::others_all), perms::none);
}

