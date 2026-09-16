#include <chrono>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>

#include <gtest/gtest.h>

#include <h2o.h>

#include <coro/run/sync_wait.h>
#include <coro/task.h>

#include <owl/core/method.h>
#include <owl/core/state.h>
#include <owl/http/request.h>
#include <owl/http/response.h>
#include <owl/routing/cors.h>
#include <owl/routing/middleware.h>
#include <owl/routing/router.h>
#include <owl/util/util.h>

namespace {
    struct AppState final {};

    int handler_calls = 0;

    owl::Response counted() {
        ++handler_calls;
        return owl::Response::ok("ok");
    }

    owl::Response varied() {
        return owl::Response::ok("ok").header("vary", "Accept-Encoding");
    }

    coro::task<owl::Response> kick(const owl::Request&, owl::Next<AppState>) {
        co_return owl::Response::ok("unauthorized", 401);
    }

    struct Capture final {
        h2o_ostream_t super{};
        std::string body;

        static void on_send(h2o_ostream_t* const self, h2o_req_t*, h2o_sendvec_t* const bufs, const std::size_t bufcnt, const h2o_send_state_t) {
            auto* const capture = reinterpret_cast<Capture*>(self);
            for (std::size_t i = 0; i < bufcnt; ++i) capture->body.append(bufs[i].raw, bufs[i].len);
        }
    };

    struct Fixture final {
        h2o_req_t req{};
        Capture capture{};
        coro::task<> sending;

        Fixture() {
            handler_calls = 0;
            h2o_mem_init_pool(&req.pool);
            req.query_at = SIZE_MAX;
            req.version = 0x101;
            req.res.content_length = SIZE_MAX;
            capture.super.do_send = &Capture::on_send;
            req._ostr_top = &capture.super;
        }

        ~Fixture() { h2o_mem_clear_pool(&req.pool); }

        void add_header(const char* const name, const char* const value) {
            h2o_add_header_by_str(&req.pool, &req.headers, name, std::char_traits<char>::length(name), 1, nullptr, value, std::char_traits<char>::length(value));
        }

        // Headers are added before the Request is made, so the chain sees
        // them the way dispatch would; the method too, since the layer
        // reads it off the Request rather than being told.
        void run(const owl::Router<AppState>& router, const owl::Method method, const std::string_view path) {
            const auto name = owl::to_string(method);
            req.method = {.base = const_cast<char*>(name.data()), .len = name.size()};
            auto* const request = owl::Request::make(&req);
            owl::detail::MatchedChains<AppState> chains{};
            const auto* const handler = router.match(method, path, *request, &chains);
            ASSERT_NE(handler, nullptr);
            const owl::Context<AppState> ctx{};
            const owl::Next<AppState> next{chains.splice(nullptr), handler, &ctx};
            sending = coro::sync_wait(next(*request)).send(&req);
            sending.start();
        }

        [[nodiscard]] bool has(const std::string_view name) const {
            for (std::size_t i = 0; i < req.res.headers.size; ++i) {
                const auto& entry = req.res.headers.entries[i];
                if (owl::util::eq_ci({entry.name->base, entry.name->len}, name)) return true;
            }
            return false;
        }

        [[nodiscard]] std::string header(const std::string_view name) const {
            for (std::size_t i = 0; i < req.res.headers.size; ++i) {
                const auto& entry = req.res.headers.entries[i];
                if (owl::util::eq_ci({entry.name->base, entry.name->len}, name)) return {entry.value.base, entry.value.len};
            }
            return {};
        }
    };
}

TEST(Cors, LeavesARequestWithoutOriginAlone) {
    auto router = owl::Router<AppState>::make()
                      .layer(owl::cors({}))
                      .route<"/ping">(owl::get(counted));
    Fixture fixture;
    fixture.run(router, owl::Method::Get, "/ping");
    EXPECT_EQ(fixture.req.res.status, 200);
    EXPECT_EQ(handler_calls, 1);
    EXPECT_FALSE(fixture.has("access-control-allow-origin"));
    EXPECT_FALSE(fixture.has("vary"));
}

TEST(Cors, AnyOriginIsAStarWithoutVary) {
    auto router = owl::Router<AppState>::make()
                      .layer(owl::cors({}))
                      .route<"/ping">(owl::get(counted));
    Fixture fixture;
    fixture.add_header("origin", "https://app.example.com");
    fixture.run(router, owl::Method::Get, "/ping");
    EXPECT_EQ(fixture.header("access-control-allow-origin"), "*");
    EXPECT_FALSE(fixture.has("vary"));
    EXPECT_FALSE(fixture.has("access-control-allow-credentials"));
}

TEST(Cors, EchoesAListedOriginAndVaries) {
    auto router = owl::Router<AppState>::make()
                      .layer(owl::cors({.origins = {"https://a.example.com", "https://b.example.com"}}))
                      .route<"/ping">(owl::get(counted));
    Fixture fixture;
    fixture.add_header("origin", "https://b.example.com");
    fixture.run(router, owl::Method::Get, "/ping");
    EXPECT_EQ(fixture.header("access-control-allow-origin"), "https://b.example.com");
    EXPECT_EQ(fixture.header("vary"), "Origin");
}

TEST(Cors, LeavesAnUnlistedOriginAloneButStillServes) {
    auto router = owl::Router<AppState>::make()
                      .layer(owl::cors({.origins = {"https://a.example.com"}}))
                      .route<"/ping">(owl::get(counted));
    Fixture fixture;
    fixture.add_header("origin", "https://evil.example.com");
    fixture.run(router, owl::Method::Get, "/ping");
    EXPECT_EQ(fixture.req.res.status, 200);
    EXPECT_EQ(handler_calls, 1);
    EXPECT_FALSE(fixture.has("access-control-allow-origin"));
}

TEST(Cors, OriginsCompareExactly) {
    auto router = owl::Router<AppState>::make()
                      .layer(owl::cors({.origins = {"https://a.example.com"}}))
                      .route<"/ping">(owl::get(counted));
    Fixture fixture;
    fixture.add_header("origin", "https://a.example.com.evil.net");
    fixture.run(router, owl::Method::Get, "/ping");
    EXPECT_FALSE(fixture.has("access-control-allow-origin"));
}

TEST(Cors, CredentialsAndExposedHeadersRideOnTheResponse) {
    auto router = owl::Router<AppState>::make()
                      .layer(owl::cors({
                          .origins = {"https://a.example.com"},
                          .expose = {"x-trace", "x-request-id"},
                          .credentials = true,
                      }))
                      .route<"/ping">(owl::get(counted));
    Fixture fixture;
    fixture.add_header("origin", "https://a.example.com");
    fixture.run(router, owl::Method::Get, "/ping");
    EXPECT_EQ(fixture.header("access-control-allow-credentials"), "true");
    EXPECT_EQ(fixture.header("access-control-expose-headers"), "x-trace, x-request-id");
}

TEST(Cors, MergesVaryWithTheHandlers) {
    auto router = owl::Router<AppState>::make()
                      .layer(owl::cors({.origins = {"https://a.example.com"}}))
                      .route<"/ping">(owl::get(varied));
    Fixture fixture;
    fixture.add_header("origin", "https://a.example.com");
    fixture.run(router, owl::Method::Get, "/ping");
    EXPECT_EQ(fixture.header("vary"), "Accept-Encoding, Origin");
}

TEST(Cors, PreflightShortCircuitsBeforeTheHandler) {
    auto router = owl::Router<AppState>::make()
                      .layer(owl::cors({}))
                      .route<"/ping">(owl::get(counted));
    Fixture fixture;
    fixture.add_header("origin", "https://app.example.com");
    fixture.add_header("access-control-request-method", "POST");
    fixture.run(router, owl::Method::Options, "/ping");
    EXPECT_EQ(fixture.req.res.status, 204);
    EXPECT_EQ(handler_calls, 0);
    EXPECT_TRUE(fixture.capture.body.empty());
    EXPECT_EQ(fixture.header("access-control-allow-origin"), "*");
    EXPECT_EQ(fixture.header("access-control-allow-methods"), "POST");
    EXPECT_FALSE(fixture.has("access-control-allow-headers"));
    EXPECT_FALSE(fixture.has("access-control-max-age"));
}

TEST(Cors, PreflightMirrorsRequestedHeaders) {
    auto router = owl::Router<AppState>::make()
                      .layer(owl::cors({}))
                      .route<"/ping">(owl::get(counted));
    Fixture fixture;
    fixture.add_header("origin", "https://app.example.com");
    fixture.add_header("access-control-request-method", "PUT");
    fixture.add_header("access-control-request-headers", "content-type, x-trace");
    fixture.run(router, owl::Method::Options, "/ping");
    EXPECT_EQ(fixture.header("access-control-allow-headers"), "content-type, x-trace");
    EXPECT_EQ(fixture.header("vary"), "Access-Control-Request-Method, Access-Control-Request-Headers");
}

TEST(Cors, PreflightUsesConfiguredMethodsAndHeaders) {
    auto router = owl::Router<AppState>::make()
                      .layer(owl::cors({
                          .methods = {owl::Method::Get, owl::Method::Post},
                          .headers = {"authorization", "content-type"},
                      }))
                      .route<"/ping">(owl::get(counted));
    Fixture fixture;
    fixture.add_header("origin", "https://app.example.com");
    fixture.add_header("access-control-request-method", "DELETE");
    fixture.add_header("access-control-request-headers", "x-anything");
    fixture.run(router, owl::Method::Options, "/ping");
    EXPECT_EQ(fixture.header("access-control-allow-methods"), "GET, POST");
    EXPECT_EQ(fixture.header("access-control-allow-headers"), "authorization, content-type");
    EXPECT_FALSE(fixture.has("vary"));
}

TEST(Cors, PreflightCarriesMaxAgeAndCredentialsForAListedOrigin) {
    auto router = owl::Router<AppState>::make()
                      .layer(owl::cors({
                          .origins = {"https://a.example.com"},
                          .credentials = true,
                          .max_age = std::chrono::seconds{600},
                      }))
                      .route<"/ping">(owl::get(counted));
    Fixture fixture;
    fixture.add_header("origin", "https://a.example.com");
    fixture.add_header("access-control-request-method", "GET");
    fixture.run(router, owl::Method::Options, "/ping");
    EXPECT_EQ(fixture.header("access-control-allow-origin"), "https://a.example.com");
    EXPECT_EQ(fixture.header("access-control-allow-credentials"), "true");
    EXPECT_EQ(fixture.header("access-control-max-age"), "600");
    EXPECT_EQ(fixture.header("vary"), "Origin, Access-Control-Request-Method, Access-Control-Request-Headers");
}

TEST(Cors, PreflightFromAnUnlistedOriginGetsNoGrant) {
    auto router = owl::Router<AppState>::make()
                      .layer(owl::cors({.origins = {"https://a.example.com"}}))
                      .route<"/ping">(owl::get(counted));
    Fixture fixture;
    fixture.add_header("origin", "https://evil.example.com");
    fixture.add_header("access-control-request-method", "GET");
    fixture.run(router, owl::Method::Options, "/ping");
    EXPECT_EQ(fixture.req.res.status, 204);
    EXPECT_EQ(handler_calls, 0);
    EXPECT_FALSE(fixture.has("access-control-allow-origin"));
    EXPECT_FALSE(fixture.has("access-control-allow-methods"));
}

TEST(Cors, PreflightNeverReachesAnInnerLayer) {
    auto router = owl::Router<AppState>::make()
                      .layer(owl::cors({}))
                      .layer(kick)
                      .route<"/ping">(owl::get(counted));
    Fixture fixture;
    fixture.add_header("origin", "https://app.example.com");
    fixture.add_header("access-control-request-method", "GET");
    fixture.run(router, owl::Method::Options, "/ping");
    EXPECT_EQ(fixture.req.res.status, 204);
    EXPECT_EQ(fixture.header("access-control-allow-origin"), "*");
}

TEST(Cors, PlainOptionsIsNotAPreflight) {
    auto router = owl::Router<AppState>::make()
                      .layer(owl::cors({}))
                      .route<"/ping">(owl::get(counted));
    Fixture fixture;
    fixture.add_header("origin", "https://app.example.com");
    fixture.run(router, owl::Method::Options, "/ping");
    EXPECT_EQ(fixture.req.res.status, 204);
    EXPECT_EQ(fixture.header("allow"), "GET, OPTIONS");
    EXPECT_EQ(fixture.header("access-control-allow-origin"), "*");
    EXPECT_FALSE(fixture.has("access-control-allow-methods"));
}

TEST(Cors, RejectsCredentialsWithAnyOrigin) {
    EXPECT_THROW((void)owl::cors({.credentials = true}), std::invalid_argument);
    EXPECT_THROW((void)owl::cors({.origins = {"https://a.example.com", "*"}, .credentials = true}), std::invalid_argument);
}
