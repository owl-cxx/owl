#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <format>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <openssl/ssl.h>

#include <coro/task.h>

#include <owl/http/response.h>
#include <owl/routing/router.h>
#include <owl/server.h>
#include <owl/ws/socket.h>

#include "support/live_worker.h"
#include "support/test_cert.h"

namespace {
    using owl_test::LiveWorker;
    using owl_test::TestCert;

    struct AppState final {
    };

    owl::Response version(owl::RequestView req) {
        return owl::Response::ok(std::format("{:#x}", req->raw()->version));
    }

    coro::task<void> echo(owl::ws::Socket sock) {
        while (auto msg = co_await sock.recv()) co_await sock.send(std::string{msg->data()}, msg->opcode());
    }

    constexpr std::string_view kGetVersion = "GET /version HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";

    struct Tcp final {
        int fd = -1;

        explicit Tcp(const std::uint16_t port) {
            fd = ::socket(AF_INET, SOCK_STREAM, 0);
            if (fd < 0) throw std::runtime_error("client: socket");
            const timeval tv{.tv_sec = 5, .tv_usec = 0};
            ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
            sockaddr_in addr{};
            addr.sin_family = AF_INET;
            addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            addr.sin_port = htons(port);
            if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
                ::close(fd);
                throw std::runtime_error("client: connect");
            }
        }

        ~Tcp() {
            ::close(fd);
        }

        Tcp(const Tcp&) = delete;
        Tcp& operator=(const Tcp&) = delete;

        void send(const std::string_view data) const {
            (void)::send(fd, data.data(), data.size(), 0);
        }

        [[nodiscard]] std::string read_until_close() const {
            std::string out;
            char tmp[1024];
            for (;;) {
                const auto n = ::recv(fd, tmp, sizeof(tmp), 0);
                if (n <= 0) return out;
                out.append(tmp, static_cast<std::size_t>(n));
            }
        }
    };

    struct ClientOptions final {
        std::string ca;
        std::vector<unsigned char> alpn{};
        int max_version = 0;
    };

    // Trusts the test certificate as its own root and checks the name, so a
    // handshake that succeeds has verified what the server presented.
    struct TlsClient final {
        Tcp tcp;
        SSL_CTX* ctx = nullptr;
        SSL* ssl = nullptr;
        bool connected = false;

        TlsClient(const std::uint16_t port, const ClientOptions& options) : tcp(port) {
            ctx = SSL_CTX_new(TLS_client_method());
            if (options.max_version != 0) {
                // A stock client no longer offers TLS 1.1 at all; this one has
                // to, for the refusal to be the server's.
                SSL_CTX_set_security_level(ctx, 0);
                SSL_CTX_set_cipher_list(ctx, "ALL:@SECLEVEL=0");
                SSL_CTX_set_min_proto_version(ctx, TLS1_VERSION);
                SSL_CTX_set_max_proto_version(ctx, options.max_version);
            }
            SSL_CTX_load_verify_locations(ctx, options.ca.c_str(), nullptr);
            SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, nullptr);
            ssl = SSL_new(ctx);
            SSL_set1_host(ssl, "localhost");
            SSL_set_tlsext_host_name(ssl, "localhost");
            if (!options.alpn.empty()) {
                SSL_set_alpn_protos(ssl, options.alpn.data(), static_cast<unsigned>(options.alpn.size()));
            }
            SSL_set_fd(ssl, tcp.fd);
            connected = SSL_connect(ssl) == 1;
        }

        ~TlsClient() {
            if (connected) SSL_shutdown(ssl);
            SSL_free(ssl);
            SSL_CTX_free(ctx);
        }

        TlsClient(const TlsClient&) = delete;
        TlsClient& operator=(const TlsClient&) = delete;

        [[nodiscard]] std::string alpn() const {
            const unsigned char* data = nullptr;
            unsigned len = 0;
            SSL_get0_alpn_selected(ssl, &data, &len);
            return {reinterpret_cast<const char*>(data), len};
        }

        void send(const std::string_view data) const {
            (void)SSL_write(ssl, data.data(), static_cast<int>(data.size()));
        }

        [[nodiscard]] std::string read_until_close() const {
            std::string out;
            char tmp[1024];
            for (;;) {
                const int n = SSL_read(ssl, tmp, sizeof(tmp));
                if (n <= 0) return out;
                out.append(tmp, static_cast<std::size_t>(n));
            }
        }

        [[nodiscard]] std::string read_head() const {
            std::string out;
            char c = 0;
            while (!out.ends_with("\r\n\r\n") && SSL_read(ssl, &c, 1) == 1) out.push_back(c);
            return out;
        }

        [[nodiscard]] std::string take(const std::size_t n) const {
            std::string out;
            char c = 0;
            while (out.size() < n && SSL_read(ssl, &c, 1) == 1) out.push_back(c);
            return out;
        }
    };

    const std::vector<unsigned char> kH2ThenHttp11 = {2, 'h', '2', 8, 'h', 't', 't', 'p', '/', '1', '.', '1'};

    void build(owl::Tls tls) {
        const auto server = owl::Server<AppState>::builder()
                                .router(owl::Router<AppState>::make())
                                .config({.port = 0, .tls = std::move(tls)})
                                .build_with(std::make_shared<AppState>());
        EXPECT_NE(server.port(), 0);
    }
}

TEST(Tls, HttpsGetIsServed) {
    const TestCert cert;
    const owl::Tls tls = cert.tls();
    auto router = owl::Router<AppState>::make().route<"/version">(owl::get(version));
    LiveWorker worker{router, 1, &tls};
    const TlsClient client{worker.port, {.ca = cert.cert}};
    ASSERT_TRUE(client.connected);
    EXPECT_GE(SSL_version(client.ssl), TLS1_2_VERSION);
    client.send(kGetVersion);
    const std::string response = client.read_until_close();
    EXPECT_TRUE(response.starts_with("HTTP/1.1 200")) << response;
    EXPECT_TRUE(response.ends_with("0x101")) << response;
}

TEST(Tls, AlpnAnswersHttp11WhenH2IsOffered) {
    const TestCert cert;
    const owl::Tls tls = cert.tls();
    auto router = owl::Router<AppState>::make().route<"/version">(owl::get(version));
    LiveWorker worker{router, 1, &tls};
    const TlsClient client{worker.port, {.ca = cert.cert, .alpn = kH2ThenHttp11}};
    ASSERT_TRUE(client.connected);
    EXPECT_EQ(client.alpn(), "http/1.1");
    client.send(kGetVersion);
    const std::string response = client.read_until_close();
    EXPECT_TRUE(response.starts_with("HTTP/1.1 200")) << response;
    EXPECT_TRUE(response.ends_with("0x101")) << response;
}

// Pins the outcome, not the layer: OpenSSL's default security level refuses
// TLS 1.1 on its own, and the server's explicit minimum is what still holds
// on a host whose openssl.cnf lowers that level.
TEST(Tls, Tls11IsRefused) {
    const TestCert cert;
    const owl::Tls tls = cert.tls();
    auto router = owl::Router<AppState>::make().route<"/version">(owl::get(version));
    LiveWorker worker{router, 1, &tls};
    const TlsClient old{worker.port, {.ca = cert.cert, .max_version = TLS1_1_VERSION}};
    EXPECT_FALSE(old.connected);
}

TEST(Tls, WebSocketEchoesOverTls) {
    const TestCert cert;
    const owl::Tls tls = cert.tls();
    auto router = owl::Router<AppState>::make().ws<"/echo">(echo);
    LiveWorker worker{router, 1, &tls};
    const TlsClient client{worker.port, {.ca = cert.cert}};
    ASSERT_TRUE(client.connected);
    client.send("GET /echo HTTP/1.1\r\nHost: localhost\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n");
    ASSERT_TRUE(client.read_head().starts_with("HTTP/1.1 101"));
    // A masked text frame "hi" under the key 01 02 03 04.
    client.send(std::string_view{"\x81\x82\x01\x02\x03\x04\x69\x6b", 8});
    EXPECT_EQ(client.take(4), "\x81\x02hi");
}

TEST(Tls, PlaintextOnTheTlsPortGetsNoHttpAndTheServerLives) {
    const TestCert cert;
    const owl::Tls tls = cert.tls();
    auto router = owl::Router<AppState>::make().route<"/version">(owl::get(version));
    LiveWorker worker{router, 1, &tls};
    {
        const Tcp plain{worker.port};
        plain.send(kGetVersion);
        EXPECT_FALSE(plain.read_until_close().starts_with("HTTP/"));
    }
    const TlsClient client{worker.port, {.ca = cert.cert}};
    ASSERT_TRUE(client.connected);
    client.send(kGetVersion);
    EXPECT_TRUE(client.read_until_close().starts_with("HTTP/1.1 200"));
}

TEST(TlsBuilder, ACertificateAndItsKeyBuild) {
    const TestCert cert;
    EXPECT_NO_THROW(build(cert.tls()));
}

TEST(TlsBuilder, MissingCertificateFileThrows) {
    const TestCert cert;
    EXPECT_THROW(build({.cert = (cert.dir / "absent.pem").string(), .key = cert.key}), std::runtime_error);
}

TEST(TlsBuilder, MissingKeyFileThrows) {
    const TestCert cert;
    EXPECT_THROW(build({.cert = cert.cert, .key = (cert.dir / "absent.pem").string()}), std::runtime_error);
}

TEST(TlsBuilder, AKeyThatIsNotTheCertificatesThrows) {
    const TestCert cert;
    EXPECT_THROW(build({.cert = cert.cert, .key = cert.unrelated_key}), std::runtime_error);
}

TEST(TlsBuilder, APassphraseProtectedKeyThrowsRatherThanPrompting) {
    const TestCert cert;
    EXPECT_THROW(build({.cert = cert.cert, .key = cert.locked_key}), std::runtime_error);
}

TEST(TlsBuilder, AnEmptyPathThrows) {
    const TestCert cert;
    EXPECT_THROW(build({.cert = cert.cert, .key = ""}), std::invalid_argument);
    EXPECT_THROW(build({.cert = "", .key = cert.key}), std::invalid_argument);
}

// The cases above reach TLS through the fixture's own workers. This one goes
// through Server itself, so that the context it loads is the one its workers
// accept with. start() never returns and there is no stop yet: the server is
// left running on a detached thread until the process ends, and is never
// destroyed under it.
TEST(TlsServer, AStartedServerAnswersOverTls) {
    static const TestCert cert;
    static auto* const server = new owl::Server<AppState>(
        owl::Server<AppState>::builder()
            .router(owl::Router<AppState>::make().route<"/version">(owl::get(version)))
            .config({.port = 0, .tls = cert.tls()})
            .build_with(std::make_shared<AppState>()));
    std::thread([] {
        server->start();
    }).detach();

    const TlsClient client{server->port(), {.ca = cert.cert, .alpn = kH2ThenHttp11}};
    ASSERT_TRUE(client.connected);
    EXPECT_EQ(client.alpn(), "http/1.1");
    client.send(kGetVersion);
    const std::string response = client.read_until_close();
    EXPECT_TRUE(response.starts_with("HTTP/1.1 200")) << response;
    EXPECT_TRUE(response.ends_with("0x101")) << response;
}
