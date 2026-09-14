// What the router refuses: patterns at compile time, registrations at
// startup, and requests that must not match -- with what the 404/405
// decision sees for each.
#include <gtest/gtest.h>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include <h2o.h>

#include <owl/routing/router.h>

// ---- patterns: what the parser refuses, and the boundaries it keeps -----

namespace {
    using owl::detail::parse_pattern;

    static_assert(!parse_pattern("").ok, "empty");
    static_assert(!parse_pattern("users").ok, "no leading slash");
    static_assert(!parse_pattern("//").ok, "double slash");
    static_assert(!parse_pattern("/a//b").ok, "empty segment inside");
    static_assert(!parse_pattern("/a/").ok, "trailing slash");
    static_assert(!parse_pattern("/{id").ok, "unterminated brace");
    static_assert(!parse_pattern("/{}").ok, "empty parameter name");
    static_assert(!parse_pattern("/{a-b}").ok, "invalid character in parameter name");
    static_assert(!parse_pattern("/{a}/{a}").ok, "duplicate parameter name");
    static_assert(!parse_pattern("/a{b}").ok, "brace inside a literal");
    static_assert(!parse_pattern("/a}").ok, "closing brace inside a literal");
    static_assert(!parse_pattern("/{a}b").ok, "text after a parameter");
    static_assert(!parse_pattern("/{a}/{b}/{c}/{d}/{e}/{f}/{g}/{h}/{i}").ok, "nine parameters");
    static_assert(!parse_pattern(
        "/1/2/3/4/5/6/7/8/9/10/11/12/13/14/15/16/17/18/19/20/21/22/23/24/25/26/27/28/29/30/31/32/33").ok,
        "thirty-three segments");

    static_assert(parse_pattern("/").ok, "root");
    static_assert(parse_pattern("/{a}/{b}/{c}/{d}/{e}/{f}/{g}/{h}").ok, "eight parameters");
    static_assert(parse_pattern(
        "/1/2/3/4/5/6/7/8/9/10/11/12/13/14/15/16/17/18/19/20/21/22/23/24/25/26/27/28/29/30/31/32").ok,
        "thirty-two segments");
    static_assert(parse_pattern("/users/{id}/posts/{pid}").ok, "typical");
    static_assert(parse_pattern("/a_b/{c_d}").ok, "underscores");
}

namespace {
    struct App {};

    owl::Response ping() { return owl::Response::ok("pong"); }
    owl::Response echo_id(owl::PathView<"id"> id) { return owl::Response::ok(std::string{id.value}); }
    coro::task<void> chat(owl::ws::Socket) { co_return; }

    struct Req {
        h2o_req_t req{};
        owl::Request* request;

        Req() {
            h2o_mem_init_pool(&req.pool);
            req.query_at = SIZE_MAX;
            request = owl::Request::make(&req);
        }

        ~Req() { h2o_mem_clear_pool(&req.pool); }

        void upgrade() { req.upgrade = h2o_iovec_init(H2O_STRLIT("websocket")); }
    };

    template <typename R>
    std::string allow(const R& router, const std::string_view path) {
        return router.allowed_methods(path).to_allow_header();
    }
}

// ---- registration: what throws --------------------------------------------

TEST(RouterRejects, SamePathSameMethodTwice) {
    EXPECT_THROW((void)owl::Router<App>::make().route<"/x">(owl::get(ping)).route<"/x">(owl::get(ping)),
                 std::invalid_argument);
}

TEST(RouterRejects, SameMethodTwiceInOneMethodRouter) {
    EXPECT_THROW((void)owl::Router<App>::make().route<"/x">(owl::get(ping).get(ping)), std::invalid_argument);
}

TEST(RouterRejects, DifferentMethodsSamePathIsFine) {
    auto router = owl::Router<App>::make().route<"/x">(owl::get(ping).post(ping));
    EXPECT_EQ(router.size(), 2u);
}

TEST(RouterRejects, ParameterNamedDifferentlyAtSameSlot) {
    EXPECT_THROW((void)owl::Router<App>::make()
                     .route<"/u/{id}">(owl::get(echo_id))
                     .route<"/u/{uid}/posts">(owl::get(ping)),
                 std::invalid_argument);
}

TEST(RouterRejects, SameParameterNameAtSameSlotIsFine) {
    auto router = owl::Router<App>::make()
                      .route<"/u/{id}">(owl::get(echo_id))
                      .route<"/u/{id}/posts">(owl::get(ping));
    EXPECT_EQ(router.size(), 2u);
}

TEST(RouterRejects, ParameterClashAcrossNest) {
    EXPECT_THROW((void)owl::Router<App>::make()
                     .route<"/u/{id}">(owl::get(echo_id))
                     .nest<"/u">(owl::Router<App>::make().route<"/{uid}">(owl::get(ping))),
                 std::invalid_argument);
}

TEST(RouterRejects, DuplicateRouteAcrossNest) {
    EXPECT_THROW((void)owl::Router<App>::make()
                     .route<"/api/ping">(owl::get(ping))
                     .nest<"/api">(owl::Router<App>::make().route<"/ping">(owl::get(ping))),
                 std::invalid_argument);
}

TEST(RouterRejects, NestPastTheDepthCap) {
    // 31 + 2 segments; 31 + 1 is the cap and is accepted below.
    auto inner = owl::Router<App>::make().route<"/y/z">(owl::get(ping));
    EXPECT_THROW((void)owl::Router<App>::make()
                     .nest<"/a/b/c/d/e/f/g/h/i/j/k/l/m/n/o/p/q/r/s/t/u/v/w/x/y/aa/bb/cc/dd/ee/ff">(std::move(inner)),
                 std::invalid_argument);
}

TEST(RouterRejects, NestAtTheDepthCapIsFine) {
    auto inner = owl::Router<App>::make().route<"/z">(owl::get(ping));
    auto router = owl::Router<App>::make()
                      .nest<"/a/b/c/d/e/f/g/h/i/j/k/l/m/n/o/p/q/r/s/t/u/v/w/x/y/aa/bb/cc/dd/ee/ff">(std::move(inner));
    Req r;
    EXPECT_NE(router.match(owl::Method::Get, "/a/b/c/d/e/f/g/h/i/j/k/l/m/n/o/p/q/r/s/t/u/v/w/x/y/aa/bb/cc/dd/ee/ff/z", *r.request),
              nullptr);
}

TEST(RouterRejects, ThrowLeavesEarlierRoutesUsable) {
    auto router = owl::Router<App>::make().route<"/x">(owl::get(ping));
    EXPECT_THROW(router = std::move(router).route<"/x">(owl::get(ping)), std::invalid_argument);
}

// ---- requests: what misses, and what the 404/405 decision sees ------------

TEST(RouterMisses, UnknownPathIs404) {
    auto router = owl::Router<App>::make().route<"/ping">(owl::get(ping));
    Req r;
    EXPECT_EQ(router.match(owl::Method::Get, "/nope", *r.request), nullptr);
    EXPECT_TRUE(router.allowed_methods("/nope").empty());
}

TEST(RouterMisses, WrongMethodIs405WithAllow) {
    auto router = owl::Router<App>::make().route<"/ping">(owl::get(ping).post(ping));
    Req r;
    EXPECT_EQ(router.match(owl::Method::Delete, "/ping", *r.request), nullptr);
    const std::string header = allow(router, "/ping");
    EXPECT_NE(header.find("GET"), std::string::npos) << header;
    EXPECT_NE(header.find("POST"), std::string::npos) << header;
}

TEST(RouterMisses, AllowIsTheUnionOfLiteralAndParameter) {
    auto router = owl::Router<App>::make()
                      .route<"/u/new">(owl::get(ping))
                      .route<"/u/{id}">(owl::post(echo_id));
    const std::string header = allow(router, "/u/new");
    EXPECT_NE(header.find("GET"), std::string::npos) << header;
    EXPECT_NE(header.find("POST"), std::string::npos) << header;
}

TEST(RouterMisses, MalformedPathsNeverMatch) {
    auto router = owl::Router<App>::make()
                      .route<"/">(owl::get(ping))
                      .route<"/a">(owl::get(ping))
                      .route<"/a/b">(owl::get(ping));
    for (const std::string_view path : {"", "a", "a/b", "//", "/a//b", "/a/", "/a/b/", "///"}) {
        Req r;
        EXPECT_EQ(router.match(owl::Method::Get, path, *r.request), nullptr) << "matched " << path;
        EXPECT_TRUE(router.allowed_methods(path).empty()) << "allowed " << path;
    }
}

TEST(RouterMisses, ThirtyThreeSegmentsNeverMatch) {
    auto router = owl::Router<App>::make().route<"/a">(owl::get(ping));
    std::string path;
    for (int i = 0; i < 33; ++i) path += "/a";
    Req r;
    EXPECT_EQ(router.match(owl::Method::Get, path, *r.request), nullptr);
    EXPECT_TRUE(router.allowed_methods(path).empty());
}

TEST(RouterMisses, PathTooShortOrTooLongForPattern) {
    auto router = owl::Router<App>::make().route<"/a/{id}/c">(owl::get(ping));
    for (const std::string_view path : {"/a", "/a/1", "/a/1/c/d", "/a/1/x"}) {
        Req r;
        EXPECT_EQ(router.match(owl::Method::Get, path, *r.request), nullptr) << "matched " << path;
    }
}

TEST(RouterMisses, MissLeavesNoCaptureBehind) {
    auto router = owl::Router<App>::make().route<"/a/{x}/b">(owl::get(ping));
    Req r;
    EXPECT_EQ(router.match(owl::Method::Get, "/a/q/z", *r.request), nullptr);
    EXPECT_EQ(r.request->param_count(), 0u);
}

TEST(RouterMisses, LiteralFailsThenParameterMatches) {
    // The literal branch is tried first and fails below "new"; what it
    // recorded is undone before the parameter branch is tried.
    auto router = owl::Router<App>::make()
                      .route<"/a/{x}/b">(owl::get(ping))
                      .route<"/a/new/c">(owl::get(ping));
    Req r;
    ASSERT_NE(router.match(owl::Method::Get, "/a/new/b", *r.request), nullptr);
    EXPECT_EQ(r.request->param("x"), "new");
    EXPECT_EQ(r.request->param_count(), 1u);
    EXPECT_EQ(r.request->route_pattern(), "/a/{x}/b");
}

TEST(RouterMisses, UpgradeOnGetOnlyPathFallsBackToGet) {
    auto router = owl::Router<App>::make().route<"/p">(owl::get(ping));
    Req plain;
    const auto* const get_handler = router.match(owl::Method::Get, "/p", *plain.request);
    ASSERT_NE(get_handler, nullptr);
    Req up;
    up.upgrade();
    EXPECT_EQ(router.match(owl::Method::Get, "/p", *up.request), get_handler);
}

TEST(RouterMisses, UpgradeWithPostNeverTakesTheWsSlot) {
    auto router = owl::Router<App>::make().ws<"/chat">(chat);
    Req up;
    up.upgrade();
    EXPECT_EQ(router.match(owl::Method::Post, "/chat", *up.request), nullptr);
}

TEST(RouterMisses, NestedMissIs404UnderThePrefixToo) {
    auto router = owl::Router<App>::make()
                      .nest<"/api">(owl::Router<App>::make().route<"/ping">(owl::get(ping)));
    for (const std::string_view path : {"/api", "/api/", "/api/nope", "/ping"}) {
        Req r;
        EXPECT_EQ(router.match(owl::Method::Get, path, *r.request), nullptr) << "matched " << path;
        EXPECT_TRUE(router.allowed_methods(path).empty()) << "allowed " << path;
    }
}
