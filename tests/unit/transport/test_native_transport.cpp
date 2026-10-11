#include "psx/transport/native_transport.hpp"

#include "psx/ca/certificate_authority.hpp"
#include "psx/transport/native_controller.hpp"
#include "psx/transport/node_server.hpp"
#include "psx/transport/node_stage_runner.hpp"

#include "psx/pipeline/distributed_runner.hpp"
#include "psx/pipeline/fanin_pipeline.hpp"

#include "psx/pipeline/segmented_pipeline.hpp"
#include "psx/transport/canary_controller.hpp"

#include "psx/os/socket.hpp"
#include "psx/os/tls.hpp"
#include "psx/runtime/reactor.hpp"
#include "test_support.hpp"
#include "tls_certs.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <string>
#include <thread>
#include <vector>

using psx::os::ExitStatus;
using psx::os::Socket;
using psx::os::Tls;
using psx::runtime::Reactor;
using namespace psx::transport;

namespace psx::transport {
struct NodeServerLifecycleProbe {
    static void scheduleDeferredReap(NodeServer& server) { server.dropConnection(999); }
};
} // namespace psx::transport

namespace {

Socket acceptWithin(Socket& listener) {
    for (int i = 0; i < 2000; ++i) {
        auto a = listener.accept();
        if (a.ok()) {
            return std::move(a.value());
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return Socket{};
}

// The node side: on OPEN, "run" the stage — stream a line then exit 0.
struct NodeStage : SessionHandler {
    Session* session = nullptr;
    std::vector<OpenRequest> opens;
    void onOpen(StreamId id, const OpenRequest& request) override {
        opens.push_back(request);
        session->sendData(id, "output-line\n", /*endStream=*/false);
        session->sendExit(id, {ExitStatus::Kind::Exited, 0});
    }
};

// The controller side: collect the stage's output and exit.
struct ControllerSink : SessionHandler {
    Reactor* reactor = nullptr;
    std::string data;
    std::string stderrData;
    bool exited = false;
    ExitStatus status{};
    void onData(StreamId, std::string_view d, bool, Channel channel) override {
        (channel == Channel::Stderr ? stderrData : data).append(d);
    }
    void onExit(StreamId, const ExitStatus& s) override {
        exited = true;
        status = s;
        reactor->stop();
    }
};

} // namespace

TEST(NativeTransportTest, RunsAStageEndToEndOverMtls) {
    // Loopback TCP.
    auto listener = Socket::listen("127.0.0.1", 0);
    ASSERT_TRUE(listener.ok());
    const auto port = listener.value().localPort();
    ASSERT_TRUE(port.ok());
    auto clientSock = Socket::connect("127.0.0.1", port.value());
    ASSERT_TRUE(clientSock.ok());
    Socket serverSock = acceptWithin(listener.value());
    ASSERT_TRUE(serverSock.valid());

    // mTLS identities.
    const auto ctlId = tls_test::generateSelfSigned("psx://controller");
    const auto nodeId = tls_test::generateSelfSigned("psx://node/1");
    auto ctlTls = Tls::create(
        {.certificatePem = ctlId.certPem, .privateKeyPem = ctlId.keyPem, .caPem = nodeId.certPem, .isServer = false});
    auto nodeTls = Tls::create(
        {.certificatePem = nodeId.certPem, .privateKeyPem = nodeId.keyPem, .caPem = ctlId.certPem, .isServer = true});
    ASSERT_TRUE(ctlTls.ok());
    ASSERT_TRUE(nodeTls.ok());

    auto reactor = Reactor::create();
    ASSERT_TRUE(reactor.ok());
    Reactor& r = *reactor.value();

    NodeStage nodeStage;
    ControllerSink ctlSink;
    ctlSink.reactor = &r;

    bool ctlReady = false;
    bool errored = false;
    NativeTransport* ctlPtr = nullptr;

    NativeTransport node(r, std::move(serverSock), std::move(nodeTls.value()), Role::Node, nodeStage,
                         {.onError = [&](psx::Error) {
                             errored = true;
                             r.stop();
                         }});
    NativeTransport ctl(r, std::move(clientSock.value()), std::move(ctlTls.value()), Role::Controller, ctlSink,
                        {.onReady =
                             [&] {
                                 ctlReady = true;
                                 ctlPtr->session().open({.argv = {"echo", "hi"}, .cwd = "/tmp"});
                             },
                         .onError =
                             [&](psx::Error) {
                                 errored = true;
                                 r.stop();
                             }});
    nodeStage.session = &node.session();
    ctlPtr = &ctl;

    ASSERT_TRUE(node.start().ok());
    ASSERT_TRUE(ctl.start().ok());
    r.after(std::chrono::seconds(3), [&] { r.stop(); });
    ASSERT_TRUE(r.run().ok());

    EXPECT_FALSE(errored);
    EXPECT_TRUE(ctlReady);
    ASSERT_EQ(nodeStage.opens.size(), 1U);
    EXPECT_EQ(nodeStage.opens[0].argv, (std::vector<std::string>{"echo", "hi"}));
    EXPECT_EQ(ctlSink.data, "output-line\n");
    EXPECT_TRUE(ctlSink.exited);
    EXPECT_EQ(ctlSink.status.code, 0);
    // Identity check both directions.
    EXPECT_EQ(ctl.peerSanUri(), "psx://node/1");
    EXPECT_EQ(node.peerSanUri(), "psx://controller");
}

TEST(NodeStageRunnerTest, RunsARealCommandOverTheEncryptedBackplane) {
    auto listener = Socket::listen("127.0.0.1", 0);
    ASSERT_TRUE(listener.ok());
    auto clientSock = Socket::connect("127.0.0.1", listener.value().localPort().value());
    ASSERT_TRUE(clientSock.ok());
    Socket serverSock = acceptWithin(listener.value());
    ASSERT_TRUE(serverSock.valid());

    const auto ctlId = tls_test::generateSelfSigned("psx://controller");
    const auto nodeId = tls_test::generateSelfSigned("psx://node/1");
    auto ctlTls = Tls::create(
        {.certificatePem = ctlId.certPem, .privateKeyPem = ctlId.keyPem, .caPem = nodeId.certPem, .isServer = false});
    auto nodeTls = Tls::create(
        {.certificatePem = nodeId.certPem, .privateKeyPem = nodeId.keyPem, .caPem = ctlId.certPem, .isServer = true});
    ASSERT_TRUE(ctlTls.ok() && nodeTls.ok());

    auto reactor = Reactor::create();
    ASSERT_TRUE(reactor.ok());
    Reactor& r = *reactor.value();

    NodeStageRunner runner(r); // the node runs each opened stream as a local process
    ControllerSink sink;
    sink.reactor = &r;
    bool errored = false;
    NativeTransport* ctlPtr = nullptr;

    NativeTransport node(r, std::move(serverSock), std::move(nodeTls.value()), Role::Node, runner,
                         {.onError = [&](psx::Error) {
                             errored = true;
                             r.stop();
                         }});
    runner.bind(node.session());
    NativeTransport ctl(r, std::move(clientSock.value()), std::move(ctlTls.value()), Role::Controller, sink,
                        {.onReady = [&] { ctlPtr->session().open({.argv = {"/bin/echo", "hello-from-stage"}}); },
                         .onError =
                             [&](psx::Error) {
                                 errored = true;
                                 r.stop();
                             }});
    ctlPtr = &ctl;

    ASSERT_TRUE(node.start().ok());
    ASSERT_TRUE(ctl.start().ok());
    r.after(std::chrono::seconds(5), [&] { r.stop(); });
    ASSERT_TRUE(r.run().ok());

    EXPECT_FALSE(errored);
    EXPECT_EQ(sink.data, "hello-from-stage\n"); // the real command's stdout, encrypted end to end
    EXPECT_TRUE(sink.exited);
    EXPECT_EQ(sink.status.code, 0);
    EXPECT_EQ(runner.runningStages(), 0U); // the stage was cleaned up
}

namespace {
// A controller sink that grants credit as it receives data (so backpressure can
// release) and collects the full output.
struct ConsumingSink : SessionHandler {
    Session* session = nullptr;
    Reactor* reactor = nullptr;
    std::string data;
    std::string stderrData;
    bool exited = false;
    void onData(StreamId id, std::string_view d, bool, Channel channel) override {
        (channel == Channel::Stderr ? stderrData : data).append(d);
        session->consume(id, static_cast<std::uint32_t>(d.size()));
    }
    void onExit(StreamId, const ExitStatus&) override {
        exited = true;
        reactor->stop();
    }
};
} // namespace

TEST(NodeStageRunnerTest, BackpressureDeliversLargeOutputThroughASmallWindow) {
    auto listener = Socket::listen("127.0.0.1", 0);
    ASSERT_TRUE(listener.ok());
    auto clientSock = Socket::connect("127.0.0.1", listener.value().localPort().value());
    ASSERT_TRUE(clientSock.ok());
    Socket serverSock = acceptWithin(listener.value());
    ASSERT_TRUE(serverSock.valid());

    const auto ctlId = tls_test::generateSelfSigned("psx://controller");
    const auto nodeId = tls_test::generateSelfSigned("psx://node/1");
    auto ctlTls = Tls::create(
        {.certificatePem = ctlId.certPem, .privateKeyPem = ctlId.keyPem, .caPem = nodeId.certPem, .isServer = false});
    auto nodeTls = Tls::create(
        {.certificatePem = nodeId.certPem, .privateKeyPem = nodeId.keyPem, .caPem = ctlId.certPem, .isServer = true});
    ASSERT_TRUE(ctlTls.ok() && nodeTls.ok());

    auto reactor = Reactor::create();
    ASSERT_TRUE(reactor.ok());
    Reactor& r = *reactor.value();

    NodeStageRunner runner(r);
    ConsumingSink sink;
    sink.reactor = &r;
    bool errored = false;
    NativeTransport* ctlPtr = nullptr;

    // Deliberately tiny window (512 B) so a ~9 KB stage forces many pause/resume
    // cycles in the node's reader.
    NativeTransport node(r, std::move(serverSock), std::move(nodeTls.value()), Role::Node, runner,
                         {.onError =
                              [&](psx::Error) {
                                  errored = true;
                                  r.stop();
                              }},
                         /*initialWindow=*/512);
    runner.bind(node.session());
    NativeTransport ctl(r, std::move(clientSock.value()), std::move(ctlTls.value()), Role::Controller, sink,
                        {.onReady = [&] { ctlPtr->session().open({.argv = {"sh", "-c", "seq 1 2000"}}); },
                         .onError =
                             [&](psx::Error) {
                                 errored = true;
                                 r.stop();
                             }},
                        /*initialWindow=*/512);
    sink.session = &ctl.session();
    ctlPtr = &ctl;

    ASSERT_TRUE(node.start().ok());
    ASSERT_TRUE(ctl.start().ok());
    r.after(std::chrono::seconds(10), [&] { r.stop(); });
    ASSERT_TRUE(r.run().ok());

    EXPECT_FALSE(errored);
    ASSERT_TRUE(sink.exited);
    // The full `seq 1 2000` output arrived intact despite the tiny window.
    std::string expected;
    for (int i = 1; i <= 2000; ++i) {
        expected += std::to_string(i) + "\n";
    }
    EXPECT_EQ(sink.data.size(), expected.size());
    EXPECT_EQ(sink.data, expected);
}

namespace {
// Builds a controller+node NativeTransport pair over loopback with the given
// controller SAN and node authorize predicate, runs the reactor, and reports
// whether the node authorized (onReady) or rejected (onError) the controller.
struct AuthzOutcome {
    bool nodeReady = false;
    bool nodeErrored = false;
};

AuthzOutcome runAuthz(const std::string& controllerSan, std::function<bool(std::string_view)> nodeAuthorize) {
    auto listener = Socket::listen("127.0.0.1", 0);
    EXPECT_TRUE(listener.ok());
    auto clientSock = Socket::connect("127.0.0.1", listener.value().localPort().value());
    EXPECT_TRUE(clientSock.ok());
    Socket serverSock = acceptWithin(listener.value());
    EXPECT_TRUE(serverSock.valid());

    const auto ctlId = tls_test::generateSelfSigned(controllerSan);
    const auto nodeId = tls_test::generateSelfSigned("psx://node/1");
    auto ctlTls = Tls::create(
        {.certificatePem = ctlId.certPem, .privateKeyPem = ctlId.keyPem, .caPem = nodeId.certPem, .isServer = false});
    auto nodeTls = Tls::create({.certificatePem = nodeId.certPem,
                                .privateKeyPem = nodeId.keyPem,
                                .caPem = ctlId.certPem, // the node trusts this cert (authN passes)
                                .isServer = true});
    EXPECT_TRUE(ctlTls.ok() && nodeTls.ok());

    auto reactor = Reactor::create();
    EXPECT_TRUE(reactor.ok());
    Reactor& r = *reactor.value();

    NodeStage nodeStage;
    ControllerSink sink;
    sink.reactor = &r;
    AuthzOutcome outcome;

    NativeTransport node(r, std::move(serverSock), std::move(nodeTls.value()), Role::Node, nodeStage,
                         {.authorize = std::move(nodeAuthorize),
                          .onReady =
                              [&] {
                                  outcome.nodeReady = true;
                                  r.stop();
                              },
                          .onError =
                              [&](psx::Error) {
                                  outcome.nodeErrored = true;
                                  r.stop();
                              }});
    nodeStage.session = &node.session();
    NativeTransport ctl(r, std::move(clientSock.value()), std::move(ctlTls.value()), Role::Controller, sink,
                        {.onError = [&](psx::Error) { r.stop(); }});

    EXPECT_TRUE(node.start().ok());
    EXPECT_TRUE(ctl.start().ok());
    r.after(std::chrono::seconds(3), [&] { r.stop(); });
    EXPECT_TRUE(r.run().ok());
    return outcome;
}
} // namespace

TEST(NativeTransportTest, RejectsAnAuthenticatedButUnauthorizedPeer) {
    // The controller's cert is trusted (CA-signed), but its identity is not on
    // the node's allow-list, so authorization fails after the handshake.
    const AuthzOutcome outcome =
        runAuthz("psx://attacker", [](std::string_view san) { return san == "psx://controller"; });
    EXPECT_TRUE(outcome.nodeErrored);
    EXPECT_FALSE(outcome.nodeReady) << "an unauthorized peer must not reach onReady";
}

TEST(NativeTransportTest, AcceptsAnAuthorizedPeer) {
    const AuthzOutcome outcome =
        runAuthz("psx://controller", [](std::string_view san) { return san == "psx://controller"; });
    EXPECT_TRUE(outcome.nodeReady);
    EXPECT_FALSE(outcome.nodeErrored);
}

TEST(NodeServerTest, AcceptsAControllerAndRunsAStageWithCaIssuedCerts) {
    using psx::ca::CertificateAuthority;

    auto ca = CertificateAuthority::create("psx-fleet");
    ASSERT_TRUE(ca.ok());
    auto nodeId = ca.value().issue("psx://node/1");
    auto ctlId = ca.value().issue("psx://controller");
    ASSERT_TRUE(nodeId.ok() && ctlId.ok());
    const std::string caCert = ca.value().certificatePem();

    auto listener = Socket::listen("127.0.0.1", 0);
    ASSERT_TRUE(listener.ok());
    const std::uint16_t port = listener.value().localPort().value();

    auto reactor = Reactor::create();
    ASSERT_TRUE(reactor.ok());
    Reactor& r = *reactor.value();

    // The node daemon: trusts the CA, only admits the controller identity.
    psx::transport::NodeServer server(r, std::move(listener.value()),
                                      {.certificatePem = nodeId.value().certificatePem,
                                       .privateKeyPem = nodeId.value().privateKeyPem,
                                       .caPem = caCert},
                                      [](std::string_view san) { return san == "psx://controller"; });
    ASSERT_TRUE(server.start().ok());

    // A controller connects and runs a stage.
    auto clientSock = Socket::connect("127.0.0.1", port);
    ASSERT_TRUE(clientSock.ok());
    auto ctlTls = Tls::create({.certificatePem = ctlId.value().certificatePem,
                               .privateKeyPem = ctlId.value().privateKeyPem,
                               .caPem = caCert,
                               .isServer = false});
    ASSERT_TRUE(ctlTls.ok());

    ControllerSink sink;
    sink.reactor = &r;
    bool errored = false;
    NativeTransport* ctlPtr = nullptr;
    NativeTransport ctl(r, std::move(clientSock.value()), std::move(ctlTls.value()), Role::Controller, sink,
                        {.onReady = [&] { ctlPtr->session().open({.argv = {"/bin/echo", "via-node-server"}}); },
                         .onError =
                             [&](psx::Error) {
                                 errored = true;
                                 r.stop();
                             }});
    ctlPtr = &ctl;
    ASSERT_TRUE(ctl.start().ok());

    r.after(std::chrono::seconds(5), [&] { r.stop(); });
    ASSERT_TRUE(r.run().ok());

    EXPECT_FALSE(errored);
    EXPECT_TRUE(sink.exited);
    EXPECT_EQ(sink.data, "via-node-server\n"); // the stage ran on the accepted connection
    EXPECT_EQ(sink.status.code, 0);
    EXPECT_EQ(server.connectionCount(), 1U); // the controller's connection was accepted and kept

    // Metrics snapshot (the `node status` source): one connection accepted and
    // still open; the echo stage has already exited, so none is running.
    const auto snapshot = server.metrics();
    EXPECT_EQ(snapshot.acceptedTotal, 1U);
    EXPECT_EQ(snapshot.activeConnections, 1U);
    EXPECT_EQ(snapshot.activeStages, 0U);
}

TEST(NodeServerTest, DropsAnUnauthorizedConnectionAndReapsIt) {
    using psx::ca::CertificateAuthority;
    auto ca = CertificateAuthority::create("psx-fleet");
    ASSERT_TRUE(ca.ok());
    auto nodeId = ca.value().issue("psx://node/1");
    auto attackerId = ca.value().issue("psx://attacker"); // CA-signed, but not authorized
    ASSERT_TRUE(nodeId.ok() && attackerId.ok());
    const std::string caCert = ca.value().certificatePem();

    auto listener = Socket::listen("127.0.0.1", 0);
    ASSERT_TRUE(listener.ok());
    const std::uint16_t port = listener.value().localPort().value();

    auto reactor = Reactor::create();
    ASSERT_TRUE(reactor.ok());
    Reactor& r = *reactor.value();

    psx::transport::NodeServer server(
        r, std::move(listener.value()),
        {.certificatePem = nodeId.value().certificatePem,
         .privateKeyPem = nodeId.value().privateKeyPem,
         .caPem = caCert},
        [](std::string_view san) { return san == "psx://controller"; }); // attacker not allowed
    ASSERT_TRUE(server.start().ok());

    auto clientSock = Socket::connect("127.0.0.1", port);
    ASSERT_TRUE(clientSock.ok());
    auto ctlTls = Tls::create({.certificatePem = attackerId.value().certificatePem,
                               .privateKeyPem = attackerId.value().privateKeyPem,
                               .caPem = caCert,
                               .isServer = false});
    ASSERT_TRUE(ctlTls.ok());

    ControllerSink sink;
    sink.reactor = &r;
    NativeTransport ctl(r, std::move(clientSock.value()), std::move(ctlTls.value()), Role::Controller, sink,
                        {.onError = [&](psx::Error) { /* connection dropped by the node */ }});
    ASSERT_TRUE(ctl.start().ok());

    // Let the accept + reject + deferred reap run, then stop.
    r.after(std::chrono::milliseconds(800), [&] { r.stop(); });
    ASSERT_TRUE(r.run().ok());

    EXPECT_EQ(server.connectionCount(), 0U) << "the unauthorized connection must be reaped";
}

TEST(NodeServerTest, DestructorCancelsDeferredReapBeforeReactorContinues) {
    auto reactor = Reactor::create();
    ASSERT_TRUE(reactor.ok());
    Reactor& r = *reactor.value();
    auto listener = Socket::listen("127.0.0.1", 0);
    ASSERT_TRUE(listener.ok());

    {
        NodeServer server(r, std::move(listener.value()), psx::os::TlsConfig{});
        psx::transport::NodeServerLifecycleProbe::scheduleDeferredReap(server);
        ASSERT_EQ(r.pendingTimers(), 1U);
    }
    ASSERT_EQ(r.pendingTimers(), 0U) << "destruction must remove callbacks that capture the server";

    bool continued = false;
    r.after(std::chrono::milliseconds(0), [&] {
        continued = true;
        r.stop();
    });
    ASSERT_TRUE(r.run().ok());
    EXPECT_TRUE(continued);
}

namespace {
// Stands up a NodeServer on a fresh listener; returns {server-owning-reactor use}.
struct Fleet {
    psx::ca::CertificateAuthority ca;
    psx::ca::Identity nodeId;
    psx::ca::Identity ctlId;
    std::string caCert;
};

Fleet makeFleet() {
    auto ca = psx::ca::CertificateAuthority::create("psx-fleet");
    EXPECT_TRUE(ca.ok());
    auto nodeId = ca.value().issue("psx://node/1");
    auto ctlId = ca.value().issue("psx://controller");
    EXPECT_TRUE(nodeId.ok() && ctlId.ok());
    std::string caCert = ca.value().certificatePem();
    return {std::move(ca.value()), std::move(nodeId.value()), std::move(ctlId.value()), std::move(caCert)};
}
} // namespace

TEST(NativeControllerTest, RunsACommandOnANodeAndCollectsOutput) {
    Fleet fleet = makeFleet();

    auto listener = Socket::listen("127.0.0.1", 0);
    ASSERT_TRUE(listener.ok());
    const std::uint16_t port = listener.value().localPort().value();

    auto reactor = Reactor::create();
    ASSERT_TRUE(reactor.ok());
    Reactor& r = *reactor.value();

    psx::transport::NodeServer server(r, std::move(listener.value()),
                                      {.certificatePem = fleet.nodeId.certificatePem,
                                       .privateKeyPem = fleet.nodeId.privateKeyPem,
                                       .caPem = fleet.caCert},
                                      [](std::string_view san) { return san == "psx://controller"; });
    ASSERT_TRUE(server.start().ok());

    psx::transport::NativeController controller(r, {.certificatePem = fleet.ctlId.certificatePem,
                                                    .privateKeyPem = fleet.ctlId.privateKeyPem,
                                                    .caPem = fleet.caCert});

    std::vector<psx::transport::NativeController::HostResult> results;
    ASSERT_TRUE(controller
                    .start({{.host = "127.0.0.1", .port = port, .expectedSan = "psx://node/1"}},
                           {"/bin/echo", "native-run"},
                           [&](std::vector<psx::transport::NativeController::HostResult> res) {
                               results = std::move(res);
                               r.stop();
                           })
                    .ok());
    r.after(std::chrono::seconds(5), [&] { r.stop(); });
    ASSERT_TRUE(r.run().ok());

    ASSERT_EQ(results.size(), 1U);
    EXPECT_TRUE(results[0].ok) << results[0].error;
    EXPECT_EQ(results[0].exitCode, 0);
    EXPECT_EQ(results[0].output, "native-run\n");
    EXPECT_EQ(results[0].host, "127.0.0.1");
    EXPECT_EQ(results[0].lamportTs, 1U);
}

TEST(NativeControllerTest, ReportsAnErrorForAnUnreachableNode) {
    Fleet fleet = makeFleet();
    auto reactor = Reactor::create();
    ASSERT_TRUE(reactor.ok());
    Reactor& r = *reactor.value();

    psx::transport::NativeController controller(r, {.certificatePem = fleet.ctlId.certificatePem,
                                                    .privateKeyPem = fleet.ctlId.privateKeyPem,
                                                    .caPem = fleet.caCert});

    // Port 1 on loopback: connection refused.
    std::vector<psx::transport::NativeController::HostResult> results;
    ASSERT_TRUE(controller
                    .start({{.host = "127.0.0.1", .port = 1, .expectedSan = "psx://node/1"}}, {"/bin/echo", "x"},
                           [&](std::vector<psx::transport::NativeController::HostResult> res) {
                               results = std::move(res);
                               r.stop();
                           })
                    .ok());
    r.after(std::chrono::seconds(5), [&] { r.stop(); });
    ASSERT_TRUE(r.run().ok());

    ASSERT_EQ(results.size(), 1U);
    EXPECT_FALSE(results[0].ok);
    EXPECT_FALSE(results[0].error.empty());
}

TEST(NativeControllerTest, RejectsInvalidAndRepeatedStartCalls) {
    auto reactor = Reactor::create();
    ASSERT_TRUE(reactor.ok());
    NativeController controller(
        *reactor.value(), {.certificatePem = {}, .privateKeyPem = {}, .caPem = {}, .crlPem = {}, .isServer = false});
    auto invalid = controller.start({}, {}, [](std::vector<NativeController::HostResult>) {});
    EXPECT_FALSE(invalid.ok());
    EXPECT_EQ(invalid.error().cls, psx::ErrorClass::InvalidArgument);

    bool completed = false;
    EXPECT_TRUE(controller
                    .start({}, {"true"},
                           [&](std::vector<NativeController::HostResult> results) {
                               EXPECT_TRUE(results.empty());
                               completed = true;
                           })
                    .ok());
    EXPECT_TRUE(completed);

    auto repeated = controller.start({}, {"true"}, [](std::vector<NativeController::HostResult>) {});
    EXPECT_FALSE(repeated.ok());
    EXPECT_EQ(repeated.error().cls, psx::ErrorClass::InvalidArgument);
}

namespace {

std::vector<NativeController::HostResult> runCapturedCommand(psx::stream::OverflowPolicy policy,
                                                             std::size_t ringBytes) {
    Fleet fleet = makeFleet();
    auto listener = Socket::listen("127.0.0.1", 0);
    EXPECT_TRUE(listener.ok());
    if (!listener.ok()) {
        return {};
    }
    const std::uint16_t port = listener.value().localPort().value();

    auto reactor = Reactor::create();
    EXPECT_TRUE(reactor.ok());
    if (!reactor.ok()) {
        return {};
    }
    Reactor& r = *reactor.value();
    NodeServer server(r, std::move(listener.value()),
                      {.certificatePem = fleet.nodeId.certificatePem,
                       .privateKeyPem = fleet.nodeId.privateKeyPem,
                       .caPem = fleet.caCert,
                       .crlPem = {},
                       .isServer = true},
                      [](std::string_view san) { return san == "psx://controller"; });
    EXPECT_TRUE(server.start().ok());

    NativeController controller(r, {.certificatePem = fleet.ctlId.certificatePem,
                                    .privateKeyPem = fleet.ctlId.privateKeyPem,
                                    .caPem = fleet.caCert,
                                    .crlPem = {},
                                    .isServer = false});
    std::vector<NativeController::HostResult> results;
    EXPECT_TRUE(controller
                    .start({{.host = "127.0.0.1", .port = port, .expectedSan = "psx://node/1"}},
                           {"/bin/sh", "-c", "printf 0123456789; printf abcdef 1>&2"},
                           [&](std::vector<NativeController::HostResult> value) {
                               results = std::move(value);
                               r.stop();
                           },
                           {.concurrency = 1, .policy = policy, .ringBytes = ringBytes, .failFast = false})
                    .ok());
    r.after(std::chrono::seconds(5), [&] { r.stop(); });
    EXPECT_TRUE(r.run().ok());
    return results;
}

} // namespace

TEST(NativeControllerTest, DropOldestBoundsEachOutputChannelAndCountsLoss) {
    const auto results = runCapturedCommand(psx::stream::OverflowPolicy::DropOldest, 4);
    ASSERT_EQ(results.size(), 1U);
    EXPECT_EQ(results[0].stdoutData, "6789");
    EXPECT_EQ(results[0].stderrData, "cdef");
    EXPECT_EQ(results[0].output, "6789cdef");
    EXPECT_EQ(results[0].droppedBytes, 8U);
}

TEST(NativeControllerTest, DropNewestBoundsEachOutputChannelAndCountsLoss) {
    const auto results = runCapturedCommand(psx::stream::OverflowPolicy::DropNewest, 4);
    ASSERT_EQ(results.size(), 1U);
    EXPECT_EQ(results[0].stdoutData, "0123");
    EXPECT_EQ(results[0].stderrData, "abcd");
    EXPECT_EQ(results[0].output, "0123abcd");
    EXPECT_EQ(results[0].droppedBytes, 8U);
}

TEST(NativeControllerTest, SpoolKeepsLosslessOutputWithBoundedMemoryRing) {
    const auto results = runCapturedCommand(psx::stream::OverflowPolicy::Spool, 4);
    ASSERT_EQ(results.size(), 1U);
    EXPECT_EQ(results[0].stdoutData, "0123456789");
    EXPECT_EQ(results[0].stderrData, "abcdef");
    EXPECT_EQ(results[0].output, "0123456789abcdef");
    EXPECT_EQ(results[0].droppedBytes, 0U);
}

TEST(NativeControllerTest, TimeoutCancellationIsClassified) {
    Fleet fleet = makeFleet();
    auto listener = Socket::listen("127.0.0.1", 0);
    ASSERT_TRUE(listener.ok());
    const std::uint16_t port = listener.value().localPort().value();
    auto reactor = Reactor::create();
    ASSERT_TRUE(reactor.ok());
    Reactor& r = *reactor.value();
    NodeServer server(r, std::move(listener.value()),
                      {.certificatePem = fleet.nodeId.certificatePem,
                       .privateKeyPem = fleet.nodeId.privateKeyPem,
                       .caPem = fleet.caCert,
                       .crlPem = {},
                       .isServer = true},
                      [](std::string_view san) { return san == "psx://controller"; });
    ASSERT_TRUE(server.start().ok());
    NativeController controller(r, {.certificatePem = fleet.ctlId.certificatePem,
                                    .privateKeyPem = fleet.ctlId.privateKeyPem,
                                    .caPem = fleet.caCert,
                                    .crlPem = {},
                                    .isServer = false});

    std::vector<NativeController::HostResult> results;
    ASSERT_TRUE(controller
                    .start({{.host = "127.0.0.1", .port = port, .expectedSan = "psx://node/1"}},
                           {"/bin/sh", "-c", "sleep 5"},
                           [&](std::vector<NativeController::HostResult> value) {
                               results = std::move(value);
                               r.stop();
                           })
                    .ok());
    r.after(std::chrono::milliseconds(100),
            [&] { controller.cancel("timed out", NativeController::CancelKind::Timeout); });
    r.after(std::chrono::seconds(3), [&] { r.stop(); });
    ASSERT_TRUE(r.run().ok());

    ASSERT_EQ(results.size(), 1U);
    EXPECT_TRUE(results[0].timedOut);
    EXPECT_FALSE(results[0].cancelled);
    EXPECT_FALSE(results[0].aborted);
    EXPECT_EQ(results[0].error, "timed out");
}

TEST(NativeControllerTest, ConcurrencyOptionSerializesConnections) {
    Fleet fleet = makeFleet();
    auto listener = Socket::listen("127.0.0.1", 0);
    ASSERT_TRUE(listener.ok());
    const std::uint16_t port = listener.value().localPort().value();
    auto reactor = Reactor::create();
    ASSERT_TRUE(reactor.ok());
    Reactor& r = *reactor.value();
    NodeServer server(r, std::move(listener.value()),
                      {.certificatePem = fleet.nodeId.certificatePem,
                       .privateKeyPem = fleet.nodeId.privateKeyPem,
                       .caPem = fleet.caCert,
                       .crlPem = {},
                       .isServer = true},
                      [](std::string_view san) { return san == "psx://controller"; });
    ASSERT_TRUE(server.start().ok());
    NativeController controller(r, {.certificatePem = fleet.ctlId.certificatePem,
                                    .privateKeyPem = fleet.ctlId.privateKeyPem,
                                    .caPem = fleet.caCert,
                                    .crlPem = {},
                                    .isServer = false});
    const std::vector<NativeController::Target> targets(
        3, {.host = "127.0.0.1", .port = port, .expectedSan = "psx://node/1"});

    std::vector<NativeController::HostResult> results;
    const auto startedAt = std::chrono::steady_clock::now();
    ASSERT_TRUE(
        controller
            .start(targets, {"/bin/sh", "-c", "sleep 0.1; printf done"},
                   [&](std::vector<NativeController::HostResult> value) {
                       results = std::move(value);
                       r.stop();
                   },
                   {.concurrency = 1, .policy = psx::stream::OverflowPolicy::Block, .ringBytes = 0, .failFast = false})
            .ok());
    r.after(std::chrono::seconds(5), [&] { r.stop(); });
    ASSERT_TRUE(r.run().ok());
    const auto elapsed = std::chrono::steady_clock::now() - startedAt;

    ASSERT_EQ(results.size(), 3U);
    EXPECT_GE(elapsed, std::chrono::milliseconds(250));
    EXPECT_EQ(server.metrics().acceptedTotal, 3U);
}

TEST(NativeControllerTest, FailFastCancelsPendingTargetsWithoutConnectingThem) {
    Fleet fleet = makeFleet();
    auto listener = Socket::listen("127.0.0.1", 0);
    ASSERT_TRUE(listener.ok());
    const std::uint16_t port = listener.value().localPort().value();
    auto reactor = Reactor::create();
    ASSERT_TRUE(reactor.ok());
    Reactor& r = *reactor.value();
    NodeServer server(r, std::move(listener.value()),
                      {.certificatePem = fleet.nodeId.certificatePem,
                       .privateKeyPem = fleet.nodeId.privateKeyPem,
                       .caPem = fleet.caCert,
                       .crlPem = {},
                       .isServer = true},
                      [](std::string_view san) { return san == "psx://controller"; });
    ASSERT_TRUE(server.start().ok());
    NativeController controller(r, {.certificatePem = fleet.ctlId.certificatePem,
                                    .privateKeyPem = fleet.ctlId.privateKeyPem,
                                    .caPem = fleet.caCert,
                                    .crlPem = {},
                                    .isServer = false});
    const std::vector<NativeController::Target> targets(
        3, {.host = "127.0.0.1", .port = port, .expectedSan = "psx://node/1"});

    std::vector<NativeController::HostResult> results;
    ASSERT_TRUE(
        controller
            .start(targets, {"/bin/sh", "-c", "exit 7"},
                   [&](std::vector<NativeController::HostResult> value) {
                       results = std::move(value);
                       r.stop();
                   },
                   {.concurrency = 1, .policy = psx::stream::OverflowPolicy::Block, .ringBytes = 0, .failFast = true})
            .ok());
    r.after(std::chrono::seconds(5), [&] { r.stop(); });
    ASSERT_TRUE(r.run().ok());

    ASSERT_EQ(results.size(), 3U);
    EXPECT_TRUE(results[0].ok);
    EXPECT_EQ(results[0].exitCode, 7);
    EXPECT_FALSE(results[0].aborted);
    for (std::size_t i = 1; i < results.size(); ++i) {
        EXPECT_TRUE(results[i].aborted);
        EXPECT_EQ(results[i].error, "fail-fast");
    }
    EXPECT_EQ(server.metrics().acceptedTotal, 1U);
}

TEST(NativeControllerTest, CompletionCallbackMayDestroyTheController) {
    Fleet fleet = makeFleet();
    auto listener = Socket::listen("127.0.0.1", 0);
    ASSERT_TRUE(listener.ok());
    const std::uint16_t port = listener.value().localPort().value();
    auto reactor = Reactor::create();
    ASSERT_TRUE(reactor.ok());
    Reactor& r = *reactor.value();
    NodeServer server(r, std::move(listener.value()),
                      {.certificatePem = fleet.nodeId.certificatePem,
                       .privateKeyPem = fleet.nodeId.privateKeyPem,
                       .caPem = fleet.caCert,
                       .crlPem = {},
                       .isServer = true},
                      [](std::string_view san) { return san == "psx://controller"; });
    ASSERT_TRUE(server.start().ok());

    std::unique_ptr<NativeController> controller =
        std::make_unique<NativeController>(r, psx::os::TlsConfig{.certificatePem = fleet.ctlId.certificatePem,
                                                                 .privateKeyPem = fleet.ctlId.privateKeyPem,
                                                                 .caPem = fleet.caCert,
                                                                 .crlPem = {},
                                                                 .isServer = false});
    bool completed = false;
    ASSERT_TRUE(controller
                    ->start({{.host = "127.0.0.1", .port = port, .expectedSan = "psx://node/1"}}, {"/bin/true"},
                            [&](std::vector<NativeController::HostResult> results) {
                                EXPECT_EQ(results.size(), 1U);
                                completed = true;
                                controller.reset();
                                r.stop();
                            })
                    .ok());
    r.after(std::chrono::seconds(5), [&] { r.stop(); });
    ASSERT_TRUE(r.run().ok());
    EXPECT_TRUE(completed);
    EXPECT_EQ(controller, nullptr);
}

// Lease / silent-partition fencing: a peer that completes the handshake and then
// goes quiet (no FIN, no PONG) must be detected. With a fast lease the controller
// fires onError(Timeout) after maxMissed silent intervals.
TEST(NativeTransportTest, LeaseExpiresWhenThePeerGoesSilent) {
    auto listener = Socket::listen("127.0.0.1", 0);
    ASSERT_TRUE(listener.ok());
    const auto port = listener.value().localPort();
    ASSERT_TRUE(port.ok());
    auto clientSock = Socket::connect("127.0.0.1", port.value());
    ASSERT_TRUE(clientSock.ok());
    Socket serverSock = acceptWithin(listener.value());
    ASSERT_TRUE(serverSock.valid());

    const auto ctlId = tls_test::generateSelfSigned("psx://controller");
    const auto nodeId = tls_test::generateSelfSigned("psx://node/1");
    auto ctlTls = Tls::create(
        {.certificatePem = ctlId.certPem, .privateKeyPem = ctlId.keyPem, .caPem = nodeId.certPem, .isServer = false});
    auto nodeTls = Tls::create(
        {.certificatePem = nodeId.certPem, .privateKeyPem = nodeId.keyPem, .caPem = ctlId.certPem, .isServer = true});
    ASSERT_TRUE(ctlTls.ok());
    ASSERT_TRUE(nodeTls.ok());

    auto reactor = Reactor::create();
    ASSERT_TRUE(reactor.ok());
    Reactor& r = *reactor.value();

    // A "frozen" node: it finishes the handshake, then never sends another frame
    // -- in particular it never PONGs. The socket stays open (no FIN), so only the
    // lease can tell that the peer is gone.
    TlsStream silentNode(r, std::move(serverSock), std::move(nodeTls.value()),
                         TlsStream::Callbacks{.onData = [](std::string_view) {}, .onError = [](psx::Error) {}});
    ASSERT_TRUE(silentNode.start().ok());

    struct Sink : SessionHandler {
    } sink;
    bool errored = false;
    psx::Error captured{psx::ErrorClass::Other, 0, "none"};
    NativeTransport ctl(r, std::move(clientSock.value()), std::move(ctlTls.value()), Role::Controller, sink,
                        {.onError =
                             [&](psx::Error e) {
                                 errored = true;
                                 captured = e;
                                 r.stop();
                             }},
                        kDefaultStreamWindow, HeartbeatOptions{std::chrono::milliseconds(5), 3});
    ASSERT_TRUE(ctl.start().ok());

    r.after(std::chrono::seconds(2), [&] { r.stop(); }); // safety net against a hang
    ASSERT_TRUE(r.run().ok());

    EXPECT_TRUE(errored) << "the lease never expired on a silent peer";
    EXPECT_EQ(captured.cls, psx::ErrorClass::Timeout);
}

// The lease must not fence a healthy-but-idle connection: with both ends running
// a fast lease and no stage traffic, the automatic PING/PONG keeps each side's
// lease satisfied, so neither errors across many intervals.
TEST(NativeTransportTest, LeaseKeepsAnIdleConnectionAlive) {
    auto listener = Socket::listen("127.0.0.1", 0);
    ASSERT_TRUE(listener.ok());
    const auto port = listener.value().localPort();
    ASSERT_TRUE(port.ok());
    auto clientSock = Socket::connect("127.0.0.1", port.value());
    ASSERT_TRUE(clientSock.ok());
    Socket serverSock = acceptWithin(listener.value());
    ASSERT_TRUE(serverSock.valid());

    const auto ctlId = tls_test::generateSelfSigned("psx://controller");
    const auto nodeId = tls_test::generateSelfSigned("psx://node/1");
    auto ctlTls = Tls::create(
        {.certificatePem = ctlId.certPem, .privateKeyPem = ctlId.keyPem, .caPem = nodeId.certPem, .isServer = false});
    auto nodeTls = Tls::create(
        {.certificatePem = nodeId.certPem, .privateKeyPem = nodeId.keyPem, .caPem = ctlId.certPem, .isServer = true});
    ASSERT_TRUE(ctlTls.ok());
    ASSERT_TRUE(nodeTls.ok());

    auto reactor = Reactor::create();
    ASSERT_TRUE(reactor.ok());
    Reactor& r = *reactor.value();

    struct Sink : SessionHandler {
    } nodeSink, ctlSink;
    bool errored = false;
    const HeartbeatOptions lease{std::chrono::milliseconds(5), 3};
    NativeTransport node(r, std::move(serverSock), std::move(nodeTls.value()), Role::Node, nodeSink,
                         {.onError = [&](psx::Error) { errored = true; }}, kDefaultStreamWindow, lease);
    NativeTransport ctl(r, std::move(clientSock.value()), std::move(ctlTls.value()), Role::Controller, ctlSink,
                        {.onError = [&](psx::Error) { errored = true; }}, kDefaultStreamWindow, lease);
    ASSERT_TRUE(node.start().ok());
    ASSERT_TRUE(ctl.start().ok());

    // Sit idle for many lease intervals; a broken lease would fire well before this.
    r.after(std::chrono::milliseconds(80), [&] { r.stop(); });
    ASSERT_TRUE(r.run().ok());

    EXPECT_FALSE(errored) << "the lease falsely fenced a healthy idle connection";
}

// A real stage that writes to both fds; the node tags each pipe's bytes with its
// channel, and the controller separates them (as the SSH path does).
TEST(NodeStageRunnerTest, SplitsStdoutAndStderrOnTheWire) {
    auto listener = Socket::listen("127.0.0.1", 0);
    ASSERT_TRUE(listener.ok());
    auto clientSock = Socket::connect("127.0.0.1", listener.value().localPort().value());
    ASSERT_TRUE(clientSock.ok());
    Socket serverSock = acceptWithin(listener.value());
    ASSERT_TRUE(serverSock.valid());

    const auto ctlId = tls_test::generateSelfSigned("psx://controller");
    const auto nodeId = tls_test::generateSelfSigned("psx://node/1");
    auto ctlTls = Tls::create(
        {.certificatePem = ctlId.certPem, .privateKeyPem = ctlId.keyPem, .caPem = nodeId.certPem, .isServer = false});
    auto nodeTls = Tls::create(
        {.certificatePem = nodeId.certPem, .privateKeyPem = nodeId.keyPem, .caPem = ctlId.certPem, .isServer = true});
    ASSERT_TRUE(ctlTls.ok() && nodeTls.ok());

    auto reactor = Reactor::create();
    ASSERT_TRUE(reactor.ok());
    Reactor& r = *reactor.value();

    NodeStageRunner runner(r);
    ConsumingSink sink;
    sink.reactor = &r;
    bool errored = false;
    NativeTransport* ctlPtr = nullptr;

    NativeTransport node(r, std::move(serverSock), std::move(nodeTls.value()), Role::Node, runner,
                         {.onError = [&](psx::Error) {
                             errored = true;
                             r.stop();
                         }});
    runner.bind(node.session());
    NativeTransport ctl(
        r, std::move(clientSock.value()), std::move(ctlTls.value()), Role::Controller, sink,
        {.onReady = [&] { ctlPtr->session().open({.argv = {"sh", "-c", "printf OUT; printf ERR 1>&2"}}); },
         .onError =
             [&](psx::Error) {
                 errored = true;
                 r.stop();
             }});
    sink.session = &ctl.session();
    ctlPtr = &ctl;

    ASSERT_TRUE(node.start().ok());
    ASSERT_TRUE(ctl.start().ok());
    r.after(std::chrono::seconds(5), [&] { r.stop(); });
    ASSERT_TRUE(r.run().ok());

    EXPECT_FALSE(errored);
    ASSERT_TRUE(sink.exited);
    EXPECT_EQ(sink.data, "OUT");       // stdout channel
    EXPECT_EQ(sink.stderrData, "ERR"); // stderr channel, kept distinct
}

// A revoked peer must be rejected over the live reactor path too (not only in the
// synchronous Tls handshake). Reproduces the `node --crl` scenario in-process.
TEST(NativeTransportTest, CrlRejectsARevokedPeerOverTheReactor) {
    auto ca = psx::ca::CertificateAuthority::create("psx-fleet");
    ASSERT_TRUE(ca.ok());
    auto nodeId = ca.value().issue("psx://node/1");
    auto ctlId = ca.value().issue("psx://controller");
    ASSERT_TRUE(nodeId.ok() && ctlId.ok());
    auto serial = psx::ca::CertificateAuthority::serialHex(ctlId.value().certificatePem);
    ASSERT_TRUE(serial.ok());
    auto crl = ca.value().issueCrl({serial.value()});
    ASSERT_TRUE(crl.ok());
    const std::string caCert = ca.value().certificatePem();

    auto listener = Socket::listen("127.0.0.1", 0);
    ASSERT_TRUE(listener.ok());
    auto clientSock = Socket::connect("127.0.0.1", listener.value().localPort().value());
    ASSERT_TRUE(clientSock.ok());
    Socket serverSock = acceptWithin(listener.value());
    ASSERT_TRUE(serverSock.valid());

    auto nodeTls = Tls::create({.certificatePem = nodeId.value().certificatePem,
                                .privateKeyPem = nodeId.value().privateKeyPem,
                                .caPem = caCert,
                                .crlPem = crl.value(), // the node enforces the CRL
                                .isServer = true});
    auto ctlTls = Tls::create({.certificatePem = ctlId.value().certificatePem,
                               .privateKeyPem = ctlId.value().privateKeyPem,
                               .caPem = caCert,
                               .isServer = false});
    ASSERT_TRUE(nodeTls.ok() && ctlTls.ok());

    auto reactor = Reactor::create();
    ASSERT_TRUE(reactor.ok());
    Reactor& r = *reactor.value();

    struct Sink : SessionHandler {
    } nodeSink, ctlSink;
    bool nodeReady = false;
    NativeTransport node(r, std::move(serverSock), std::move(nodeTls.value()), Role::Node, nodeSink,
                         {.onReady = [&] { nodeReady = true; }, .onError = [&](psx::Error) { r.stop(); }});
    NativeTransport ctl(r, std::move(clientSock.value()), std::move(ctlTls.value()), Role::Controller, ctlSink,
                        {.onError = [&](psx::Error) { r.stop(); }});
    ASSERT_TRUE(node.start().ok());
    ASSERT_TRUE(ctl.start().ok());
    r.after(std::chrono::seconds(2), [&] { r.stop(); });
    ASSERT_TRUE(r.run().ok());

    EXPECT_FALSE(nodeReady) << "the node accepted a peer whose certificate is revoked";
}

// Concurrent fan-out over the real transport: one reactor, one NodeServer, and a
// controller opening many mTLS connections at once, each running a stage. A
// scaled-down stand-in for the 1000-node target (the full count is a CI /
// dedicated-host concern — 2*N RSA handshakes); this proves the reactor + TLS +
// NodeServer path handles a large concurrent fan-out correctly.
TEST(NodeServerTest, HandlesManyConcurrentControllerConnections) {
    using psx::ca::CertificateAuthority;
    auto ca = CertificateAuthority::create("psx-fleet");
    ASSERT_TRUE(ca.ok());
    auto nodeId = ca.value().issue("psx://node/1");
    auto ctlId = ca.value().issue("psx://controller");
    ASSERT_TRUE(nodeId.ok() && ctlId.ok());
    const std::string caCert = ca.value().certificatePem();

    auto listener = Socket::listen("127.0.0.1", 0);
    ASSERT_TRUE(listener.ok());
    const std::uint16_t port = listener.value().localPort().value();

    auto reactor = Reactor::create();
    ASSERT_TRUE(reactor.ok());
    Reactor& r = *reactor.value();

    NodeServer server(r, std::move(listener.value()),
                      {.certificatePem = nodeId.value().certificatePem,
                       .privateKeyPem = nodeId.value().privateKeyPem,
                       .caPem = caCert},
                      [](std::string_view san) { return san == "psx://controller"; });
    ASSERT_TRUE(server.start().ok());

    NativeController controller(r, {.certificatePem = ctlId.value().certificatePem,
                                    .privateKeyPem = ctlId.value().privateKeyPem,
                                    .caPem = caCert});
    constexpr int kNodes = 100;
    std::vector<NativeController::Target> targets(
        kNodes, NativeController::Target{.host = "127.0.0.1", .port = port, .expectedSan = "psx://node/1"});

    std::vector<NativeController::HostResult> results;
    ASSERT_TRUE(controller
                    .start(targets, {"/bin/echo", "hi"},
                           [&](std::vector<NativeController::HostResult> res) {
                               results = std::move(res);
                               r.stop();
                           })
                    .ok());
    r.after(std::chrono::seconds(30), [&] { r.stop(); });
    ASSERT_TRUE(r.run().ok());

    ASSERT_EQ(results.size(), static_cast<std::size_t>(kNodes));
    int succeeded = 0;
    for (const auto& res : results) {
        if (res.ok && res.exitCode == 0 && res.output == "hi\n") {
            ++succeeded;
        }
    }
    EXPECT_EQ(succeeded, kNodes) << "every concurrent connection should run its stage to a clean exit";
    EXPECT_EQ(server.metrics().acceptedTotal, static_cast<std::uint64_t>(kNodes));
}

// Cross-node stdin: the controller sends bytes to a remote stage's stdin over
// the backplane; the node feeds them to the process and closes stdin on
// endStream. `cat` echoes stdin to stdout, so the controller gets its bytes back.
TEST(NodeStageRunnerTest, ForwardsControllerStdinToTheStage) {
    auto listener = Socket::listen("127.0.0.1", 0);
    ASSERT_TRUE(listener.ok());
    auto clientSock = Socket::connect("127.0.0.1", listener.value().localPort().value());
    ASSERT_TRUE(clientSock.ok());
    Socket serverSock = acceptWithin(listener.value());
    ASSERT_TRUE(serverSock.valid());

    const auto ctlId = tls_test::generateSelfSigned("psx://controller");
    const auto nodeId = tls_test::generateSelfSigned("psx://node/1");
    auto ctlTls = Tls::create(
        {.certificatePem = ctlId.certPem, .privateKeyPem = ctlId.keyPem, .caPem = nodeId.certPem, .isServer = false});
    auto nodeTls = Tls::create(
        {.certificatePem = nodeId.certPem, .privateKeyPem = nodeId.keyPem, .caPem = ctlId.certPem, .isServer = true});
    ASSERT_TRUE(ctlTls.ok() && nodeTls.ok());

    auto reactor = Reactor::create();
    ASSERT_TRUE(reactor.ok());
    Reactor& r = *reactor.value();

    NodeStageRunner runner(r);
    ConsumingSink sink;
    sink.reactor = &r;
    bool errored = false;
    NativeTransport* ctlPtr = nullptr;

    NativeTransport node(r, std::move(serverSock), std::move(nodeTls.value()), Role::Node, runner,
                         {.onError = [&](psx::Error) {
                             errored = true;
                             r.stop();
                         }});
    runner.bind(node.session());
    NativeTransport ctl(r, std::move(clientSock.value()), std::move(ctlTls.value()), Role::Controller, sink,
                        {.onReady =
                             [&] {
                                 const StreamId id = ctlPtr->session().open({.argv = {"cat"}});
                                 ctlPtr->session().sendData(id, "hello from stdin\n", /*endStream=*/true);
                             },
                         .onError =
                             [&](psx::Error) {
                                 errored = true;
                                 r.stop();
                             }});
    sink.session = &ctl.session();
    ctlPtr = &ctl;

    ASSERT_TRUE(node.start().ok());
    ASSERT_TRUE(ctl.start().ok());
    r.after(std::chrono::seconds(5), [&] { r.stop(); });
    ASSERT_TRUE(r.run().ok());

    EXPECT_FALSE(errored);
    ASSERT_TRUE(sink.exited);
    EXPECT_EQ(sink.data, "hello from stdin\n"); // cat echoed the forwarded stdin
}

// Bidirectional backpressure: a large stdin streamed through `cat` under a tiny
// window. cat's throttled stdout stalls its stdin reads, so the node buffers
// stdin and withholds credit; every byte must still round-trip intact.
TEST(NodeStageRunnerTest, LargeStdinRoundTripsUnderATinyWindow) {
    auto listener = Socket::listen("127.0.0.1", 0);
    ASSERT_TRUE(listener.ok());
    auto clientSock = Socket::connect("127.0.0.1", listener.value().localPort().value());
    ASSERT_TRUE(clientSock.ok());
    Socket serverSock = acceptWithin(listener.value());
    ASSERT_TRUE(serverSock.valid());

    const auto ctlId = tls_test::generateSelfSigned("psx://controller");
    const auto nodeId = tls_test::generateSelfSigned("psx://node/1");
    auto ctlTls = Tls::create(
        {.certificatePem = ctlId.certPem, .privateKeyPem = ctlId.keyPem, .caPem = nodeId.certPem, .isServer = false});
    auto nodeTls = Tls::create(
        {.certificatePem = nodeId.certPem, .privateKeyPem = nodeId.keyPem, .caPem = ctlId.certPem, .isServer = true});
    ASSERT_TRUE(ctlTls.ok() && nodeTls.ok());

    auto reactor = Reactor::create();
    ASSERT_TRUE(reactor.ok());
    Reactor& r = *reactor.value();

    std::string payload;
    for (int i = 0; i < 4000; ++i) {
        payload += "line " + std::to_string(i) + "\n";
    }

    NodeStageRunner runner(r);
    ConsumingSink sink;
    sink.reactor = &r;
    bool errored = false;
    NativeTransport* ctlPtr = nullptr;

    NativeTransport node(r, std::move(serverSock), std::move(nodeTls.value()), Role::Node, runner,
                         {.onError =
                              [&](psx::Error) {
                                  errored = true;
                                  r.stop();
                              }},
                         /*initialWindow=*/512);
    runner.bind(node.session());
    NativeTransport ctl(r, std::move(clientSock.value()), std::move(ctlTls.value()), Role::Controller, sink,
                        {.onReady =
                             [&] {
                                 const StreamId id = ctlPtr->session().open({.argv = {"cat"}});
                                 ctlPtr->session().sendData(id, payload, /*endStream=*/true);
                             },
                         .onError =
                             [&](psx::Error) {
                                 errored = true;
                                 r.stop();
                             }},
                        /*initialWindow=*/512);
    sink.session = &ctl.session();
    ctlPtr = &ctl;

    ASSERT_TRUE(node.start().ok());
    ASSERT_TRUE(ctl.start().ok());
    r.after(std::chrono::seconds(10), [&] { r.stop(); });
    ASSERT_TRUE(r.run().ok());

    EXPECT_FALSE(errored);
    ASSERT_TRUE(sink.exited);
    EXPECT_EQ(sink.data.size(), payload.size());
    EXPECT_EQ(sink.data, payload);
}

// Cross-node pipeline: two remote stages bridged by the controller. Both run on
// one node (two connections/streams); the controller pipes stage0 stdout into
// stage1 stdin and closes it on stage0 exit, delivering stage1's stdout.
TEST(DistributedRunnerTest, BridgesTwoRemoteStages) {
    using psx::ca::CertificateAuthority;
    using psx::pipeline::DistributedRunner;
    using psx::pipeline::RemoteStage;

    auto ca = CertificateAuthority::create("psx-fleet");
    ASSERT_TRUE(ca.ok());
    auto nodeId = ca.value().issue("psx://node/1");
    auto ctlId = ca.value().issue("psx://controller");
    ASSERT_TRUE(nodeId.ok() && ctlId.ok());
    const std::string caCert = ca.value().certificatePem();

    auto listener = Socket::listen("127.0.0.1", 0);
    ASSERT_TRUE(listener.ok());
    const std::uint16_t port = listener.value().localPort().value();

    auto reactor = Reactor::create();
    ASSERT_TRUE(reactor.ok());
    Reactor& r = *reactor.value();

    NodeServer server(r, std::move(listener.value()),
                      {.certificatePem = nodeId.value().certificatePem,
                       .privateKeyPem = nodeId.value().privateKeyPem,
                       .caPem = caCert},
                      [](std::string_view san) { return san == "psx://controller"; });
    ASSERT_TRUE(server.start().ok());

    std::string output;
    DistributedRunner runner(
        r,
        {.certificatePem = ctlId.value().certificatePem, .privateKeyPem = ctlId.value().privateKeyPem, .caPem = caCert},
        [&](std::string_view chunk) { output.append(chunk); });

    const std::vector<RemoteStage> stages = {
        {.argv = {"printf", "b\na\nc\n"}, .host = "127.0.0.1", .port = port, .expectedSan = "psx://node/1"},
        {.argv = {"sort"}, .host = "127.0.0.1", .port = port, .expectedSan = "psx://node/1"}};

    DistributedRunner::Outcome outcome;
    bool completed = false;
    ASSERT_TRUE(runner
                    .run(stages,
                         [&](DistributedRunner::Outcome result) {
                             outcome = std::move(result);
                             completed = true;
                             r.stop();
                         })
                    .ok());
    r.after(std::chrono::seconds(10), [&] { r.stop(); });
    ASSERT_TRUE(r.run().ok());

    ASSERT_TRUE(completed);
    EXPECT_TRUE(outcome.error.empty()) << outcome.error;
    EXPECT_EQ(output, "a\nb\nc\n"); // stage1 (sort) output of stage0 (printf) input
    EXPECT_EQ(outcome.exitCode, 0);
    EXPECT_EQ(outcome.stageExitCodes, (std::vector<int>{0, 0}));
    EXPECT_EQ(outcome.stageLamportTimestamps, (std::vector<std::uint64_t>{1, 2}));
    EXPECT_EQ(runner.lamportClockValue(), 2U);
}

// pipefail across the wire: a failing downstream stage sets the pipeline code.
TEST(DistributedRunnerTest, PipefailAcrossRemoteStages) {
    using psx::ca::CertificateAuthority;
    using psx::pipeline::DistributedRunner;
    using psx::pipeline::RemoteStage;

    auto ca = CertificateAuthority::create("psx-fleet");
    ASSERT_TRUE(ca.ok());
    auto nodeId = ca.value().issue("psx://node/1");
    auto ctlId = ca.value().issue("psx://controller");
    ASSERT_TRUE(nodeId.ok() && ctlId.ok());
    const std::string caCert = ca.value().certificatePem();

    auto listener = Socket::listen("127.0.0.1", 0);
    ASSERT_TRUE(listener.ok());
    const std::uint16_t port = listener.value().localPort().value();

    auto reactor = Reactor::create();
    ASSERT_TRUE(reactor.ok());
    Reactor& r = *reactor.value();

    NodeServer server(r, std::move(listener.value()),
                      {.certificatePem = nodeId.value().certificatePem,
                       .privateKeyPem = nodeId.value().privateKeyPem,
                       .caPem = caCert},
                      {});
    ASSERT_TRUE(server.start().ok());

    DistributedRunner runner(r, {.certificatePem = ctlId.value().certificatePem,
                                 .privateKeyPem = ctlId.value().privateKeyPem,
                                 .caPem = caCert});
    const std::vector<RemoteStage> stages = {
        {.argv = {"echo", "hi"}, .host = "127.0.0.1", .port = port},
        {.argv = {"sh", "-c", "cat >/dev/null; exit 3"}, .host = "127.0.0.1", .port = port}};

    DistributedRunner::Outcome outcome;
    bool completed = false;
    ASSERT_TRUE(runner
                    .run(stages,
                         [&](DistributedRunner::Outcome result) {
                             outcome = std::move(result);
                             completed = true;
                             r.stop();
                         })
                    .ok());
    r.after(std::chrono::seconds(10), [&] { r.stop(); });
    ASSERT_TRUE(r.run().ok());

    ASSERT_TRUE(completed);
    EXPECT_EQ(outcome.stageExitCodes, (std::vector<int>{0, 3}));
    EXPECT_EQ(outcome.exitCode, 3);
}

namespace {
struct DistOutcome {
    std::string output;
    psx::pipeline::DistributedRunner::Outcome outcome;
    bool completed = false;
};

// Runs a remote pipeline (all stages on one fresh node) and returns the result.
DistOutcome runRemotePipeline(const std::vector<std::vector<std::string>>& commands,
                              int timeoutSeconds,
                              bool externalStdin = false,
                              const std::string& stdinInput = "") {
    using psx::ca::CertificateAuthority;
    using psx::pipeline::DistributedRunner;
    using psx::pipeline::RemoteStage;

    DistOutcome result;
    auto ca = CertificateAuthority::create("psx-fleet");
    EXPECT_TRUE(ca.ok());
    auto nodeId = ca.value().issue("psx://node/1");
    auto ctlId = ca.value().issue("psx://controller");
    EXPECT_TRUE(nodeId.ok() && ctlId.ok());
    const std::string caCert = ca.value().certificatePem();

    auto listener = Socket::listen("127.0.0.1", 0);
    EXPECT_TRUE(listener.ok());
    const std::uint16_t port = listener.value().localPort().value();

    auto reactor = Reactor::create();
    EXPECT_TRUE(reactor.ok());
    Reactor& r = *reactor.value();

    NodeServer server(r, std::move(listener.value()),
                      {.certificatePem = nodeId.value().certificatePem,
                       .privateKeyPem = nodeId.value().privateKeyPem,
                       .caPem = caCert},
                      {});
    EXPECT_TRUE(server.start().ok());

    auto runner = std::make_unique<DistributedRunner>(r,
                                                      psx::os::TlsConfig{.certificatePem = ctlId.value().certificatePem,
                                                                         .privateKeyPem = ctlId.value().privateKeyPem,
                                                                         .caPem = caCert},
                                                      [&](std::string_view chunk) { result.output.append(chunk); });

    std::vector<RemoteStage> stages;
    for (const auto& argv : commands) {
        stages.push_back({.argv = argv, .host = "127.0.0.1", .port = port, .expectedSan = ""});
    }
    EXPECT_TRUE(runner
                    ->run(
                        stages,
                        [&](DistributedRunner::Outcome outcome) {
                            result.outcome = std::move(outcome);
                            result.completed = true;
                            r.stop();
                        },
                        externalStdin)
                    .ok());
    if (externalStdin) {
        runner->writeStdin(stdinInput); // buffered until the streams open
        runner->closeStdin();
    }
    r.after(std::chrono::seconds(timeoutSeconds), [&] { r.stop(); });
    EXPECT_TRUE(r.run().ok());
    return result;
}
} // namespace

// The first stage has no upstream: its stdin must be EOF'd so a reader like `cat`
// does not block forever.
TEST(DistributedRunnerTest, FirstStageStdinIsClosedSoAReaderDoesNotHang) {
    auto result = runRemotePipeline({{"cat"}, {"cat"}}, /*timeoutSeconds=*/5);
    ASSERT_TRUE(result.completed) << "a stdin-reading first stage hung";
    EXPECT_TRUE(result.output.empty());
    EXPECT_EQ(result.outcome.exitCode, 0);
}

// An infinite upstream must not hang the pipeline. It is explicitly fenced and
// accounted as a known non-zero termination when the downstream exits.
TEST(DistributedRunnerTest, InfiniteUpstreamCompletesWhenDownstreamExits) {
    auto result = runRemotePipeline({{"yes"}, {"head", "-n", "3"}}, /*timeoutSeconds=*/10);
    ASSERT_TRUE(result.completed) << "an infinite upstream hung the pipeline";
    EXPECT_EQ(result.output, "y\ny\ny\n");
    EXPECT_EQ(result.outcome.stageExitCodes, (std::vector<int>{137, 0}));
    EXPECT_EQ(result.outcome.exitCode, 137);
}

TEST(DistributedRunnerTest, RightmostKnownFailureWinsAfterUpstreamIsFenced) {
    auto result = runRemotePipeline({{"yes"}, {"sh", "-c", "head -n 1 >/dev/null; exit 4"}},
                                    /*timeoutSeconds=*/10);
    ASSERT_TRUE(result.completed) << "a failing early consumer hung";
    EXPECT_EQ(result.outcome.stageExitCodes, (std::vector<int>{137, 4}));
    EXPECT_EQ(result.outcome.exitCode, 4);
}

TEST(DistributedRunnerTest, FeedsExternalStdinToTheFirstRemoteStage) {
    auto result = runRemotePipeline({{"cat"}}, /*timeoutSeconds=*/5, /*externalStdin=*/true, "remote stdin here\n");
    ASSERT_TRUE(result.completed);
    EXPECT_EQ(result.output, "remote stdin here\n");
    EXPECT_EQ(result.outcome.exitCode, 0);
}

TEST(DistributedRunnerTest, ExternalStdinFlowsThroughARemoteChain) {
    auto result = runRemotePipeline({{"cat"}, {"tr", "a-z", "A-Z"}}, 5, /*externalStdin=*/true, "mixed CASE text\n");
    ASSERT_TRUE(result.completed);
    EXPECT_EQ(result.output, "MIXED CASE TEXT\n");
    EXPECT_EQ(result.outcome.exitCode, 0);
}

namespace {
struct SegStage {
    std::vector<std::string> argv;
    bool local;
};
struct SegResult {
    std::string output;
    int exitCode = -1;
    std::vector<int> stageExitCodes;
    bool completed = false;
    std::string error;
};

// Runs a mixed pipeline: remote stages go to one fresh node, local stages run
// as processes. Returns the final output + outcome.
SegResult runSegmented(const std::vector<SegStage>& stages, int timeoutSeconds) {
    using psx::ca::CertificateAuthority;
    using psx::pipeline::ResolvedStage;
    using psx::pipeline::SegmentedPipeline;

    SegResult result;
    auto ca = CertificateAuthority::create("psx-fleet");
    EXPECT_TRUE(ca.ok());
    auto nodeId = ca.value().issue("psx://node/1");
    auto ctlId = ca.value().issue("psx://controller");
    EXPECT_TRUE(nodeId.ok() && ctlId.ok());
    const std::string caCert = ca.value().certificatePem();

    auto listener = Socket::listen("127.0.0.1", 0);
    EXPECT_TRUE(listener.ok());
    const std::uint16_t port = listener.value().localPort().value();

    auto reactor = Reactor::create();
    EXPECT_TRUE(reactor.ok());
    Reactor& r = *reactor.value();

    NodeServer server(r, std::move(listener.value()),
                      {.certificatePem = nodeId.value().certificatePem,
                       .privateKeyPem = nodeId.value().privateKeyPem,
                       .caPem = caCert},
                      {});
    EXPECT_TRUE(server.start().ok());

    auto runner = std::make_unique<SegmentedPipeline>(r,
                                                      psx::os::TlsConfig{.certificatePem = ctlId.value().certificatePem,
                                                                         .privateKeyPem = ctlId.value().privateKeyPem,
                                                                         .caPem = caCert},
                                                      [&](std::string_view chunk) { result.output.append(chunk); });

    std::vector<ResolvedStage> resolved;
    for (const SegStage& s : stages) {
        if (s.local) {
            resolved.push_back({.argv = s.argv, .host = "", .port = 0, .expectedSan = ""});
        } else {
            resolved.push_back({.argv = s.argv, .host = "127.0.0.1", .port = port, .expectedSan = ""});
        }
    }
    EXPECT_TRUE(runner
                    ->run(resolved,
                          [&](SegmentedPipeline::Outcome outcome) {
                              result.exitCode = outcome.exitCode;
                              result.stageExitCodes = std::move(outcome.stageExitCodes);
                              result.error = outcome.error;
                              result.completed = true;
                              r.stop();
                          })
                    .ok());
    r.after(std::chrono::seconds(timeoutSeconds), [&] { r.stop(); });
    EXPECT_TRUE(r.run().ok());
    return result;
}
} // namespace

// Remote gather -> local process (the common §5 shape).
TEST(SegmentedPipelineTest, RemoteThenLocal) {
    auto result = runSegmented({{{"printf", "b\na\nc\n"}, /*local=*/false}, {{"sort"}, /*local=*/true}}, 10);
    ASSERT_TRUE(result.completed) << result.error;
    EXPECT_EQ(result.output, "a\nb\nc\n");
    EXPECT_EQ(result.exitCode, 0);
}

// Local source -> remote sink.
TEST(SegmentedPipelineTest, LocalThenRemote) {
    auto result = runSegmented({{{"printf", "3\n1\n2\n"}, /*local=*/true}, {{"sort"}, /*local=*/false}}, 10);
    ASSERT_TRUE(result.completed) << result.error;
    EXPECT_EQ(result.output, "1\n2\n3\n");
    EXPECT_EQ(result.exitCode, 0);
}

// local -> remote -> local (three segments).
TEST(SegmentedPipelineTest, LocalRemoteLocalThreeSegments) {
    auto result = runSegmented({{{"printf", "b\na\nc\n"}, true}, {{"sort"}, false}, {{"tr", "a-z", "A-Z"}, true}}, 10);
    ASSERT_TRUE(result.completed) << result.error;
    EXPECT_EQ(result.output, "A\nB\nC\n");
    EXPECT_EQ(result.exitCode, 0);
}

TEST(SegmentedPipelineTest, EarlyRemoteConsumerFencesAndAccountsInfiniteLocalUpstream) {
    auto result = runSegmented({{{"yes"}, /*local=*/true}, {{"head", "-n", "3"}, /*local=*/false}}, 10);
    ASSERT_TRUE(result.completed) << "an infinite local segment hung: " << result.error;
    EXPECT_EQ(result.output, "y\ny\ny\n");
    EXPECT_EQ(result.stageExitCodes, (std::vector<int>{137, 0}));
    EXPECT_EQ(result.exitCode, 137);
}

TEST(SegmentedPipelineTest, EarlyLocalConsumerFencesAndAccountsInfiniteRemoteUpstream) {
    auto result = runSegmented({{{"yes"}, /*local=*/false}, {{"head", "-n", "3"}, /*local=*/true}}, 10);
    ASSERT_TRUE(result.completed) << "an infinite remote segment hung: " << result.error;
    EXPECT_EQ(result.output, "y\ny\ny\n");
    EXPECT_EQ(result.stageExitCodes, (std::vector<int>{137, 0}));
    EXPECT_EQ(result.exitCode, 137);
}

// Dual-stack hostname: `localhost` resolves to ::1 (first) and 127.0.0.1. With
// an IPv4-only node, the connect must fall back from the failing ::1 to IPv4.
TEST(NativeTransportTest, ConnectsToADualStackHostnameViaFallback) {
    using psx::ca::CertificateAuthority;
    auto ca = CertificateAuthority::create("psx-fleet");
    ASSERT_TRUE(ca.ok());
    auto nodeId = ca.value().issue("psx://node/1");
    auto ctlId = ca.value().issue("psx://controller");
    ASSERT_TRUE(nodeId.ok() && ctlId.ok());
    const std::string caCert = ca.value().certificatePem();

    auto listener = Socket::listen("127.0.0.1", 0); // IPv4 only
    ASSERT_TRUE(listener.ok());
    const std::uint16_t port = listener.value().localPort().value();

    auto reactor = Reactor::create();
    ASSERT_TRUE(reactor.ok());
    Reactor& r = *reactor.value();

    NodeServer server(r, std::move(listener.value()),
                      {.certificatePem = nodeId.value().certificatePem,
                       .privateKeyPem = nodeId.value().privateKeyPem,
                       .caPem = caCert},
                      {});
    ASSERT_TRUE(server.start().ok());

    // Connect by hostname: ::1 is tried first and refused; the transport falls
    // back to 127.0.0.1 while the reactor runs (NodeServer accepts there).
    auto clientSock = Socket::connect("localhost", port);
    ASSERT_TRUE(clientSock.ok());
    auto ctlTls = Tls::create({.certificatePem = ctlId.value().certificatePem,
                               .privateKeyPem = ctlId.value().privateKeyPem,
                               .caPem = caCert,
                               .isServer = false});
    ASSERT_TRUE(ctlTls.ok());

    ConsumingSink sink;
    sink.reactor = &r;
    bool errored = false;
    NativeTransport* ctlPtr = nullptr;
    NativeTransport ctl(r, std::move(clientSock.value()), std::move(ctlTls.value()), Role::Controller, sink,
                        {.onReady = [&] { ctlPtr->session().open({.argv = {"/bin/echo", "fallback-ok"}}); },
                         .onError =
                             [&](psx::Error) {
                                 errored = true;
                                 r.stop();
                             }});
    sink.session = &ctl.session();
    ctlPtr = &ctl;
    ASSERT_TRUE(ctl.start().ok());
    r.after(std::chrono::seconds(5), [&] { r.stop(); });
    ASSERT_TRUE(r.run().ok());

    EXPECT_FALSE(errored) << "connecting to localhost must fall back to IPv4";
    ASSERT_TRUE(sink.exited);
    EXPECT_EQ(sink.data, "fallback-ok\n");
}

// A connect that exhausts every resolved address fails cleanly via onError.
TEST(NativeTransportTest, ConnectToAClosedPortFailsCleanly) {
    auto probe = Socket::listen("127.0.0.1", 0);
    ASSERT_TRUE(probe.ok());
    const std::uint16_t closedPort = probe.value().localPort().value();
    probe = Socket{}; // close the listener: the port is now refused

    const auto ctlId = tls_test::generateSelfSigned("psx://controller");
    const auto nodeId = tls_test::generateSelfSigned("psx://node/1");
    auto ctlTls = Tls::create(
        {.certificatePem = ctlId.certPem, .privateKeyPem = ctlId.keyPem, .caPem = nodeId.certPem, .isServer = false});
    ASSERT_TRUE(ctlTls.ok());
    auto clientSock = Socket::connect("127.0.0.1", closedPort);
    ASSERT_TRUE(clientSock.ok()); // async: the failure surfaces later

    auto reactor = Reactor::create();
    ASSERT_TRUE(reactor.ok());
    Reactor& r = *reactor.value();

    bool errored = false;
    bool ready = false;
    struct Sink : SessionHandler {
    } sink;
    NativeTransport ctl(r, std::move(clientSock.value()), std::move(ctlTls.value()), Role::Controller, sink,
                        {.onReady =
                             [&] {
                                 ready = true;
                                 r.stop();
                             },
                         .onError =
                             [&](psx::Error) {
                                 errored = true;
                                 r.stop();
                             }});
    ASSERT_TRUE(ctl.start().ok());
    r.after(std::chrono::seconds(5), [&] { r.stop(); });
    ASSERT_TRUE(r.run().ok());

    EXPECT_TRUE(errored) << "a refused connect must surface as onError";
    EXPECT_FALSE(ready);
}

namespace {
// Runs a fan-in pipeline: `sourceArgv` on `fanHosts` copies of one node, merged
// into `downstream`. Returns the final output + outcome.
struct FanResult {
    std::string output;
    int exitCode = -1;
    bool completed = false;
    std::string error;
};
FanResult runFanIn(const std::vector<std::string>& sourceArgv,
                   int fanHosts,
                   const std::vector<psx::pipeline::ResolvedStage>& downstream,
                   int timeoutSeconds) {
    using psx::ca::CertificateAuthority;
    using psx::pipeline::FanInPipeline;

    FanResult result;
    auto ca = CertificateAuthority::create("psx-fleet");
    EXPECT_TRUE(ca.ok());
    auto nodeId = ca.value().issue("psx://node/1");
    auto ctlId = ca.value().issue("psx://controller");
    EXPECT_TRUE(nodeId.ok() && ctlId.ok());
    const std::string caCert = ca.value().certificatePem();

    auto listener = Socket::listen("127.0.0.1", 0);
    EXPECT_TRUE(listener.ok());
    const std::uint16_t port = listener.value().localPort().value();

    auto reactor = Reactor::create();
    EXPECT_TRUE(reactor.ok());
    Reactor& r = *reactor.value();

    NodeServer server(r, std::move(listener.value()),
                      {.certificatePem = nodeId.value().certificatePem,
                       .privateKeyPem = nodeId.value().privateKeyPem,
                       .caPem = caCert},
                      {});
    EXPECT_TRUE(server.start().ok());

    auto runner = std::make_unique<FanInPipeline>(r,
                                                  psx::os::TlsConfig{.certificatePem = ctlId.value().certificatePem,
                                                                     .privateKeyPem = ctlId.value().privateKeyPem,
                                                                     .caPem = caCert},
                                                  [&](std::string_view chunk) { result.output.append(chunk); });

    std::vector<psx::transport::NativeController::Target> targets(
        static_cast<std::size_t>(fanHosts),
        psx::transport::NativeController::Target{.host = "127.0.0.1", .port = port, .expectedSan = ""});

    EXPECT_TRUE(runner
                    ->run(sourceArgv, targets, downstream,
                          [&](FanInPipeline::Outcome outcome) {
                              result.exitCode = outcome.exitCode;
                              result.error = outcome.error;
                              result.completed = true;
                              r.stop();
                          })
                    .ok());
    r.after(std::chrono::seconds(timeoutSeconds), [&] { r.stop(); });
    EXPECT_TRUE(r.run().ok());
    return result;
}
} // namespace

// Fan-in with no downstream: the sources' merged output is the result.
TEST(FanInPipelineTest, MergesSourcesWithNoDownstream) {
    auto result = runFanIn({"echo", "shard"}, /*fanHosts=*/2, /*downstream=*/{}, 10);
    ASSERT_TRUE(result.completed) << result.error;
    EXPECT_EQ(result.output, "shard\nshard\n"); // both hosts printed "shard\n"
    EXPECT_EQ(result.exitCode, 0);
}

// Fan-in feeding a local downstream (the §5.2 `grep@shards | sort@local` shape).
TEST(FanInPipelineTest, MergesSourcesIntoALocalDownstream) {
    // 3 hosts each emit "line\n"; the local sort receives all 3 lines.
    std::vector<psx::pipeline::ResolvedStage> downstream = {
        {.argv = {"wc", "-l"}, .host = "", .port = 0, .expectedSan = ""}};
    auto result = runFanIn({"echo", "line"}, /*fanHosts=*/3, downstream, 10);
    ASSERT_TRUE(result.completed) << result.error;
    EXPECT_NE(result.output.find("3"), std::string::npos) << "wc -l should count 3 merged lines";
    EXPECT_EQ(result.exitCode, 0);
}

TEST(FanInPipelineTest, EarlyDownstreamExitWaitsForAndAccountsSourceCancellation) {
    std::vector<psx::pipeline::ResolvedStage> downstream = {
        {.argv = {"head", "-n", "3"}, .host = "", .port = 0, .expectedSan = ""}};
    auto result = runFanIn({"yes"}, /*fanHosts=*/2, downstream, 10);
    ASSERT_TRUE(result.completed) << "infinite fan-in sources hung: " << result.error;
    EXPECT_EQ(result.output, "y\ny\ny\n");
    EXPECT_TRUE(result.error.empty()) << result.error;
    EXPECT_EQ(result.exitCode, 137);
}

TEST(FanInPipelineTest, RightmostDownstreamFailureWinsAfterSourceCancellation) {
    std::vector<psx::pipeline::ResolvedStage> downstream = {
        {.argv = {"sh", "-c", "head -n 1 >/dev/null; exit 4"}, .host = "", .port = 0, .expectedSan = ""}};
    auto result = runFanIn({"yes"}, /*fanHosts=*/2, downstream, 10);
    ASSERT_TRUE(result.completed) << "failing fan-in consumer hung: " << result.error;
    EXPECT_TRUE(result.error.empty()) << result.error;
    EXPECT_EQ(result.exitCode, 4);
}

namespace {
struct CanaryResult {
    std::vector<psx::transport::CanaryController::HostResult> results;
    std::uint64_t acceptedByNode = 0;
    bool completed = false;
};
// Runs a canary rollout of `command` over `numTargets` connections to one node.
CanaryResult runCanary(const std::vector<std::string>& command, std::size_t canaryCount, int numTargets) {
    using psx::ca::CertificateAuthority;
    using psx::transport::CanaryController;

    CanaryResult out;
    auto ca = CertificateAuthority::create("psx-fleet");
    EXPECT_TRUE(ca.ok());
    auto nodeId = ca.value().issue("psx://node/1");
    auto ctlId = ca.value().issue("psx://controller");
    EXPECT_TRUE(nodeId.ok() && ctlId.ok());
    const std::string caCert = ca.value().certificatePem();

    auto listener = Socket::listen("127.0.0.1", 0);
    EXPECT_TRUE(listener.ok());
    const std::uint16_t port = listener.value().localPort().value();

    auto reactor = Reactor::create();
    EXPECT_TRUE(reactor.ok());
    Reactor& r = *reactor.value();

    NodeServer server(r, std::move(listener.value()),
                      {.certificatePem = nodeId.value().certificatePem,
                       .privateKeyPem = nodeId.value().privateKeyPem,
                       .caPem = caCert},
                      {});
    EXPECT_TRUE(server.start().ok());

    auto controller =
        std::make_unique<CanaryController>(r, psx::os::TlsConfig{.certificatePem = ctlId.value().certificatePem,
                                                                 .privateKeyPem = ctlId.value().privateKeyPem,
                                                                 .caPem = caCert});
    std::vector<CanaryController::Target> targets(
        static_cast<std::size_t>(numTargets),
        CanaryController::Target{.host = "127.0.0.1", .port = port, .expectedSan = ""});

    EXPECT_TRUE(controller
                    ->start(targets, canaryCount, command,
                            [&](std::vector<CanaryController::HostResult> results) {
                                out.results = std::move(results);
                                out.completed = true;
                                r.stop();
                            })
                    .ok());
    r.after(std::chrono::seconds(10), [&] { r.stop(); });
    EXPECT_TRUE(r.run().ok());
    out.acceptedByNode = server.metrics().acceptedTotal;
    return out;
}
} // namespace

// Canary succeeds: the rest of the fleet is rolled out too.
TEST(CanaryControllerTest, SuccessfulCanaryRollsOutTheRest) {
    auto out = runCanary({"/bin/echo", "ok"}, /*canaryCount=*/1, /*numTargets=*/3);
    ASSERT_TRUE(out.completed);
    ASSERT_EQ(out.results.size(), 3U);
    for (const auto& res : out.results) {
        EXPECT_TRUE(res.ok);
        EXPECT_EQ(res.exitCode, 0);
    }
    EXPECT_EQ(out.acceptedByNode, 3U); // all hosts ran
}

// Canary fails: the rest are skipped, never contacted.
TEST(CanaryControllerTest, FailedCanarySkipsTheRest) {
    auto out = runCanary({"/bin/sh", "-c", "exit 1"}, /*canaryCount=*/1, /*numTargets=*/3);
    ASSERT_TRUE(out.completed);
    ASSERT_EQ(out.results.size(), 3U);
    EXPECT_TRUE(out.results[0].ok);        // the canary ran...
    EXPECT_EQ(out.results[0].exitCode, 1); // ...but exited non-zero (a failure)
    for (std::size_t i = 1; i < out.results.size(); ++i) {
        EXPECT_FALSE(out.results[i].ok);
        EXPECT_NE(out.results[i].error.find("canary"), std::string::npos); // skipped
    }
    EXPECT_EQ(out.acceptedByNode, 1U) << "only the canary connected; the rest were skipped";
}

// Lamport clock happens-before verification:
// When the controller dispatches multiple stages across nodes, each stage dispatch
// ticks the controller's clock, yielding strictly monotonic timestamps (1, 2, 3),
// and the node observes the received timestamp, ensuring that local stage execution
// happens strictly after dispatch.
TEST(LamportClockIntegrationTest, StageDispatchesMaintainStrictHappensBeforeOrder) {
    Fleet fleet = makeFleet();

    auto listener = Socket::listen("127.0.0.1", 0);
    ASSERT_TRUE(listener.ok());
    const std::uint16_t port = listener.value().localPort().value();

    auto reactor = Reactor::create();
    ASSERT_TRUE(reactor.ok());
    Reactor& r = *reactor.value();

    psx::transport::NodeServer server(r, std::move(listener.value()),
                                      {.certificatePem = fleet.nodeId.certificatePem,
                                       .privateKeyPem = fleet.nodeId.privateKeyPem,
                                       .caPem = fleet.caCert},
                                      [](std::string_view san) { return san == "psx://controller"; });
    ASSERT_TRUE(server.start().ok());

    psx::transport::NativeController controller(r, {.certificatePem = fleet.ctlId.certificatePem,
                                                    .privateKeyPem = fleet.ctlId.privateKeyPem,
                                                    .caPem = fleet.caCert});

    const std::vector<NativeController::Target> targets = {
        {.host = "127.0.0.1", .port = port, .expectedSan = "psx://node/1"},
        {.host = "127.0.0.1", .port = port, .expectedSan = "psx://node/1"},
        {.host = "127.0.0.1", .port = port, .expectedSan = "psx://node/1"}};

    std::vector<NativeController::HostResult> results;
    ASSERT_TRUE(controller
                    .start(targets, {"/bin/echo", "ds-test"},
                           [&](std::vector<NativeController::HostResult> res) {
                               results = std::move(res);
                               r.stop();
                           },
                           {.concurrency = 1}) // serialized dispatch
                    .ok());

    r.after(std::chrono::seconds(10), [&] { r.stop(); });
    ASSERT_TRUE(r.run().ok());

    ASSERT_EQ(results.size(), 3U);
    for (std::size_t i = 0; i < results.size(); ++i) {
        EXPECT_TRUE(results[i].ok) << results[i].error;
        EXPECT_EQ(results[i].exitCode, 0);
        EXPECT_EQ(results[i].lamportTs, static_cast<std::uint64_t>(i + 1));
    }
    // Strict partial/total order check: ts(dispatch_0) < ts(dispatch_1) < ts(dispatch_2)
    EXPECT_LT(results[0].lamportTs, results[1].lamportTs);
    EXPECT_LT(results[1].lamportTs, results[2].lamportTs);
}

// Cluster snapshot verification (simplified Chandy-Lamport local state capture):
// As nodes start and finish their assigned stages, NativeController captures
// point-in-time cluster states (JSONL lines with host, stageId, status, exit code, lamportTs).
TEST(ClusterSnapshotIntegrationTest, ControllerRecordsNodeSnapshotsToDisk) {
    test_support::ScopedTempCwd cwd("cluster-snapshot-test");
    const std::string snapshotFile = (cwd.path() / "snapshots.jsonl").string();

    Fleet fleet = makeFleet();

    auto listener = Socket::listen("127.0.0.1", 0);
    ASSERT_TRUE(listener.ok());
    const std::uint16_t port = listener.value().localPort().value();

    auto reactor = Reactor::create();
    ASSERT_TRUE(reactor.ok());
    Reactor& r = *reactor.value();

    psx::transport::NodeServer server(r, std::move(listener.value()),
                                      {.certificatePem = fleet.nodeId.certificatePem,
                                       .privateKeyPem = fleet.nodeId.privateKeyPem,
                                       .caPem = fleet.caCert},
                                      [](std::string_view san) { return san == "psx://controller"; });
    ASSERT_TRUE(server.start().ok());

    psx::transport::NativeController controller(r, {.certificatePem = fleet.ctlId.certificatePem,
                                                    .privateKeyPem = fleet.ctlId.privateKeyPem,
                                                    .caPem = fleet.caCert});

    const std::vector<NativeController::Target> targets = {
        {.host = "127.0.0.1", .port = port, .expectedSan = "psx://node/1"}};

    std::vector<NativeController::HostResult> results;
    ASSERT_TRUE(controller
                    .start(targets, {"/bin/echo", "snap-test"},
                           [&](std::vector<NativeController::HostResult> res) {
                               results = std::move(res);
                               r.stop();
                           },
                           {.snapshotPath = snapshotFile, .runId = "run-snap-01"})
                    .ok());

    r.after(std::chrono::seconds(5), [&] { r.stop(); });
    ASSERT_TRUE(r.run().ok());

    ASSERT_EQ(results.size(), 1U);
    EXPECT_TRUE(results[0].ok);

    // In-memory captureSnapshot() verification:
    auto snap = controller.captureSnapshot();
    ASSERT_EQ(snap.nodes().size(), 1U);
    EXPECT_EQ(snap.nodes()[0].host, "127.0.0.1");
    EXPECT_EQ(snap.nodes()[0].status, "exited");
    EXPECT_EQ(snap.nodes()[0].exitCode, 0);
    EXPECT_EQ(snap.nodes()[0].lamportTs, 1U);

    // On-disk JSONL verification:
    std::ifstream in(snapshotFile);
    ASSERT_TRUE(in.good());
    std::vector<std::string> lines;
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty()) {
            lines.push_back(line);
        }
    }
    ASSERT_FALSE(lines.empty());
    for (const auto& l : lines) {
        EXPECT_NE(l.find(R"("type":"cluster_snapshot")"), std::string::npos);
        EXPECT_NE(l.find(R"("run_id":"run-snap-01")"), std::string::npos);
        EXPECT_NE(l.find(R"("127.0.0.1")"), std::string::npos);
    }
    const std::string& lastLine = lines.back();
    EXPECT_NE(lastLine.find(R"("status":"exited")"), std::string::npos);
    EXPECT_NE(lastLine.find(R"("exit_code":0)"), std::string::npos);
    EXPECT_NE(lastLine.find(R"("lamport_ts":1)"), std::string::npos);
}

// A Lamport clock belongs to a process. Two controllers on two separate
// connections to the same node must advance ONE node clock: A's OPEN(ts=1)
// moves it to 2, then B's OPEN(ts=1) must move it to max(2,1)+1 = 3. With a
// clock per connection (the earlier bug) the node would report 2.
TEST(LamportClockIntegrationTest, NodeClockIsSharedAcrossControllerConnections) {
    Fleet fleet = makeFleet();
    auto listener = Socket::listen("127.0.0.1", 0);
    ASSERT_TRUE(listener.ok());
    const std::uint16_t port = listener.value().localPort().value();
    auto reactor = Reactor::create();
    ASSERT_TRUE(reactor.ok());
    Reactor& r = *reactor.value();

    NodeServer server(r, std::move(listener.value()),
                      {.certificatePem = fleet.nodeId.certificatePem,
                       .privateKeyPem = fleet.nodeId.privateKeyPem,
                       .caPem = fleet.caCert},
                      [](std::string_view san) { return san == "psx://controller"; });
    ASSERT_TRUE(server.start().ok());

    const psx::os::TlsConfig ctlConfig{.certificatePem = fleet.ctlId.certificatePem,
                                       .privateKeyPem = fleet.ctlId.privateKeyPem,
                                       .caPem = fleet.caCert};
    const std::vector<NativeController::Target> target = {
        {.host = "127.0.0.1", .port = port, .expectedSan = "psx://node/1"}};

    for (int i = 0; i < 2; ++i) { // two independent controllers, each with a fresh clock
        NativeController controller(r, ctlConfig);
        std::vector<NativeController::HostResult> results;
        ASSERT_TRUE(controller
                        .start(target, {"/bin/true"},
                               [&](std::vector<NativeController::HostResult> res) {
                                   results = std::move(res);
                                   r.stop();
                               })
                        .ok());
        r.after(std::chrono::seconds(5), [&] { r.stop(); });
        ASSERT_TRUE(r.run().ok());
        ASSERT_EQ(results.size(), 1U);
        ASSERT_TRUE(results[0].ok) << results[0].error;
        EXPECT_EQ(results[0].lamportTs, 1U); // each controller's first dispatch
    }
    EXPECT_EQ(server.metrics().lamportClock, 3U);
}

// Never-dispatched targets must not be stamped with a logical time.
TEST(ClusterSnapshotIntegrationTest, UndispatchedTargetHasNoLamportTimestamp) {
    Fleet fleet = makeFleet();
    auto reactor = Reactor::create();
    ASSERT_TRUE(reactor.ok());
    Reactor& r = *reactor.value();
    NativeController controller(r, {.certificatePem = fleet.ctlId.certificatePem,
                                    .privateKeyPem = fleet.ctlId.privateKeyPem,
                                    .caPem = fleet.caCert});
    std::vector<NativeController::HostResult> results;
    ASSERT_TRUE(controller
                    .start({{.host = "127.0.0.1", .port = 1, .expectedSan = "psx://node/1"}}, {"/bin/true"},
                           [&](std::vector<NativeController::HostResult> res) {
                               results = std::move(res);
                               r.stop();
                           })
                    .ok());
    r.after(std::chrono::seconds(5), [&] { r.stop(); });
    ASSERT_TRUE(r.run().ok());
    auto snap = controller.captureSnapshot();
    ASSERT_EQ(snap.nodes().size(), 1U);
    EXPECT_EQ(snap.nodes()[0].status, "lost");
    EXPECT_EQ(snap.nodes()[0].lamportTs, 0U);
}


