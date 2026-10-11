#include <gtest/gtest.h>

#include "psx/cli/run_command.hpp"

#include "psx/stream/bounded_buffer.hpp"

#include <stdexcept>
#include <utility>
#include <vector>

using psx::cli::parseRun;
using psx::cli::SelectorKind;
using psx::cli::SinkMode;

TEST(ParseRunTest, DefaultsToAllHostsGroupSinkAndTheCommandAfterDashDash) {
    const auto inv = parseRun({"--", "uptime"});
    EXPECT_TRUE(inv.inventoryPath.empty());
    EXPECT_EQ(inv.selector.kind, SelectorKind::All);
    EXPECT_EQ(inv.sink, SinkMode::Group);
    EXPECT_EQ(inv.command, (std::vector<std::string>{"uptime"}));
    EXPECT_EQ(inv.timeoutSec, 0);
}

TEST(ParseRunTest, CommandKeepsItsOwnFlagsAfterDashDash) {
    const auto inv = parseRun({"-g", "web", "--stream", "--", "tail", "-F", "/var/log/x"});
    EXPECT_EQ(inv.selector.kind, SelectorKind::Group);
    EXPECT_EQ(inv.selector.value, "web");
    EXPECT_EQ(inv.sink, SinkMode::Stream);
    EXPECT_EQ(inv.command, (std::vector<std::string>{"tail", "-F", "/var/log/x"}));
}

TEST(ParseRunTest, SelectorsAreMutuallyExclusive) {
    EXPECT_EQ(parseRun({"-t", "canary", "--", "id"}).selector.kind, SelectorKind::Tag);
    EXPECT_EQ(parseRun({"-t", "canary", "--", "id"}).selector.value, "canary");
    const auto byHost = parseRun({"-H", "h1,h2,h3", "--", "id"});
    EXPECT_EQ(byHost.selector.kind, SelectorKind::Hosts);
    EXPECT_EQ(byHost.selector.hosts, (std::vector<std::string>{"h1", "h2", "h3"}));

    EXPECT_THROW(static_cast<void>(parseRun({"-g", "a", "-t", "b", "--", "id"})), std::runtime_error);
    EXPECT_THROW(static_cast<void>(parseRun({"-g", "a", "-H", "h", "--", "id"})), std::runtime_error);
}

TEST(ParseRunTest, SinkModesAndInventoryAndTimeout) {
    EXPECT_EQ(parseRun({"--json", "--", "id"}).sink, SinkMode::Json);
    EXPECT_EQ(parseRun({"--group", "--", "id"}).sink, SinkMode::Group);
    const auto inv = parseRun({"-i", "/etc/fleet.ini", "--timeout", "30", "--", "id"});
    EXPECT_EQ(inv.inventoryPath, "/etc/fleet.ini");
    EXPECT_EQ(inv.timeoutSec, 30);
    EXPECT_THROW(static_cast<void>(parseRun({"--stream", "--json", "--", "id"})), std::runtime_error); // one sink only
}

TEST(ParseRunTest, ConsensusOrderedFailureAndRetryFlags) {
    const auto defaults = parseRun({"--", "id"});
    EXPECT_EQ(defaults.sink, SinkMode::Group);
    EXPECT_FALSE(defaults.consensusJson);
    EXPECT_FALSE(defaults.ordered);
    EXPECT_FALSE(defaults.failFast);
    EXPECT_FALSE(defaults.idempotent);

    EXPECT_EQ(parseRun({"--consensus", "--", "id"}).sink, SinkMode::Consensus);
    const auto consensusJson = parseRun({"--consensus", "--json", "--", "id"});
    EXPECT_EQ(consensusJson.sink, SinkMode::Consensus);
    EXPECT_TRUE(consensusJson.consensusJson);
    EXPECT_TRUE(parseRun({"--ordered", "--", "id"}).ordered);
    EXPECT_TRUE(parseRun({"--fail-fast", "--", "id"}).failFast);
    EXPECT_TRUE(parseRun({"--idempotent", "--retries", "2", "--", "id"}).idempotent);
    EXPECT_EQ(parseRun({"--idempotent", "--retries", "2", "--", "id"}).retries, 2);

    EXPECT_THROW(static_cast<void>(parseRun({"--consensus", "--stream", "--", "id"})), std::runtime_error);
    EXPECT_THROW(static_cast<void>(parseRun({"--consensus", "--group", "--", "id"})), std::runtime_error);
    EXPECT_THROW(static_cast<void>(parseRun({"--consensus", "--json", "--stream", "--", "id"})), std::runtime_error);
}

TEST(ParseRunTest, PolicyAndRingFlags) {
    using psx::stream::OverflowPolicy;
    EXPECT_EQ(psx::cli::parseRun({"--", "id"}).policy, OverflowPolicy::Block);
    EXPECT_EQ(psx::cli::parseRun({"--", "id"}).ringBytes, 0U);
    EXPECT_EQ(psx::cli::parseRun({"--overflow", "drop-oldest", "--", "id"}).policy, OverflowPolicy::DropOldest);
    EXPECT_EQ(psx::cli::parseRun({"--overflow", "drop-newest", "--", "id"}).policy, OverflowPolicy::DropNewest);
    EXPECT_EQ(psx::cli::parseRun({"--ring", "1MiB", "--", "id"}).ringBytes, 1024U * 1024);
    EXPECT_EQ(psx::cli::parseRun({"--ring", "256KiB", "--", "id"}).ringBytes, 256U * 1024);
    EXPECT_EQ(psx::cli::parseRun({"--ring", "2M", "--", "id"}).ringBytes, 2U * 1024 * 1024);
    EXPECT_EQ(psx::cli::parseRun({"--ring", "4096", "--", "id"}).ringBytes, 4096U);
    EXPECT_THROW(static_cast<void>(psx::cli::parseRun({"--overflow", "nope", "--", "id"})), std::runtime_error);
    EXPECT_THROW(static_cast<void>(psx::cli::parseRun({"--ring", "1XB", "--", "id"})), std::runtime_error);
    EXPECT_THROW(static_cast<void>(psx::cli::parseRun({"--ring", "big", "--", "id"})), std::runtime_error);
    EXPECT_THROW(static_cast<void>(psx::cli::parseRun({"--ring", "999999999999999999999999999999", "--", "id"})),
                 std::runtime_error);
    EXPECT_THROW(static_cast<void>(psx::cli::parseRun({"--ring", "18446744073709551615G", "--", "id"})),
                 std::runtime_error);
}

TEST(ParseRunTest, ConcurrencyFlag) {
    EXPECT_EQ(psx::cli::parseRun({"--", "id"}).concurrency, 64);
    EXPECT_EQ(psx::cli::parseRun({"-c", "8", "--", "id"}).concurrency, 8);
    EXPECT_EQ(psx::cli::parseRun({"--concurrency", "256", "--", "id"}).concurrency, 256);
    EXPECT_EQ(psx::cli::parseRun({"-c", "0", "--", "id"}).concurrency, 0); // 0 = all at once
    EXPECT_THROW(static_cast<void>(psx::cli::parseRun({"-c", "x", "--", "id"})), std::runtime_error);
}

TEST(ParseRunTest, PolicyFileFlag) {
    EXPECT_TRUE(psx::cli::parseRun({"--", "id"}).policyPath.empty());
    EXPECT_EQ(psx::cli::parseRun({"--policy", "/etc/psx.policy", "--", "id"}).policyPath, "/etc/psx.policy");
    EXPECT_THROW(static_cast<void>(psx::cli::parseRun({"--policy", "--", "id"})), std::runtime_error);
}

TEST(ParseRunTest, SnapshotFileFlagIsParsed) {
    EXPECT_TRUE(psx::cli::parseRun({"--", "id"}).snapshotPath.empty());
    EXPECT_EQ(psx::cli::parseRun({"--snapshot-file", "/var/log/snap.jsonl", "--", "id"}).snapshotPath,
              "/var/log/snap.jsonl");
    EXPECT_THROW(static_cast<void>(psx::cli::parseRun({"--snapshot-file", "--", "id"})), std::runtime_error);
}

TEST(ParseRunTest, TransportNativeAndItsFlags) {
    const auto defaultTransport = psx::cli::parseRun({"--", "id"});
    EXPECT_FALSE(defaultTransport.native);
    EXPECT_FALSE(defaultTransport.transportExplicit);
    const auto ssh = psx::cli::parseRun({"--transport", "ssh", "--", "id"});
    EXPECT_FALSE(ssh.native);
    EXPECT_TRUE(ssh.transportExplicit);
    const auto n = psx::cli::parseRun(
        {"--transport", "native", "--cert", "c", "--key", "k", "--ca", "a", "--native-port", "9000", "--", "id"});
    EXPECT_TRUE(n.native);
    EXPECT_TRUE(n.transportExplicit);
    EXPECT_EQ(n.certPath, "c");
    EXPECT_EQ(n.keyPath, "k");
    EXPECT_EQ(n.caPath, "a");
    EXPECT_EQ(n.nativePort, 9000);
    EXPECT_THROW(psx::cli::parseRun({"--transport", "bogus", "--", "id"}), psx::cli::CliError);
}

TEST(ParseRunTest, NativeRejectsSshOnlyExecutionOptions) {
    const std::vector<std::vector<std::string>> cases = {
        {"--transport", "native", "--reuse", "--", "id"},
        {"--reuse", "--transport", "native", "--", "id"},
        {"--transport", "native", "--idempotent", "--", "id"},
        {"--transport", "native", "--retries", "0", "--", "id"},
        {"--transport", "native", "--retries", "2", "--", "id"},
        {"--transport", "native", "--shell", "posix", "--", "id"},
        {"--shell", "cmd", "--transport", "native", "--", "id"},
    };
    for (const auto& args : cases) {
        EXPECT_THROW(static_cast<void>(parseRun(args)), psx::cli::CliError);
    }
}

TEST(ParseRunTest, ExplicitSshRejectsNativeOnlyExecutionOptions) {
    const std::vector<std::vector<std::string>> cases = {
        {"--transport", "ssh", "--canary", "1", "--", "id"},
        {"--transport", "ssh", "--cert", "c", "--", "id"},
        {"--transport", "ssh", "--key", "k", "--", "id"},
        {"--transport", "ssh", "--ca", "a", "--", "id"},
        {"--transport", "ssh", "--crl", "r", "--", "id"},
        {"--transport", "ssh", "--native-port", "7433", "--", "id"},
    };
    for (const auto& args : cases) {
        EXPECT_THROW(static_cast<void>(parseRun(args)), psx::cli::CliError);
    }
}

TEST(ParseRunTest, ShellSelectsTheRemoteQuoting) {
    EXPECT_EQ(psx::cli::parseRun({"--", "id"}).shell, RemoteShell::Posix); // default
    EXPECT_EQ(psx::cli::parseRun({"--shell", "cmd", "--", "id"}).shell, RemoteShell::Cmd);
    EXPECT_EQ(psx::cli::parseRun({"--shell", "powershell", "--", "id"}).shell, RemoteShell::PowerShell);
    EXPECT_EQ(psx::cli::parseRun({"--shell", "pwsh", "--", "id"}).shell, RemoteShell::PowerShell);
    EXPECT_THROW(psx::cli::parseRun({"--shell", "bash", "--", "id"}), psx::cli::CliError);
}

TEST(ParseRunTest, OverflowSpoolIsAccepted) {
    EXPECT_EQ(psx::cli::parseRun({"--overflow", "spool", "--", "id"}).policy, psx::stream::OverflowPolicy::Spool);
    EXPECT_THROW(psx::cli::parseRun({"--overflow", "bogus", "--", "id"}), psx::cli::CliError);
}

TEST(ParseRunTest, ReuseFlag) {
    EXPECT_FALSE(psx::cli::parseRun({"--", "id"}).reuse);
    EXPECT_TRUE(psx::cli::parseRun({"--reuse", "--", "id"}).reuse);
}

TEST(ParseRunTest, NoColorIsHonoured) {
    EXPECT_FALSE(parseRun({"--no-color", "--stream", "--", "id"}).colour);
    EXPECT_TRUE(parseRun({"--stream", "--", "id"}).colour);
}

TEST(ParseRunTest, RejectsMissingCommandOrValues) {
    EXPECT_THROW(static_cast<void>(parseRun({"-g", "web"})), std::runtime_error);      // no --
    EXPECT_THROW(static_cast<void>(parseRun({"--"})), std::runtime_error);             // empty command
    EXPECT_THROW(static_cast<void>(parseRun({"-g", "--", "id"})), std::runtime_error); // -g needs a value
    EXPECT_THROW(static_cast<void>(parseRun({"--timeout", "x", "--", "id"})), std::runtime_error);
    EXPECT_THROW(static_cast<void>(parseRun({"--bogus", "--", "id"})), std::runtime_error);
}

TEST(ParseRunTest, LongFormSelectorsAlsoWork) {
    EXPECT_EQ(parseRun({"--group-name", "web", "--", "id"}).selector.value, "web");
    EXPECT_EQ(parseRun({"--tag", "canary", "--", "id"}).selector.kind, SelectorKind::Tag);
    EXPECT_EQ(parseRun({"--hosts", "a,b", "--", "id"}).selector.hosts, (std::vector<std::string>{"a", "b"}));
}

// ---- end-to-end: runSubcommand over a fake ssh + an inventory file ----

#include "psx/cli/hosts_command.hpp"
#include "psx/cli/selection.hpp"
#include "test_support.hpp"

#include <filesystem>
#include <fstream>
#include <sstream>

TEST(RunSubcommandTest, RunsTheCommandOnSelectedHostsAndRendersGroup) {
    test_support::ScopedTempCwd cwd("run-e2e");
    test_support::FakeSshOnPath fakeSsh;
    {
        std::ofstream ini("fleet.ini");
        ini << "[web]\nh1 user=alice\nh2 user=bob\n[db]\nh3 user=carol\n";
    }

    psx::cli::RunInvocation inv;
    inv.inventoryPath = "fleet.ini";
    inv.selector.kind = psx::cli::SelectorKind::Group;
    inv.selector.value = "web";
    inv.sink = psx::cli::SinkMode::Group;
    inv.command = {"ok"}; // the fake ssh echoes host=<target> for the `ok` command

    std::ostringstream out;
    std::ostringstream err;
    const int code = psx::cli::runSubcommand(inv, out, err, false);
    EXPECT_EQ(code, 0) << err.str();
    // Only the two web hosts ran, grouped.
    EXPECT_NE(out.str().find("CLIENT alice@h1"), std::string::npos) << out.str();
    EXPECT_NE(out.str().find("CLIENT bob@h2"), std::string::npos) << out.str();
    EXPECT_EQ(out.str().find("carol"), std::string::npos) << "db host must not run";
}

TEST(RunSubcommandTest, BufferedJsonRendersOnlyTheDropPolicyCapture) {
    test_support::ScopedTempCwd cwd("run-bounded-json");
    test_support::FakeSshOnPath fakeSsh;
    {
        std::ofstream ini("fleet.ini");
        ini << "[all]\nh1 user=alice\n";
    }

    psx::cli::RunInvocation inv;
    inv.inventoryPath = "fleet.ini";
    inv.sink = psx::cli::SinkMode::Json;
    inv.policy = psx::stream::OverflowPolicy::DropOldest;
    inv.ringBytes = 4;
    inv.command = {"big"};

    std::ostringstream out;
    std::ostringstream err;
    ASSERT_EQ(psx::cli::runSubcommand(inv, out, err, false), 0) << err.str();
    EXPECT_NE(out.str().find("\"stdout\":\"oooo\""), std::string::npos) << out.str();
    EXPECT_NE(out.str().find("\"stderr\":\"eeee\""), std::string::npos) << out.str();
    EXPECT_NE(out.str().find("\"dropped\":299992"), std::string::npos) << out.str();
    EXPECT_LT(out.str().size(), 1024U) << "the buffering sink retained raw output outside the ring";
}

TEST(RunSubcommandTest, NoHostsSelectedIsExitCode3) {
    test_support::ScopedTempCwd cwd("run-none");
    test_support::FakeSshOnPath fakeSsh;
    {
        std::ofstream ini("fleet.ini");
        ini << "[web]\nh1\n";
    }
    psx::cli::RunInvocation inv;
    inv.inventoryPath = "fleet.ini";
    inv.selector.kind = psx::cli::SelectorKind::Group;
    inv.selector.value = "nonexistent";
    inv.command = {"ok"};
    std::ostringstream out, err;
    EXPECT_EQ(psx::cli::runSubcommand(inv, out, err, false), 3);
}

TEST(RunSubcommandTest, MissingInventoryIsExitCode2) {
    test_support::ScopedTempCwd cwd("run-noinv");
    psx::cli::RunInvocation inv;
    inv.command = {"id"};
    std::ostringstream out, err;
    EXPECT_EQ(psx::cli::runSubcommand(inv, out, err, false), 2);
    EXPECT_NE(err.str().find("no inventory"), std::string::npos) << err.str();
}

TEST(RunSubcommandTest, NativeRejectsSshOnlyOptionsBeforeInventoryLookup) {
    psx::cli::RunInvocation inv;
    inv.native = true;
    inv.reuse = true;
    inv.command = {"id"};

    std::ostringstream out, err;
    EXPECT_EQ(psx::cli::runSubcommand(inv, out, err, false), 2);
    EXPECT_NE(err.str().find("--reuse"), std::string::npos) << err.str();
    EXPECT_NE(err.str().find("ssh"), std::string::npos) << err.str();
    EXPECT_EQ(err.str().find("inventory"), std::string::npos) << err.str();
}

TEST(RunSubcommandTest, InventoryTransportIsHonoredAndMixedSelectionRequiresOverride) {
    test_support::ScopedTempCwd cwd("run-inventory-transport");
    test_support::FakeSshOnPath fakeSsh;
    {
        std::ofstream inventory("native.ini");
        inventory << "[all]\nnode-1 transport=native\n";
    }
    {
        std::ofstream inventory("mixed.ini");
        inventory << "[all]\nnode-1 transport=ssh\nnode-2 transport=native\n";
    }

    psx::cli::RunInvocation fromInventory;
    fromInventory.inventoryPath = "native.ini";
    fromInventory.command = {"ok"};
    std::ostringstream nativeOut, nativeErr;
    EXPECT_EQ(psx::cli::runSubcommand(fromInventory, nativeOut, nativeErr, false), 2);
#if defined(PIPESHELLX_HAVE_TLS)
    EXPECT_NE(nativeErr.str().find("requires --cert"), std::string::npos) << nativeErr.str();
#else
    EXPECT_NE(nativeErr.str().find("no native transport support"), std::string::npos) << nativeErr.str();
#endif

    fromInventory.inventoryPath = "mixed.ini";
    std::ostringstream mixedOut, mixedErr;
    EXPECT_EQ(psx::cli::runSubcommand(fromInventory, mixedOut, mixedErr, false), 2);
    EXPECT_NE(mixedErr.str().find("mixed ssh/native"), std::string::npos) << mixedErr.str();
    EXPECT_NE(mixedErr.str().find("--transport"), std::string::npos) << mixedErr.str();
}

TEST(RunSubcommandTest, ExplicitTransportOverridesInventoryPreference) {
    test_support::ScopedTempCwd cwd("run-inventory-transport-override");
    test_support::FakeSshOnPath fakeSsh;
    {
        std::ofstream inventory("native.ini");
        inventory << "[all]\nnode-1 transport=native\n";
    }
    {
        std::ofstream inventory("ssh.ini");
        inventory << "[all]\nnode-1 transport=ssh\n";
    }

    psx::cli::RunInvocation forceSsh;
    forceSsh.inventoryPath = "native.ini";
    forceSsh.transportExplicit = true;
    forceSsh.native = false;
    forceSsh.command = {"ok"};
    std::ostringstream sshOut, sshErr;
    EXPECT_EQ(psx::cli::runSubcommand(forceSsh, sshOut, sshErr, false), 0) << sshErr.str();

    psx::cli::RunInvocation forceNative;
    forceNative.inventoryPath = "ssh.ini";
    forceNative.transportExplicit = true;
    forceNative.native = true;
    forceNative.command = {"ok"};
    std::ostringstream nativeOut, nativeErr;
    EXPECT_EQ(psx::cli::runSubcommand(forceNative, nativeOut, nativeErr, false), 2);
#if defined(PIPESHELLX_HAVE_TLS)
    EXPECT_NE(nativeErr.str().find("requires --cert"), std::string::npos) << nativeErr.str();
#else
    EXPECT_NE(nativeErr.str().find("no native transport support"), std::string::npos) << nativeErr.str();
#endif
}

TEST(RunSubcommandTest, InventoryDerivedNativeRejectsExplicitDefaultSshOptions) {
    test_support::ScopedTempCwd cwd("run-inventory-native-ssh-options");
    {
        std::ofstream inventory("fleet.ini");
        inventory << "[all]\nnode-1 transport=native\n";
    }

    const std::vector<std::pair<std::vector<std::string>, std::string>> cases = {
        {{"-i", "fleet.ini", "--retries", "0", "--", "id"}, "--retries"},
        {{"-i", "fleet.ini", "--shell", "posix", "--", "id"}, "--shell"},
    };
    for (const auto& [args, flag] : cases) {
        const auto invocation = parseRun(args);
        std::ostringstream out, err;
        EXPECT_EQ(psx::cli::runSubcommand(invocation, out, err, false), 2) << flag;
        EXPECT_NE(err.str().find(flag), std::string::npos) << err.str();
        EXPECT_NE(err.str().find("transport ssh"), std::string::npos) << err.str();
        EXPECT_EQ(err.str().find("requires --cert"), std::string::npos) << err.str();
    }
}

TEST(RunSubcommandTest, InventoryDerivedSshRejectsNativeOnlyOptions) {
    test_support::ScopedTempCwd cwd("run-inventory-ssh-native-options");
    test_support::FakeSshOnPath fakeSsh;
    {
        std::ofstream inventory("fleet.ini");
        inventory << "[all]\nnode-1 transport=ssh\n";
    }

    const std::vector<std::pair<std::vector<std::string>, std::string>> cases = {
        {{"-i", "fleet.ini", "--canary", "1", "--", "ok"}, "--canary"},
        {{"-i", "fleet.ini", "--cert", "c", "--", "ok"}, "--cert"},
        {{"-i", "fleet.ini", "--key", "k", "--", "ok"}, "--key"},
        {{"-i", "fleet.ini", "--ca", "a", "--", "ok"}, "--ca"},
        {{"-i", "fleet.ini", "--crl", "r", "--", "ok"}, "--crl"},
        {{"-i", "fleet.ini", "--native-port", "7433", "--", "ok"}, "--native-port"},
    };
    for (const auto& [args, flag] : cases) {
        const auto invocation = parseRun(args);
        std::ostringstream out, err;
        EXPECT_EQ(psx::cli::runSubcommand(invocation, out, err, false), 2) << flag;
        EXPECT_NE(err.str().find(flag), std::string::npos) << err.str();
        EXPECT_NE(err.str().find("transport native"), std::string::npos) << err.str();
        EXPECT_TRUE(out.str().empty());
    }
}

TEST(HostsSubcommandTest, ListsHostsWithGroupsAndTags) {
    test_support::ScopedTempCwd cwd("hosts-e2e");
    {
        std::ofstream ini("fleet.ini");
        ini << "[web]\nh1 tag=canary\nh2\n[db]\nh1\n";
    }
    std::ostringstream out, err;
    const int code = psx::cli::hostsSubcommand("fleet.ini", out, err);
    EXPECT_EQ(code, 0) << err.str();
    // h1 is in both groups and tagged canary; h2 only web.
    EXPECT_NE(out.str().find("h1"), std::string::npos) << out.str();
    EXPECT_NE(out.str().find("web"), std::string::npos) << out.str();
    EXPECT_NE(out.str().find("db"), std::string::npos) << out.str();
    EXPECT_NE(out.str().find("canary"), std::string::npos) << out.str();
    EXPECT_NE(out.str().find("HOST\tGROUPS\tTAGS\tTRANSPORT"), std::string::npos) << out.str();
}

TEST(InventoryResolutionTest, HonorsFlagEnvironmentProjectLegacyAndUserPrecedence) {
    test_support::ScopedTempCwd cwd("inventory-precedence");
    const auto userConfig = cwd.path() / "config" / "pipeshellx";
    std::filesystem::create_directories(userConfig);

    auto writeInventory = [](const std::filesystem::path& path, const std::string& host) {
        std::ofstream file(path);
        file << "[all]\n" << host << "\n";
    };
    auto resolvedHost = [](const std::string& explicitPath) {
        std::ostringstream err;
        const auto resolved = psx::cli::resolveHosts(explicitPath, {}, err);
        EXPECT_TRUE(resolved.ok()) << err.str();
        EXPECT_EQ(resolved.clients.size(), 1U) << err.str();
        return resolved;
    };

    writeInventory(userConfig / "inventory.ini", "user-host");
    writeInventory("inventory.ini", "project-host");
    writeInventory("env.ini", "env-host");
    writeInventory("flag.ini", "flag-host");
    {
        std::ofstream legacy("clients.txt");
        legacy << "legacy@legacy-host\n";
    }

    test_support::ScopedEnv xdg("XDG_CONFIG_HOME", (cwd.path() / "config").string());
    {
        test_support::ScopedEnv env("PIPESHELLX_INVENTORY", std::string("env.ini"));
        const auto fromFlag = resolvedHost("flag.ini");
        EXPECT_EQ(fromFlag.inventoryPath, "flag.ini");
        EXPECT_EQ(fromFlag.clients.front().host, "flag-host");

        const auto fromEnv = resolvedHost("");
        EXPECT_EQ(fromEnv.inventoryPath, "env.ini");
        EXPECT_EQ(fromEnv.clients.front().host, "env-host");
    }

    test_support::ScopedEnv noEnv("PIPESHELLX_INVENTORY", std::nullopt);
    const auto fromProject = resolvedHost("");
    EXPECT_EQ(fromProject.inventoryPath, "inventory.ini");
    EXPECT_EQ(fromProject.clients.front().host, "project-host");

    std::filesystem::remove("inventory.ini");
    const auto fromLegacy = resolvedHost("");
    EXPECT_EQ(fromLegacy.inventoryPath, "clients.txt");
    EXPECT_EQ(fromLegacy.clients.front().host, "legacy-host");

    std::filesystem::remove("clients.txt");
    const auto fromUser = resolvedHost("");
    EXPECT_EQ(fromUser.inventoryPath, (userConfig / "inventory.ini").string());
    EXPECT_EQ(fromUser.clients.front().host, "user-host");

    const auto homeConfig = cwd.path() / "home" / ".config" / "pipeshellx";
    std::filesystem::create_directories(homeConfig);
    writeInventory(homeConfig / "inventory.ini", "home-host");
    test_support::ScopedEnv noXdg("XDG_CONFIG_HOME", std::nullopt);
    test_support::ScopedEnv home("HOME", (cwd.path() / "home").string());
    const auto fromHome = resolvedHost("");
    EXPECT_EQ(fromHome.inventoryPath, (homeConfig / "inventory.ini").string());
    EXPECT_EQ(fromHome.clients.front().host, "home-host");
}

TEST(HostsSubcommandTest, AddRequiresExplicitInventoryAndRejectsDuplicatesWithoutRewriting) {
    test_support::ScopedTempCwd cwd("hosts-add");
    std::ostringstream out;
    std::ostringstream err;

    EXPECT_EQ(psx::cli::hostsSubcommand(std::vector<std::string>{"add", "node-1"}, out, err), 2);
    EXPECT_NE(err.str().find("explicit -i FILE"), std::string::npos) << err.str();
    EXPECT_FALSE(std::filesystem::exists("inventory.ini"));

    out.str({});
    err.str({});
    EXPECT_EQ(psx::cli::hostsSubcommand(std::vector<std::string>{"add", "alice:plaintext@node-1", "-i", "fleet.ini"},
                                        out, err),
              2);
    EXPECT_NE(err.str().find("user:password"), std::string::npos) << err.str();
    EXPECT_FALSE(std::filesystem::exists("fleet.ini"));

    out.str({});
    err.str({});
    EXPECT_EQ(psx::cli::hostsSubcommand(std::vector<std::string>{"add", "node-1", "-i", "clients.txt"}, out, err), 2);
    EXPECT_NE(err.str().find("legacy import source"), std::string::npos) << err.str();
    EXPECT_FALSE(std::filesystem::exists("clients.txt"));

    out.str({});
    err.str({});
    EXPECT_EQ(psx::cli::hostsSubcommand(std::vector<std::string>{"add", "node-1", "--group", "web", "--user", "deploy",
                                                                 "--transport", "native", "--native-port", "7433", "-i",
                                                                 "fleet.ini"},
                                        out, err),
              0)
        << err.str();
    const auto added = psx::inventory::Inventory::loadFromFile("fleet.ini");
    ASSERT_EQ(added.hosts().size(), 1U);
    EXPECT_EQ(added.hosts().front().name, "node-1");
    EXPECT_EQ(added.hosts().front().user, "deploy");
    EXPECT_EQ(added.hosts().front().transport, "native");
    EXPECT_EQ(added.hosts().front().nativePort, 7433);
    EXPECT_EQ(added.hosts().front().groups, (std::vector<std::string>{"web"}));

    std::ifstream beforeFile("fleet.ini");
    const std::string before((std::istreambuf_iterator<char>(beforeFile)), std::istreambuf_iterator<char>());
    out.str({});
    err.str({});
    EXPECT_EQ(psx::cli::hostsSubcommand(std::vector<std::string>{"-i", "fleet.ini", "add", "node-1"}, out, err), 2);
    EXPECT_NE(err.str().find("already exists"), std::string::npos) << err.str();
    std::ifstream afterFile("fleet.ini");
    const std::string after((std::istreambuf_iterator<char>(afterFile)), std::istreambuf_iterator<char>());
    EXPECT_EQ(after, before);
}

TEST(HostsSubcommandTest, RemoveIsAtomicAndAMissingHostLeavesTheInventoryUnchanged) {
    test_support::ScopedTempCwd cwd("hosts-remove");
    {
        std::ofstream inventory("fleet.ini");
        inventory << "[web]\nnode-1\nnode-2 transport=native\n";
    }

    std::ostringstream out;
    std::ostringstream err;
    EXPECT_EQ(psx::cli::hostsSubcommand(std::vector<std::string>{"remove", "node-1", "-i", "fleet.ini"}, out, err), 0)
        << err.str();
    const auto remaining = psx::inventory::Inventory::loadFromFile("fleet.ini");
    ASSERT_EQ(remaining.hosts().size(), 1U);
    EXPECT_EQ(remaining.hosts().front().name, "node-2");

    std::ifstream beforeFile("fleet.ini");
    const std::string before((std::istreambuf_iterator<char>(beforeFile)), std::istreambuf_iterator<char>());
    out.str({});
    err.str({});
    EXPECT_EQ(psx::cli::hostsSubcommand(std::vector<std::string>{"remove", "missing", "-i", "fleet.ini"}, out, err), 2);
    EXPECT_NE(err.str().find("not found"), std::string::npos) << err.str();
    std::ifstream afterFile("fleet.ini");
    const std::string after((std::istreambuf_iterator<char>(afterFile)), std::istreambuf_iterator<char>());
    EXPECT_EQ(after, before);
}

TEST(HostsSubcommandTest, ImportStripsSecretsAndRejectsDuplicatesAsOneAtomicOperation) {
    test_support::ScopedTempCwd cwd("hosts-import");
    {
        std::ofstream inventory("fleet.ini");
        inventory << "[existing]\nalready-here\n";
    }
    {
        std::ofstream clients("clients.txt");
        clients << "alice@node-1\nssh://bob@node-2:2222?password=do-not-write-me\n";
    }

    std::ostringstream out;
    std::ostringstream err;
    EXPECT_EQ(psx::cli::hostsSubcommand(std::vector<std::string>{"import", "clients.txt", "-i", "fleet.ini"}, out, err),
              0)
        << err.str();
    std::ifstream importedFile("fleet.ini");
    const std::string imported((std::istreambuf_iterator<char>(importedFile)), std::istreambuf_iterator<char>());
    EXPECT_NE(imported.find("node-1"), std::string::npos) << imported;
    EXPECT_NE(imported.find("node-2"), std::string::npos) << imported;
    EXPECT_EQ(imported.find("password"), std::string::npos) << imported;
    EXPECT_EQ(imported.find("do-not-write-me"), std::string::npos) << imported;

    out.str({});
    err.str({});
    EXPECT_EQ(psx::cli::hostsSubcommand(std::vector<std::string>{"import", "clients.txt", "-i", "fleet.ini"}, out, err),
              2);
    EXPECT_NE(err.str().find("already exists"), std::string::npos) << err.str();
    std::ifstream afterFile("fleet.ini");
    const std::string after((std::istreambuf_iterator<char>(afterFile)), std::istreambuf_iterator<char>());
    EXPECT_EQ(after, imported);
}

TEST(HostsSubcommandTest, InvalidGrammarAndUnsafeValuesReturnClearUsageErrors) {
    test_support::ScopedTempCwd cwd("hosts-invalid");
    struct InvalidCase {
        std::vector<std::string> args;
        std::string diagnostic;
    };
    const std::vector<InvalidCase> cases = {
        {{"unknown"}, "unknown action"},
        {{"list", "extra"}, "does not accept"},
        {{"-i"}, "requires a value"},
        {{"add", "node-1", "--transport", "telnet", "-i", "fleet.ini"}, "ssh or native"},
        {{"add", "node-1", "password=plaintext", "-i", "fleet.ini"}, "secret option"},
        {{"remove", "node-1", "extra", "-i", "fleet.ini"}, "exactly one HOST"},
        {{"import", "same.ini", "-i", "same.ini"}, "different files"},
    };
    for (const auto& testCase : cases) {
        std::ostringstream out;
        std::ostringstream err;
        EXPECT_EQ(psx::cli::hostsSubcommand(testCase.args, out, err), 2);
        EXPECT_TRUE(out.str().empty());
        EXPECT_NE(err.str().find(testCase.diagnostic), std::string::npos)
            << "diagnostic for case " << testCase.diagnostic << ": " << err.str();
    }
    EXPECT_FALSE(std::filesystem::exists("fleet.ini"));
}

TEST(RunSubcommandTest, PolicyFileRestrictsTheCommand) {
    test_support::ScopedTempCwd cwd("run-policy");
    test_support::FakeSshOnPath fakeSsh;
    {
        std::ofstream ini("fleet.ini");
        ini << "[web]\nu@h1\n";
    }
    {
        std::ofstream pol("policy.txt");
        pol << "allow echo\n";
    }
    // An allowed command runs.
    psx::cli::RunInvocation ok;
    ok.inventoryPath = "fleet.ini";
    ok.policyPath = "policy.txt";
    ok.command = {"echo", "hi"};
    std::ostringstream out1, err1;
    EXPECT_EQ(psx::cli::runSubcommand(ok, out1, err1, false), 0) << err1.str();

    // A disallowed command is rejected before anything runs (exit 2).
    psx::cli::RunInvocation bad = ok;
    bad.command = {"rm", "-rf", "/"};
    std::ostringstream out2, err2;
    EXPECT_EQ(psx::cli::runSubcommand(bad, out2, err2, false), 2);
    EXPECT_NE(err2.str().find("not allowed"), std::string::npos) << err2.str();
    EXPECT_TRUE(out2.str().empty()) << "nothing runs when the policy rejects";
}

TEST(RunSubcommandTest, MissingPolicyFileIsExitCode2) {
    test_support::ScopedTempCwd cwd("run-nopolicy");
    psx::cli::RunInvocation inv;
    inv.policyPath = "does-not-exist.txt";
    inv.command = {"id"};
    std::ostringstream out, err;
    EXPECT_EQ(psx::cli::runSubcommand(inv, out, err, false), 2);
    EXPECT_NE(err.str().find("cannot open"), std::string::npos) << err.str();
}

TEST(ParseRunTest, CanaryFlagCarriesTheSpecVerbatim) {
    EXPECT_TRUE(parseRun({"--", "id"}).canary.empty());
    EXPECT_EQ(parseRun({"--canary", "2", "--", "id"}).canary, "2");
    EXPECT_EQ(parseRun({"--canary", "10%", "--", "id"}).canary, "10%");
}

TEST(ParseRunTest, CanaryRejectsMalformedOrNonPositiveSpecs) {
    for (const std::string value : {"", "0", "0%", "-1", "-5%", "abc", "1.5", "10%%", "%", "2x"}) {
        EXPECT_THROW(static_cast<void>(parseRun({"--canary", value, "--", "id"})), psx::cli::CliError) << value;
    }
}

TEST(CanaryCountTest, CountsAndPercentagesClampIntoRange) {
    using psx::cli::canaryCount;
    // An explicit count, clamped to [1, total].
    EXPECT_EQ(canaryCount("3", 10), 3U);
    EXPECT_EQ(canaryCount("0", 10), 1U);    // never zero canaries when hosts exist
    EXPECT_EQ(canaryCount("200", 10), 10U); // never more than the whole fleet
    // Percentages round up so a small percent still exercises at least one host.
    EXPECT_EQ(canaryCount("50%", 10), 5U);
    EXPECT_EQ(canaryCount("5%", 10), 1U); // ceil(0.5) -> 1
    EXPECT_EQ(canaryCount("100%", 7), 7U);
    EXPECT_EQ(canaryCount("25%", 8), 2U);
    // A malformed spec falls back to a single canary host.
    EXPECT_EQ(canaryCount("abc", 10), 1U);
    // No hosts -> no canaries, regardless of spec.
    EXPECT_EQ(canaryCount("50%", 0), 0U);
}

#if defined(PIPESHELLX_HAVE_TLS)

#include "psx/ca/certificate_authority.hpp"
#include "psx/os/socket.hpp"
#include "psx/runtime/reactor.hpp"
#include "psx/transport/node_server.hpp"

#include <chrono>
#include <csignal>
#include <iterator>
#include <pthread.h>
#include <thread>

namespace {

class NativeRunServer {
public:
    NativeRunServer() {
        auto ca = psx::ca::CertificateAuthority::create("native-run-test");
        if (!ca.ok()) {
            throw std::runtime_error(ca.error().message());
        }
        auto node = ca.value().issue("psx://node/1");
        auto controller = ca.value().issue("psx://controller");
        if (!node.ok() || !controller.ok()) {
            throw std::runtime_error("could not issue native-run test identities");
        }
        caPem_ = ca.value().certificatePem();
        controllerCert_ = std::move(controller.value().certificatePem);
        controllerKey_ = std::move(controller.value().privateKeyPem);

        auto listener = psx::os::Socket::listen("127.0.0.1", 0);
        if (!listener.ok()) {
            throw std::runtime_error(listener.error().message());
        }
        auto localPort = listener.value().localPort();
        if (!localPort.ok()) {
            throw std::runtime_error(localPort.error().message());
        }
        port_ = localPort.value();

        auto reactor = psx::runtime::Reactor::create();
        if (!reactor.ok()) {
            throw std::runtime_error(reactor.error().message());
        }
        reactor_ = std::move(reactor.value());
        server_ = std::make_unique<psx::transport::NodeServer>(
            *reactor_, std::move(listener.value()),
            psx::os::TlsConfig{.certificatePem = node.value().certificatePem,
                               .privateKeyPem = node.value().privateKeyPem,
                               .caPem = caPem_,
                               .crlPem = {},
                               .isServer = true},
            [](std::string_view san) { return san == "psx://controller"; });
        if (auto started = server_->start(); !started.ok()) {
            throw std::runtime_error(started.error().message());
        }

        writeFile("controller.crt", controllerCert_);
        writeFile("controller.key", controllerKey_);
        writeFile("ca.crt", caPem_);
        std::ofstream inventory("fleet.ini");
        inventory << "[all]\n127.0.0.1 san=psx://node/1 native_port=" << port_ << "\n";

        thread_ = std::thread([this] {
            // Linux signalfd requires every pre-existing thread to block the
            // subscribed signals. The CLI reactor blocks them on its own thread.
            sigset_t set;
            sigemptyset(&set);
            sigaddset(&set, SIGINT);
            sigaddset(&set, SIGTERM);
            (void)::pthread_sigmask(SIG_BLOCK, &set, nullptr);
            (void)reactor_->run();
        });
    }

    ~NativeRunServer() {
        reactor_->stop();
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    psx::cli::RunInvocation invocation(std::vector<std::string> command) const {
        psx::cli::RunInvocation value;
        value.inventoryPath = "fleet.ini";
        value.native = true;
        value.transportExplicit = true;
        value.certPath = "controller.crt";
        value.keyPath = "controller.key";
        value.caPath = "ca.crt";
        value.sink = psx::cli::SinkMode::Json;
        value.command = std::move(command);
        return value;
    }

private:
    static void writeFile(const char* path, const std::string& contents) {
        std::ofstream file(path, std::ios::binary);
        file << contents;
    }

    std::string caPem_;
    std::string controllerCert_;
    std::string controllerKey_;
    std::uint16_t port_ = 0;
    std::unique_ptr<psx::runtime::Reactor> reactor_;
    std::unique_ptr<psx::transport::NodeServer> server_;
    std::thread thread_;
};

std::string readTextFile(const char* path) {
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

} // namespace

TEST(NativeRunSubcommandTest, ReportsBoundedLossAndWritesCompleteAuditLifecycle) {
    test_support::ScopedTempCwd cwd("native-run-audit");
    NativeRunServer server;
    auto invocation = server.invocation({"/bin/sh", "-c", "printf 0123456789; printf abcdef 1>&2"});
    invocation.policy = psx::stream::OverflowPolicy::DropOldest;
    invocation.ringBytes = 4;
    invocation.auditPath = "audit.jsonl";

    std::ostringstream out, err;
    EXPECT_EQ(psx::cli::runSubcommand(invocation, out, err, false), 0) << err.str();
    EXPECT_NE(out.str().find("\"dropped\":8"), std::string::npos) << out.str();
    EXPECT_NE(out.str().find("\"stdout\":\"6789\""), std::string::npos) << out.str();
    EXPECT_NE(out.str().find("\"stderr\":\"cdef\""), std::string::npos) << out.str();
    EXPECT_EQ(out.str().find("0123456789"), std::string::npos) << out.str();

    const std::string audit = readTextFile("audit.jsonl");
    EXPECT_NE(audit.find("\"event\":\"run_started\""), std::string::npos) << audit;
    EXPECT_NE(audit.find("\"event\":\"stage_finished\""), std::string::npos) << audit;
    EXPECT_NE(audit.find("\"dropped_bytes\":8"), std::string::npos) << audit;
    EXPECT_NE(audit.find("\"event\":\"run_finished\""), std::string::npos) << audit;
}

TEST(NativeRunSubcommandTest, TimeoutIsVisibleInSinkAndAudit) {
    test_support::ScopedTempCwd cwd("native-run-timeout");
    NativeRunServer server;
    auto invocation = server.invocation({"/bin/sh", "-c", "sleep 5"});
    invocation.timeoutSec = 1;
    invocation.auditPath = "audit.jsonl";

    std::ostringstream out, err;
    const auto started = std::chrono::steady_clock::now();
    EXPECT_EQ(psx::cli::runSubcommand(invocation, out, err, false), 1) << err.str();
    EXPECT_LT(std::chrono::steady_clock::now() - started, std::chrono::seconds(4));
    EXPECT_NE(out.str().find("\"timed_out\":true"), std::string::npos) << out.str();

    const std::string audit = readTextFile("audit.jsonl");
    EXPECT_NE(audit.find("\"timed_out\":true"), std::string::npos) << audit;
    EXPECT_NE(audit.find("\"exit_code\":1"), std::string::npos) << audit;
}

TEST(NativeRunSubcommandTest, InterruptCancelsTheRunAndReturns130) {
    test_support::ScopedTempCwd cwd("native-run-interrupt");
    NativeRunServer server;
    // The native child signals its parent (this test process) only after the
    // controller's SignalSource is live and the stage has opened.
    auto invocation = server.invocation({"/bin/sh", "-c", "kill -INT $PPID; sleep 5"});
    invocation.timeoutSec = 10;
    invocation.auditPath = "audit.jsonl";

    std::ostringstream out, err;
    const auto started = std::chrono::steady_clock::now();
    EXPECT_EQ(psx::cli::runSubcommand(invocation, out, err, false), 130) << err.str();
    EXPECT_LT(std::chrono::steady_clock::now() - started, std::chrono::seconds(4));
    EXPECT_NE(out.str().find("\"cancelled\":true"), std::string::npos) << out.str();

    const std::string audit = readTextFile("audit.jsonl");
    EXPECT_NE(audit.find("\"cancelled\":true"), std::string::npos) << audit;
    EXPECT_NE(audit.find("\"exit_code\":130"), std::string::npos) << audit;
}

#endif // PIPESHELLX_HAVE_TLS
