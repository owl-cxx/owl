// Dispatch-level RED recording, compiled only when OWL_ENABLE_PROMETHEUS is
// ON: a launched Job must record matched, kicked, and thrown exchanges, with
// the route pattern (not the request path) as the label.

#include <gtest/gtest.h>
#include <stdexcept>
#include <string>

#include <prometheus/prometheus.h>

#include <owl/core/state.h>
#include <owl/detail.h>
#include <owl/routing/router.h>

namespace {
    struct Capture final {
        h2o_ostream_t super{};
        std::string body{};

        static void on_send(h2o_ostream_t* const self, h2o_req_t*, h2o_sendvec_t* const bufs, const std::size_t bufcnt, const h2o_send_state_t) {
            auto* const capture = reinterpret_cast<Capture*>(self);
            for (std::size_t i = 0; i < bufcnt; ++i) {
                capture->body.append(bufs[i].raw, bufs[i].len);
            }
        }
    };

    struct Fixture final {
        h2o_globalconf_t globalconf{};
        h2o_context_t ctx{};
        h2o_conn_t conn{};
        h2o_req_t req{};
        Capture capture{};
        owl::Request* request = nullptr;

        Fixture() {
            // send_error_floor walks the real h2o error path, which wants a
            // pathconf (reason phrase) and a conn->ctx (log/send state).
            h2o_config_init(&globalconf);
            auto* const hostconf = h2o_config_register_host(&globalconf, h2o_iovec_init(H2O_STRLIT("default")), 65535);
            auto* const pathconf = h2o_config_register_path(hostconf, "/", 0);
            h2o_context_init(&ctx, h2o_evloop_create(), &globalconf);
            conn.ctx = &ctx;
            conn.hosts = globalconf.hosts;

            h2o_mem_init_pool(&req.pool);
            req.conn = &conn;
            req.pathconf = pathconf;
            req.method = {.base = const_cast<char*>("GET"), .len = 3};
            req.query_at = SIZE_MAX;
            req.version = 0x101;
            req.res.content_length = SIZE_MAX;
            capture.super.do_send = &Capture::on_send;
            req._ostr_top = &capture.super;
            request = owl::Request::make(&req);
        }

        ~Fixture() {
            h2o_mem_clear_pool(&req.pool);
            h2o_context_dispose(&ctx);
            h2o_config_dispose(&globalconf);
        }
    };

    struct App final {
        int n = 0;
    };

    owl::Response ping() {
        return owl::Response::ok("pong");
    }

    coro::task<owl::Response> kick(const owl::Request&, owl::Next<App>) {
        co_return owl::Response::ok("unauthorized", 401);
    }

    owl::Response boom() {
        throw std::runtime_error("boom");
    }

    // A dispatcher wired the way Server wires one, so on_req itself decides
    // what a miss is labelled; the Fixture above stops short of on_req.
    struct Wired final {
        h2o_globalconf_t globalconf{};
        h2o_context_t ctx{};
        h2o_conn_t conn{};
        h2o_req_t req{};
        Capture capture{};
        owl::Router<App> router;
        owl::MiddlewareChain<App> layers;
        owl::detail::Dispatcher<App>* dispatcher = nullptr;

        explicit Wired(owl::Router<App> routes) : router(std::move(routes)) {
            h2o_config_init(&globalconf);
            auto* const hostconf = h2o_config_register_host(&globalconf, h2o_iovec_init(H2O_STRLIT("default")), 65535);
            auto* const pathconf = h2o_config_register_path(hostconf, "/", 0);
            dispatcher = owl::detail::make_dispatcher<App>(pathconf, &router, &layers, std::make_shared<App>(), owl::Config{});
            h2o_context_init(&ctx, h2o_evloop_create(), &globalconf);
            conn.ctx = &ctx;
            conn.hosts = globalconf.hosts;

            h2o_mem_init_pool(&req.pool);
            req.conn = &conn;
            req.pathconf = pathconf;
            req.query_at = SIZE_MAX;
            req.version = 0x101;
            req.res.content_length = SIZE_MAX;
            capture.super.do_send = &Capture::on_send;
            req._ostr_top = &capture.super;
        }

        ~Wired() {
            h2o_mem_clear_pool(&req.pool);
            h2o_context_dispose(&ctx);
            h2o_config_dispose(&globalconf);
        }

        int dispatch(const char* const method, const char* const path) {
            req.method = h2o_iovec_init(method, std::char_traits<char>::length(method));
            req.path = h2o_iovec_init(path, std::char_traits<char>::length(path));
            req.path_normalized = req.path;
            dispatcher->super.on_req(&dispatcher->super, &req);
            return req.res.status;
        }
    };
}

struct DispatchClear : testing::Test {
    DispatchClear() {
        owl::prometheus::clear();
    }
};

TEST_F(DispatchClear, RecordsMatchedRequest) {
    auto router = owl::Router<App>::make().route<"/ping">(owl::get(ping));
    Fixture fixture;
    auto* const job = owl::detail::Job<App>::make(&fixture.req.pool);
    const auto* const handler = router.match(owl::Method::Get, "/ping", *fixture.request, &job->chains);
    ASSERT_NE(handler, nullptr);
    const owl::Context<App> ctx{std::make_shared<App>(), fixture.ctx.loop};
    job->launch(handler, fixture.request, nullptr, &ctx);
    EXPECT_EQ(fixture.req.res.status, 200);

    const std::string body = owl::prometheus::dump();
    EXPECT_NE(body.find("http_requests_total{method=\"GET\",status=\"200\",route=\"/ping\"} 1"), std::string::npos);
    EXPECT_NE(body.find("http_request_duration_seconds_count{method=\"GET\",status=\"200\",route=\"/ping\"} 1"), std::string::npos);
}

TEST_F(DispatchClear, RecordsKickedRequest) {
    auto router = owl::Router<App>::make()
                      .layer(kick)
                      .route<"/ping">(owl::get(ping));
    Fixture fixture;
    auto* const job = owl::detail::Job<App>::make(&fixture.req.pool);
    const auto* const handler = router.match(owl::Method::Get, "/ping", *fixture.request, &job->chains);
    ASSERT_NE(handler, nullptr);
    const owl::Context<App> ctx{std::make_shared<App>(), fixture.ctx.loop};
    job->launch(handler, fixture.request, nullptr, &ctx);
    EXPECT_EQ(fixture.req.res.status, 401);

    const std::string body = owl::prometheus::dump();
    EXPECT_NE(body.find("http_requests_total{method=\"GET\",status=\"401\",route=\"/ping\"} 1"), std::string::npos);
}

TEST_F(DispatchClear, RecordsMissesAsUnmatched) {
    Wired missing{owl::Router<App>::make().route<"/ping">(owl::get(ping))};
    EXPECT_EQ(missing.dispatch("GET", "/nope"), 404);
    Wired wrong_method{owl::Router<App>::make().route<"/ping">(owl::get(ping))};
    EXPECT_EQ(wrong_method.dispatch("POST", "/ping"), 405);

    const std::string body = owl::prometheus::dump();
    EXPECT_NE(body.find("http_requests_total{method=\"GET\",status=\"404\",route=\"unmatched\"} 1"), std::string::npos);
    EXPECT_NE(body.find("http_requests_total{method=\"POST\",status=\"405\",route=\"unmatched\"} 1"), std::string::npos);
    EXPECT_EQ(body.find("route=\"\""), std::string::npos);
    EXPECT_EQ(body.find("http_request_duration_seconds"), std::string::npos);
}

TEST_F(DispatchClear, RecordsThrownHandlerAs500) {
    auto router = owl::Router<App>::make().route<"/boom">(owl::get(boom));
    Fixture fixture;
    auto* const job = owl::detail::Job<App>::make(&fixture.req.pool);
    const auto* const handler = router.match(owl::Method::Get, "/boom", *fixture.request, &job->chains);
    ASSERT_NE(handler, nullptr);
    const owl::Context<App> ctx{std::make_shared<App>(), fixture.ctx.loop};
    job->launch(handler, fixture.request, nullptr, &ctx);

    const std::string body = owl::prometheus::dump();
    EXPECT_NE(body.find("http_requests_total{method=\"GET\",status=\"500\",route=\"/boom\"} 1"), std::string::npos);
    EXPECT_NE(body.find("http_request_duration_seconds_count{method=\"GET\",status=\"500\",route=\"/boom\"} 1"), std::string::npos);
}
