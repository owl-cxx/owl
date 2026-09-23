#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <format>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <h2o.h>

#include <coro/task.h>

#include <owl/extract/extractors.h>
#include <owl/http/response.h>
#include <owl/routing/router.h>
#include <owl/ws/socket.h>

#include "support/live_worker.h"

namespace {
    using owl_test::LiveWorker;

    struct AppState final {
    };

    owl::Response version(owl::RequestView req) {
        return owl::Response::ok(std::format("{:#x}", req->raw()->version));
    }

    coro::task<void> echo(owl::ws::Socket sock) {
        while (auto msg = co_await sock.recv()) co_await sock.send(std::string{msg->data()}, msg->opcode());
    }

    owl::Response crumbs(owl::CookieView<"a"> a, owl::CookieView<"b"> b) {
        return owl::Response::ok(std::format("{}|{}", a.value, b.value));
    }

    // HPACK integers (RFC 7541 §5.1) and strings (§5.2, never Huffman):
    // enough to send a request. Responses go through h2o's own decoder.
    void put_int(std::string& out, const unsigned prefix_bits, std::uint32_t value, const std::uint8_t flags) {
        const std::uint32_t mask = (1u << prefix_bits) - 1;
        if (value < mask) {
            out.push_back(static_cast<char>(flags | value));
            return;
        }
        out.push_back(static_cast<char>(flags | mask));
        value -= mask;
        while (value >= 128) {
            out.push_back(static_cast<char>((value % 128) | 128));
            value /= 128;
        }
        out.push_back(static_cast<char>(value));
    }

    void put_str(std::string& out, const std::string_view s) {
        put_int(out, 7, static_cast<std::uint32_t>(s.size()), 0);
        out.append(s);
    }

    void put_indexed(std::string& out, const std::uint32_t index) {
        put_int(out, 7, index, 0x80);
    }

    void put_literal(std::string& out, const std::uint32_t name_index, const std::string_view value) {
        put_int(out, 4, name_index, 0x00);
        put_str(out, value);
    }

    void put_literal(std::string& out, const std::string_view name, const std::string_view value) {
        out.push_back('\0');
        put_str(out, name);
        put_str(out, value);
    }

    // RFC 7541 Appendix A.
    constexpr std::uint32_t idx_authority = 1;
    constexpr std::uint32_t idx_method_get = 2;
    constexpr std::uint32_t idx_path = 4;
    constexpr std::uint32_t idx_scheme_http = 6;
    constexpr std::uint32_t idx_cookie = 32;

    [[nodiscard]] std::string frame(const std::uint8_t type, const std::uint8_t flags, const std::uint32_t stream, const std::string_view payload) {
        std::string out;
        const auto len = static_cast<std::uint32_t>(payload.size());
        out.push_back(static_cast<char>(len >> 16));
        out.push_back(static_cast<char>(len >> 8));
        out.push_back(static_cast<char>(len));
        out.push_back(static_cast<char>(type));
        out.push_back(static_cast<char>(flags));
        out.push_back(static_cast<char>(stream >> 24));
        out.push_back(static_cast<char>(stream >> 16));
        out.push_back(static_cast<char>(stream >> 8));
        out.push_back(static_cast<char>(stream));
        out.append(payload);
        return out;
    }

    struct Frame final {
        std::uint8_t type = 0;
        std::uint8_t flags = 0;
        std::uint32_t stream = 0;
        std::string payload;
    };

    struct Answer final {
        int status = 0;
        std::vector<std::pair<std::string, std::string>> headers;
        std::string body;
        bool done = false;
    };

    // h2o's decoder, so a Huffman-coded or dynamically indexed response
    // header is no concern of the test. One table per connection: the
    // server's dynamic table grows across responses.
    struct Hpack final {
        h2o_mem_pool_t pool{};
        h2o_hpack_header_table_t table{};

        Hpack() {
            h2o_mem_init_pool(&pool);
            table.hpack_capacity = H2O_HTTP2_SETTINGS_DEFAULT.header_table_size;
            table.hpack_max_capacity = H2O_HTTP2_SETTINGS_DEFAULT.header_table_size;
        }

        ~Hpack() {
            h2o_hpack_dispose_header_table(&table);
            h2o_mem_clear_pool(&pool);
        }

        Hpack(const Hpack&) = delete;
        Hpack& operator=(const Hpack&) = delete;

        void decode(const std::string_view block, Answer& into) {
            h2o_headers_t headers{};
            const char* err = nullptr;
            const int r = h2o_hpack_parse_response(&pool, h2o_hpack_decode_header, &table, &into.status, &headers, nullptr,
                                                   reinterpret_cast<const std::uint8_t*>(block.data()), block.size(), &err);
            if (r != 0) throw std::runtime_error(std::string{"hpack: "} + (err != nullptr ? err : "error"));
            for (std::size_t i = 0; i < headers.size; ++i) {
                const auto& h = headers.entries[i];
                into.headers.emplace_back(std::string{h.name->base, h.name->len}, std::string{h.value.base, h.value.len});
            }
        }
    };

    struct Client final {
        int fd = -1;
        std::string buf;
        Hpack hpack;
        std::map<std::uint32_t, Answer> answers;

        explicit Client(const std::uint16_t port) {
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
                fd = -1;
                throw std::runtime_error("client: connect");
            }
        }

        ~Client() {
            if (fd >= 0) ::close(fd);
        }

        Client(const Client&) = delete;
        Client& operator=(const Client&) = delete;

        void send_all(const std::string_view data) const {
            std::size_t off = 0;
            while (off < data.size()) {
                const auto n = ::send(fd, data.data() + off, data.size() - off, 0);
                if (n <= 0) return;
                off += static_cast<std::size_t>(n);
            }
        }

        [[nodiscard]] bool fill(const std::size_t n) {
            while (buf.size() < n) {
                char tmp[4096];
                const auto r = ::recv(fd, tmp, sizeof(tmp), 0);
                if (r <= 0) return false;
                buf.append(tmp, static_cast<std::size_t>(r));
            }
            return true;
        }

        void preface() {
            send_all("PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n");
            send_all(frame(H2O_HTTP2_FRAME_TYPE_SETTINGS, 0, 0, ""));
        }

        void get(const std::uint32_t stream, const std::string_view path, const std::string_view extra = {}) {
            std::string block;
            put_indexed(block, idx_method_get);
            put_indexed(block, idx_scheme_http);
            put_literal(block, idx_path, path);
            put_literal(block, idx_authority, "127.0.0.1");
            block.append(extra);
            send_all(frame(H2O_HTTP2_FRAME_TYPE_HEADERS, H2O_HTTP2_FRAME_FLAG_END_HEADERS | H2O_HTTP2_FRAME_FLAG_END_STREAM, stream, block));
        }

        // What a browser sends to open a WebSocket over HTTP/2 (RFC 8441).
        void connect_websocket(const std::uint32_t stream, const std::string_view path) {
            std::string block;
            put_literal(block, idx_method_get, "CONNECT");
            put_literal(block, ":protocol", "websocket");
            put_indexed(block, idx_scheme_http);
            put_literal(block, idx_path, path);
            put_literal(block, idx_authority, "127.0.0.1");
            put_literal(block, "sec-websocket-version", "13");
            put_literal(block, "sec-websocket-key", "dGhlIHNhbXBsZSBub25jZQ==");
            send_all(frame(H2O_HTTP2_FRAME_TYPE_HEADERS, H2O_HTTP2_FRAME_FLAG_END_HEADERS, stream, block));
        }

        [[nodiscard]] bool read_frame(Frame& out) {
            if (!fill(9)) return false;
            const auto byte = [this](const std::size_t i) { return static_cast<std::uint32_t>(static_cast<unsigned char>(buf[i])); };
            const auto len = (byte(0) << 16) | (byte(1) << 8) | byte(2);
            if (!fill(9 + len)) return false;
            out.type = static_cast<std::uint8_t>(byte(3));
            out.flags = static_cast<std::uint8_t>(byte(4));
            out.stream = ((byte(5) << 24) | (byte(6) << 16) | (byte(7) << 8) | byte(8)) & 0x7fffffffu;
            out.payload = buf.substr(9, len);
            buf.erase(0, 9 + len);
            return true;
        }

        Answer answer(const std::uint32_t stream) {
            while (!answers[stream].done) {
                Frame f;
                if (!read_frame(f)) throw std::runtime_error("connection closed before the stream ended");
                switch (f.type) {
                case H2O_HTTP2_FRAME_TYPE_SETTINGS:
                    if ((f.flags & H2O_HTTP2_FRAME_FLAG_ACK) == 0) send_all(frame(H2O_HTTP2_FRAME_TYPE_SETTINGS, H2O_HTTP2_FRAME_FLAG_ACK, 0, ""));
                    break;
                case H2O_HTTP2_FRAME_TYPE_HEADERS:
                    if ((f.flags & H2O_HTTP2_FRAME_FLAG_END_HEADERS) == 0) throw std::runtime_error("CONTINUATION is not handled");
                    hpack.decode(f.payload, answers[f.stream]);
                    if ((f.flags & H2O_HTTP2_FRAME_FLAG_END_STREAM) != 0) answers[f.stream].done = true;
                    break;
                case H2O_HTTP2_FRAME_TYPE_DATA:
                    answers[f.stream].body += f.payload;
                    if ((f.flags & H2O_HTTP2_FRAME_FLAG_END_STREAM) != 0) answers[f.stream].done = true;
                    break;
                case H2O_HTTP2_FRAME_TYPE_RST_STREAM:
                    answers[f.stream].done = true;
                    break;
                case H2O_HTTP2_FRAME_TYPE_GOAWAY:
                    throw std::runtime_error("GOAWAY: " + f.payload.substr(8));
                default:
                    break;
                }
            }
            return answers[stream];
        }

        // The HTTP/1.1 half of an h2c upgrade; returns the status code. The
        // HTTP2-Settings value is the one curl sends: max streams 100,
        // window 10 MiB, push off.
        int upgrade(const std::string_view path) {
            send_all(std::format(
                "GET {} HTTP/1.1\r\n"
                "Host: 127.0.0.1\r\n"
                "Connection: Upgrade, HTTP2-Settings\r\n"
                "Upgrade: h2c\r\n"
                "HTTP2-Settings: AAMAAABkAAQAoAAAAAIAAAAA\r\n"
                "\r\n",
                path));
            while (buf.find("\r\n\r\n") == std::string::npos) {
                if (!fill(buf.size() + 1)) return 0;
            }
            const auto end = buf.find("\r\n\r\n");
            const auto head = buf.substr(0, end);
            buf.erase(0, end + 4);
            const auto sp = head.find(' ');
            return sp == std::string::npos ? 0 : std::stoi(head.substr(sp + 1, 3));
        }
    };
}

TEST(Http2, PriorKnowledgeGetIsServedAsHttp2) {
    auto router = owl::Router<AppState>::make().route<"/version">(owl::get(version));
    LiveWorker worker{router};
    Client client{worker.port};
    client.preface();
    client.get(1, "/version");
    const auto answer = client.answer(1);
    EXPECT_EQ(answer.status, 200);
    EXPECT_EQ(answer.body, "0x200");
}

TEST(Http2, UpgradeFromHttp1IsAnsweredWith101ThenServedAsHttp2) {
    auto router = owl::Router<AppState>::make().route<"/version">(owl::get(version));
    LiveWorker worker{router};
    Client client{worker.port};
    ASSERT_EQ(client.upgrade("/version"), 101);
    client.preface();
    const auto answer = client.answer(1);
    EXPECT_EQ(answer.status, 200);
    EXPECT_EQ(answer.body, "0x200");
}

TEST(Http2, StreamsMultiplexOnOneConnection) {
    auto router = owl::Router<AppState>::make().route<"/version">(owl::get(version));
    LiveWorker worker{router};
    Client client{worker.port};
    client.preface();
    client.get(1, "/version");
    client.get(3, "/missing");
    const auto first = client.answer(1);
    const auto second = client.answer(3);
    EXPECT_EQ(first.status, 200);
    EXPECT_EQ(first.body, "0x200");
    EXPECT_EQ(second.status, 404);
}

// h2o advertises SETTINGS_ENABLE_CONNECT_PROTOCOL, so a browser on an
// HTTP/2 connection opens a WebSocket this way and does not fall back to
// an HTTP/1.1 upgrade. owl has no WebSocket over HTTP/2 yet: the request
// reaches the router as CONNECT and misses. Pinned so the day it changes
// is a deliberate one.
TEST(Http2, ExtendedConnectToAWebSocketRouteIs404) {
    auto router = owl::Router<AppState>::make().ws<"/ws">(echo);
    LiveWorker worker{router};
    Client client{worker.port};
    client.preface();
    client.connect_websocket(1, "/ws");
    EXPECT_EQ(client.answer(1).status, 404);
}

// RFC 9113 §8.2.3: an HTTP/2 client may send the cookie string as several
// `cookie` fields, and Chromium does. h2o passes them through unmerged.
TEST(Http2, SplitCookieFieldsAreOneCookieString) {
    auto router = owl::Router<AppState>::make().route<"/crumbs">(owl::get(crumbs));
    LiveWorker worker{router};
    Client client{worker.port};
    client.preface();
    std::string extra;
    put_literal(extra, idx_cookie, "a=1");
    put_literal(extra, idx_cookie, "b=2");
    client.get(1, "/crumbs", extra);
    const auto answer = client.answer(1);
    EXPECT_EQ(answer.status, 200);
    EXPECT_EQ(answer.body, "1|2");
}
