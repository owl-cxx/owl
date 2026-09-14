#include <concepts>
#include <cstddef>
#include <gtest/gtest.h>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include <coro/run/sync_wait.h>
#include <coro/task.h>

#include <owl/routing/router.h>
#include <owl/core/state.h>
#include <owl/ws/socket.h>

namespace {
    owl::Response ping() {
        return owl::Response::ok("pong");
    }

    owl::Response custom_options() {
        return owl::Response::ok("custom");
    }

    coro::task<void> chat(owl::ws::Socket) { co_return; }

    struct Capture final {
        h2o_ostream_t super{};
        std::string body;

        static void on_send(h2o_ostream_t* const self, h2o_req_t*, h2o_sendvec_t* const bufs, const std::size_t bufcnt, const h2o_send_state_t) {
            auto* const capture = reinterpret_cast<Capture*>(self);
            for (std::size_t i = 0; i < bufcnt; ++i) capture->body.append(bufs[i].raw, bufs[i].len);
        }
    };

    // A request that can be answered: match, run the handler, send, and
    // read back what was sent.
    struct Answered final {
        h2o_req_t req{};
        Capture capture{};
        owl::Request* request = nullptr;
        coro::task<> sending;

        Answered() {
            h2o_mem_init_pool(&req.pool);
            req.query_at = SIZE_MAX;
            req.version = 0x101;
            req.res.content_length = SIZE_MAX;
            capture.super.do_send = &Capture::on_send;
            req._ostr_top = &capture.super;
        }

        ~Answered() { h2o_mem_clear_pool(&req.pool); }

        template <typename S>
        void run(const owl::Router<S>& router, const owl::Method method, const std::string_view path) {
            const auto name = owl::to_string(method);
            req.method = {.base = const_cast<char*>(name.data()), .len = name.size()};
            request = owl::Request::make(&req);
            const auto* const handler = router.match(method, path, *request);
            ASSERT_NE(handler, nullptr);
            const owl::Context<S> ctx{};
            sending = coro::sync_wait((*handler)(*request, ctx)).send(&req);
            sending.start();
        }

        [[nodiscard]] std::string header(const std::string_view name) const {
            for (std::size_t i = 0; i < req.res.headers.size; ++i) {
                const auto& entry = req.res.headers.entries[i];
                if (owl::util::eq_ci({entry.name->base, entry.name->len}, name)) return {entry.value.base, entry.value.len};
            }
            return {};
        }
    };

    struct App {
        int n = 7;
    };

    template <typename Outer, typename Inner>
    concept can_nest = requires(Outer outer, Inner inner) {
        std::move(outer).template nest<"/api">(std::move(inner));
    };

    static_assert(can_nest<owl::Router<App>, owl::Router<App>>);

    owl::Response read_state(owl::State<App> app) {
        return owl::Response::ok(std::to_string(app->n));
    }

    owl::Response echo_id(owl::PathView<"id"> id) {
        return owl::Response::ok(std::string{id.value});
    }

    coro::task<owl::Response> pass_through(const owl::Request& req, owl::Next<App> next) {
        co_return co_await next(req);
    }
}

TEST(Router, MatchesLiteralGet) {
    auto router = owl::Router<App>::make()
                      .route<"/ping">(owl::get(ping));
    h2o_req_t req{};
    h2o_mem_init_pool(&req.pool);
    req.query_at = SIZE_MAX;
    auto* request = owl::Request::make(&req);
    const auto* handler = router.match(owl::Method::Get, "/ping", *request);
    ASSERT_NE(handler, nullptr);
    h2o_mem_clear_pool(&req.pool);
}

TEST(Router, MatchSetsRoutePattern) {
    auto router = owl::Router<App>::make()
                      .route<"/ping">(owl::get(ping));
    h2o_req_t req{};
    h2o_mem_init_pool(&req.pool);
    req.query_at = SIZE_MAX;
    auto* request = owl::Request::make(&req);
    ASSERT_NE(router.match(owl::Method::Get, "/ping", *request), nullptr);
    EXPECT_EQ(request->route_pattern(), "/ping");
    h2o_mem_clear_pool(&req.pool);
}

TEST(Router, UnknownPathIsNull) {
    auto router = owl::Router<App>::make()
                      .route<"/ping">(owl::get(ping));
    h2o_req_t req{};
    h2o_mem_init_pool(&req.pool);
    req.query_at = SIZE_MAX;
    auto* request = owl::Request::make(&req);
    EXPECT_EQ(router.match(owl::Method::Get, "/nope", *request), nullptr);
    h2o_mem_clear_pool(&req.pool);
}

TEST(Router, CapturesPathParam) {
    auto router = owl::Router<App>::make()
                      .route<"/users/{id}">(owl::get(echo_id));
    h2o_req_t req{};
    h2o_mem_init_pool(&req.pool);
    req.query_at = SIZE_MAX;
    auto* request = owl::Request::make(&req);
    ASSERT_NE(router.match(owl::Method::Get, "/users/42", *request), nullptr);
    EXPECT_EQ(request->param("id"), "42");
    EXPECT_EQ(request->route_pattern(), "/users/{id}");
    h2o_mem_clear_pool(&req.pool);
}

TEST(Router, RoutesStateHandler) {
    auto router = owl::Router<App>::make()
                      .route<"/n">(owl::get(read_state));
    h2o_req_t req{};
    h2o_mem_init_pool(&req.pool);
    req.query_at = SIZE_MAX;
    auto* request = owl::Request::make(&req);
    EXPECT_NE(router.match(owl::Method::Get, "/n", *request), nullptr);
    h2o_mem_clear_pool(&req.pool);
}

TEST(Router, NestPrefix) {
    auto inner = owl::Router<App>::make()
                     .route<"/ping">(owl::get(ping));
    auto router = owl::Router<App>::make()
                      .nest<"/api/v1">(std::move(inner));
    h2o_req_t req{};
    h2o_mem_init_pool(&req.pool);
    req.query_at = SIZE_MAX;
    auto* request = owl::Request::make(&req);
    EXPECT_NE(router.match(owl::Method::Get, "/api/v1/ping", *request), nullptr);
    EXPECT_EQ(router.match(owl::Method::Get, "/ping", *request), nullptr);
    h2o_mem_clear_pool(&req.pool);
}

TEST(Router, NestedMatchSetsFullRoutePattern) {
    auto inner = owl::Router<App>::make()
                     .route<"/ping">(owl::get(ping));
    auto router = owl::Router<App>::make()
                      .nest<"/api/v1">(std::move(inner));
    h2o_req_t req{};
    h2o_mem_init_pool(&req.pool);
    req.query_at = SIZE_MAX;
    auto* request = owl::Request::make(&req);
    ASSERT_NE(router.match(owl::Method::Get, "/api/v1/ping", *request), nullptr);
    EXPECT_EQ(request->route_pattern(), "/api/v1/ping");
    h2o_mem_clear_pool(&req.pool);
}

TEST(Router, NestStateHandler) {
    auto inner = owl::Router<App>::make()
                     .route<"/n">(owl::get(read_state));
    auto router = owl::Router<App>::make()
                      .nest<"/api">(std::move(inner));
    h2o_req_t req{};
    h2o_mem_init_pool(&req.pool);
    req.query_at = SIZE_MAX;
    auto* request = owl::Request::make(&req);
    EXPECT_NE(router.match(owl::Method::Get, "/api/n", *request), nullptr);
    h2o_mem_clear_pool(&req.pool);
}

TEST(Router, OptionsMatchesARouteThatRegisteredNone) {
    auto router = owl::Router<App>::make()
                      .route<"/ping">(owl::get(ping));
    h2o_req_t req{};
    h2o_mem_init_pool(&req.pool);
    req.query_at = SIZE_MAX;
    auto* request = owl::Request::make(&req);
    ASSERT_NE(router.match(owl::Method::Options, "/ping", *request), nullptr);
    EXPECT_EQ(request->route_pattern(), "/ping");
    h2o_mem_clear_pool(&req.pool);
}

TEST(Router, OptionsFallbackAnswersNoContentWithAllow) {
    auto router = owl::Router<App>::make()
                      .route<"/ping">(owl::get(ping).post(ping));
    Answered answered;
    answered.run(router, owl::Method::Options, "/ping");
    EXPECT_EQ(answered.req.res.status, 204);
    EXPECT_EQ(answered.header("allow"), "GET, POST, OPTIONS");
    EXPECT_TRUE(answered.capture.body.empty());
}

TEST(Router, RegisteredOptionsWinsOverTheFallback) {
    auto router = owl::Router<App>::make()
                      .route<"/ping">(owl::get(ping).options(custom_options));
    Answered answered;
    answered.run(router, owl::Method::Options, "/ping");
    EXPECT_EQ(answered.req.res.status, 200);
    EXPECT_EQ(answered.capture.body, "custom");
}

TEST(Router, OptionsOnAWebSocketOnlyPathIsNull) {
    auto router = owl::Router<App>::make().ws<"/chat">(chat);
    h2o_req_t req{};
    h2o_mem_init_pool(&req.pool);
    req.query_at = SIZE_MAX;
    auto* request = owl::Request::make(&req);
    EXPECT_EQ(router.match(owl::Method::Options, "/chat", *request), nullptr);
    h2o_mem_clear_pool(&req.pool);
}

TEST(Router, OptionsOnAnIntermediateNodeIsNull) {
    auto router = owl::Router<App>::make()
                      .route<"/api/ping">(owl::get(ping));
    h2o_req_t req{};
    h2o_mem_init_pool(&req.pool);
    req.query_at = SIZE_MAX;
    auto* request = owl::Request::make(&req);
    EXPECT_EQ(router.match(owl::Method::Options, "/api", *request), nullptr);
    h2o_mem_clear_pool(&req.pool);
}

TEST(Router, AllowedMethodsListOptionsForAnyRoute) {
    auto router = owl::Router<App>::make()
                      .route<"/ping">(owl::get(ping));
    EXPECT_TRUE(router.allowed_methods("/ping").has(owl::Method::Options));
    EXPECT_TRUE(router.allowed_methods("/nope").empty());
}

TEST(Router, LayerCollectsChain) {
    auto router = owl::Router<App>::make()
                      .layer(pass_through)
                      .route<"/ping">(owl::get(ping));
    h2o_req_t req{};
    h2o_mem_init_pool(&req.pool);
    req.query_at = SIZE_MAX;
    auto* request = owl::Request::make(&req);
    owl::detail::MatchedChains<App> chains{};
    ASSERT_NE(router.match(owl::Method::Get, "/ping", *request, &chains), nullptr);
    EXPECT_EQ(chains.count, 1u);
    h2o_mem_clear_pool(&req.pool);
}
