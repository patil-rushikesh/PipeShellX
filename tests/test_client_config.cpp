#include <gtest/gtest.h>

#include "client_config.hpp"

TEST(ClientConfigTest, ParsesPasswordFromSshUrl) {
    EXPECT_THROW(
        static_cast<void>(ClientConfig::parseEntry(
            "ssh://admin@server.example.com:2222?identity=/home/admin/.ssh/id_ed25519&password=secret123"
        )),
        std::runtime_error
    );
}

TEST(ClientConfigTest, SerializesPasswordWhenPresent) {
    ClientEntry entry;
    entry.user = "admin";
    entry.host = "server.example.com";
    entry.password = "secret123";

    EXPECT_EQ(entry.serialize(), "admin@server.example.com");
}
