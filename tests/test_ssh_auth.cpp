#include <gtest/gtest.h>

#include "ssh_auth.hpp"

TEST(SshAuthTest, BuildsBaseArgumentsForSystemSshClient) {
    ClientEntry client;
    client.user = "admin";
    client.host = "server.example.com";
    client.port = 2222;
    client.identityFile = "/home/admin/.ssh/id_ed25519";

    const auto args = buildSshBaseArguments(client);

    ASSERT_GE(args.size(), 8U);
    EXPECT_EQ(args.front(), "/usr/bin/ssh");
    EXPECT_NE(std::find(args.begin(), args.end(), "StrictHostKeyChecking=no"), args.end());
    EXPECT_NE(std::find(args.begin(), args.end(), "ConnectTimeout=5"), args.end());
    EXPECT_NE(std::find(args.begin(), args.end(), "-p"), args.end());
    EXPECT_NE(std::find(args.begin(), args.end(), "2222"), args.end());
    EXPECT_NE(std::find(args.begin(), args.end(), "-i"), args.end());
    EXPECT_EQ(args.back(), client.sshTarget());
}

TEST(SshAuthTest, PrependsSshpassWhenPasswordExists) {
    ClientEntry client;
    client.user = "admin";
    client.host = "server.example.com";
    client.password = "secret123";

    const auto args = buildSshCommandArguments(client, "hostname");

    ASSERT_GE(args.size(), 8U);
    EXPECT_EQ(args[0], "sshpass");
    EXPECT_EQ(args[1], "-p");
    EXPECT_EQ(args[2], "secret123");
    EXPECT_EQ(args[3], "/usr/bin/ssh");
    EXPECT_EQ(args.back(), "hostname");
}

TEST(SshAuthTest, DetectsCommonAuthenticationFailures) {
    EXPECT_TRUE(isSshAuthenticationFailure("Permission denied (publickey,password)."));
    EXPECT_TRUE(isSshAuthenticationFailure("No more authentication methods available"));
    EXPECT_FALSE(isSshAuthenticationFailure("Connection refused"));
}
