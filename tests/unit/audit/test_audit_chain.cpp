#include "psx/audit/audit_log.hpp"
#include "psx/audit/sha256.hpp"
#include "psx/cli/audit_command.hpp"

#include "test_support.hpp"

#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <sstream>
#include <string>
#include <vector>

#if defined(PIPESHELLX_HAVE_TLS)
#include <openssl/evp.h>
#endif

using psx::audit::AuditLog;
using psx::audit::sha256Hex;
using psx::audit::verifyAuditLog;

namespace {

std::vector<std::string> readLines(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    std::vector<std::string> lines;
    std::string line;
    while (std::getline(in, line)) {
        lines.push_back(line);
    }
    return lines;
}
void writeLines(const std::string& path, const std::vector<std::string>& lines) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    for (const auto& l : lines) {
        out << l << '\n';
    }
}
void writeSampleRun(const std::string& path) {
    AuditLog audit(path);
    ASSERT_TRUE(audit.ok());
    audit.runStarted("r1", "uptime", 2);
    audit.stageFinished("r1", {.host = "h1", .stageId = "s0", .exitCode = 0});
    audit.stageFinished("r1", {.host = "h2", .stageId = "s1", .exitCode = 1, .error = "boom"});
    audit.runFinished("r1", {.total = 2, .succeeded = 1, .failed = 1});
}

} // namespace

// ---------------------------------------------------------------- SHA-256

TEST(Sha256Test, NistVectors) {
    EXPECT_EQ(sha256Hex(""), "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    EXPECT_EQ(sha256Hex("abc"), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    EXPECT_EQ(sha256Hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"),
              "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    EXPECT_EQ(sha256Hex(std::string(1000000, 'a')), "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

TEST(Sha256Test, IncrementalUpdatesMatchOneShot) {
    psx::audit::Sha256 s;
    s.update("ab");
    s.update("");
    s.update("c");
    const auto d = s.finish();
    std::string hex;
    for (auto b : d) {
        static const char* k = "0123456789abcdef";
        hex += k[b >> 4];
        hex += k[b & 15];
    }
    EXPECT_EQ(hex, sha256Hex("abc"));
}

#if defined(PIPESHELLX_HAVE_TLS)
// Cross-check every length across the padding boundaries against OpenSSL.
TEST(Sha256Test, MatchesOpenSslForEveryLengthUpTo200) {
    std::string data;
    for (int len = 0; len <= 200; ++len) {
        unsigned char md[32];
        unsigned int mdLen = 0;
        ASSERT_EQ(EVP_Digest(data.data(), data.size(), md, &mdLen, EVP_sha256(), nullptr), 1);
        std::string expect;
        for (unsigned i = 0; i < mdLen; ++i) {
            static const char* k = "0123456789abcdef";
            expect += k[md[i] >> 4];
            expect += k[md[i] & 15];
        }
        EXPECT_EQ(sha256Hex(data), expect) << "length " << len;
        data.push_back(static_cast<char>('A' + len % 26));
    }
}
#endif

// ------------------------------------------------------------- hash chain

TEST(AuditChainTest, FreshLogVerifiesAndLinksEveryRecord) {
    test_support::ScopedTempCwd cwd("audit-chain");
    const std::string path = (cwd.path() / "a.jsonl").string();
    writeSampleRun(path);
    auto r = verifyAuditLog(path);
    ASSERT_TRUE(r.ok());
    EXPECT_TRUE(r.value().intact) << r.value().reason;
    EXPECT_EQ(r.value().records, 4U);
    const auto lines = readLines(path);
    EXPECT_NE(lines[0].find(R"("prev":"0000000000000000000000000000000000000000000000000000000000000000")"),
              std::string::npos);
    EXPECT_NE(lines.back().find(r.value().head), std::string::npos);
}

TEST(AuditChainTest, ChainContinuesAcrossReopen) {
    test_support::ScopedTempCwd cwd("audit-reopen");
    const std::string path = (cwd.path() / "a.jsonl").string();
    std::string head1;
    {
        AuditLog a(path);
        a.runStarted("r1", "x", 1);
        head1 = a.head();
    }
    {
        AuditLog b(path);
        EXPECT_EQ(b.head(), head1) << "reopen did not pick up the chain";
        b.runStarted("r2", "y", 1);
    }
    auto r = verifyAuditLog(path);
    ASSERT_TRUE(r.ok());
    EXPECT_TRUE(r.value().intact) << r.value().reason;
    EXPECT_EQ(r.value().records, 2U);
}

TEST(AuditChainTest, ModifiedRecordIsDetected) {
    test_support::ScopedTempCwd cwd("audit-modify");
    const std::string path = (cwd.path() / "a.jsonl").string();
    writeSampleRun(path);
    auto lines = readLines(path);
    const auto pos = lines[2].find("\"exit_code\":1");
    ASSERT_NE(pos, std::string::npos);
    lines[2].replace(pos, 13, "\"exit_code\":0"); // hide the failure
    writeLines(path, lines);
    auto r = verifyAuditLog(path);
    ASSERT_TRUE(r.ok());
    EXPECT_FALSE(r.value().intact);
    EXPECT_EQ(r.value().brokenLine, 3U);
}

TEST(AuditChainTest, DeletedInsertedAndReorderedRecordsAreDetected) {
    test_support::ScopedTempCwd cwd("audit-structure");
    const std::string path = (cwd.path() / "a.jsonl").string();
    writeSampleRun(path);
    const auto original = readLines(path);

    auto deleted = original;
    deleted.erase(deleted.begin() + 1);
    writeLines(path, deleted);
    EXPECT_EQ(verifyAuditLog(path).value().brokenLine, 2U);

    auto swapped = original;
    std::swap(swapped[1], swapped[2]);
    writeLines(path, swapped);
    EXPECT_EQ(verifyAuditLog(path).value().brokenLine, 2U);

    auto inserted = original;
    inserted.insert(inserted.begin() + 2, original[1]); // replay a valid record
    writeLines(path, inserted);
    EXPECT_EQ(verifyAuditLog(path).value().brokenLine, 3U);

    auto forged = original;
    forged.insert(forged.begin() + 2, R"({"ts_ms":1,"event":"stage_finished","host":"h9"})");
    writeLines(path, forged);
    EXPECT_EQ(verifyAuditLog(path).value().brokenLine, 3U);
}

// The documented limit: cutting records off the end leaves a valid chain.
// Only an externally kept head catches it.
TEST(AuditChainTest, TruncationNeedsAnAnchoredHeadToBeDetected) {
    test_support::ScopedTempCwd cwd("audit-truncate");
    const std::string path = (cwd.path() / "a.jsonl").string();
    writeSampleRun(path);
    const std::string anchoredHead = verifyAuditLog(path).value().head;
    auto lines = readLines(path);
    lines.pop_back();
    writeLines(path, lines);
    EXPECT_TRUE(verifyAuditLog(path).value().intact) << "a truncated chain still verifies on its own";

    std::ostringstream out;
    std::ostringstream err;
    EXPECT_EQ(psx::cli::runAuditCommand({"verify", path, "--expect-head", anchoredHead}, out, err), 1);
    EXPECT_NE(out.str().find("HEAD MISMATCH"), std::string::npos);
}

TEST(AuditChainTest, LegacyPrefixIsAcceptedButUnchainedLinesInsideTheChainAreNot) {
    test_support::ScopedTempCwd cwd("audit-legacy");
    const std::string path = (cwd.path() / "a.jsonl").string();
    writeLines(path, {R"({"ts_ms":1,"event":"run_started"})", R"({"ts_ms":2,"event":"run_finished"})"});
    {
        AuditLog a(path); // continues from the legacy tail
        a.runStarted("r", "x", 1);
    }
    auto r = verifyAuditLog(path);
    ASSERT_TRUE(r.ok());
    EXPECT_TRUE(r.value().intact) << r.value().reason;
    EXPECT_EQ(r.value().legacyRecords, 2U);
    EXPECT_EQ(r.value().records, 1U);

    auto lines = readLines(path);
    lines.push_back(R"({"ts_ms":9,"event":"run_started"})");
    writeLines(path, lines);
    EXPECT_FALSE(verifyAuditLog(path).value().intact) << "an unchained append after the chain was accepted";
}

TEST(AuditChainTest, PartialLastRecordIsTerminatedAndReported) {
    test_support::ScopedTempCwd cwd("audit-partial");
    const std::string path = (cwd.path() / "a.jsonl").string();
    writeSampleRun(path);
    {
        std::ofstream out(path, std::ios::app | std::ios::binary);
        out << R"({"ts_ms":5,"event":"stage_fin)"; // crash mid-write, no newline
    }
    {
        AuditLog a(path);
        a.runStarted("r2", "x", 1);
    }
    const auto lines = readLines(path);
    ASSERT_EQ(lines.size(), 6U) << "the new record was glued onto the partial one";
    auto r = verifyAuditLog(path);
    EXPECT_FALSE(r.value().intact);
    EXPECT_EQ(r.value().brokenLine, 5U);
}

TEST(AuditChainTest, NewLogIsOwnerOnly) {
    test_support::ScopedTempCwd cwd("audit-perms");
    const auto path = cwd.path() / "a.jsonl";
    { AuditLog a(path.string()); }
    using std::filesystem::perms;
    EXPECT_EQ(std::filesystem::status(path).permissions() & (perms::group_all | perms::others_all), perms::none);
}

TEST(AuditCommandTest, VerifyReportsOkBrokenAndUsage) {
    test_support::ScopedTempCwd cwd("audit-cli");
    const std::string path = (cwd.path() / "a.jsonl").string();
    writeSampleRun(path);
    std::ostringstream out;
    std::ostringstream err;
    EXPECT_EQ(psx::cli::runAuditCommand({"verify", path}, out, err), 0);
    EXPECT_NE(out.str().find("OK: hash chain intact, 4"), std::string::npos) << out.str();

    auto lines = readLines(path);
    lines[1][lines[1].find("h1")] = 'X';
    writeLines(path, lines);
    std::ostringstream out2;
    EXPECT_EQ(psx::cli::runAuditCommand({"verify", path}, out2, err), 1);
    EXPECT_NE(out2.str().find("BROKEN at line 2"), std::string::npos) << out2.str();

    std::ostringstream o;
    std::ostringstream e;
    EXPECT_EQ(psx::cli::runAuditCommand({}, o, e), 2);
    EXPECT_EQ(psx::cli::runAuditCommand({"verify"}, o, e), 2);
    EXPECT_EQ(psx::cli::runAuditCommand({"verify", path, "--expect-head", "nothex"}, o, e), 2);
    EXPECT_EQ(psx::cli::runAuditCommand({"verify", (cwd.path() / "missing").string()}, o, e), 2);
}
