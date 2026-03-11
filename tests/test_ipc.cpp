#include <gtest/gtest.h>
#include "ipc_engine.h"

class PipeTest : public ::testing::Test {};

TEST_F(PipeTest, PipeWriteRead) {
    Pipe pipe;
    const char* msg = "HelloPipe";
    ASSERT_EQ(pipe.write(msg, strlen(msg)), strlen(msg));
    char buf[32] = {};
    ASSERT_GT(pipe.read(buf, sizeof(buf)), 0);
    ASSERT_STREQ(buf, msg);
}

TEST_F(PipeTest, NonBlockingReadWrite) {
    Pipe pipe;
    pipe.setNonBlocking(true);
    const char* msg = "NonBlocking";
    ASSERT_EQ(pipe.write(msg, strlen(msg)), strlen(msg));
    char buf[32] = {};
    ASSERT_GT(pipe.read(buf, sizeof(buf)), 0);
    ASSERT_STREQ(buf, msg);
}

TEST_F(PipeTest, PipeErrorHandling) {
    Pipe pipe;
    close(pipe.getReadFd());
    char buf[8];
    ASSERT_EQ(pipe.read(buf, sizeof(buf)), -1);
}