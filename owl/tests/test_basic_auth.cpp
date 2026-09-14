#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include <h2o.h>

#include <coro/run/sync_wait.h>
#include <coro/task.h>

#include <owl/core/method.h>
#include <owl/core/state.h>
#include <owl/http/auth.h>
#include <owl/http/request.h>
#include <owl/http/response.h>
#include <owl/routing/basic_auth.h>
#include <owl/routing/middleware.h>
#include <owl/routing/router.h>
#include <owl/util/util.h>

namespace {
    struct App final {};

    int handler_calls = 0;

    owl::Response counted() {
        ++handler_calls;
        return owl::Response::ok("ok");
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

        void run(const owl::Router<App>& router, const std::string_view path = "/ping") {
            req.method = {.base = const_cast<char*>("GET"), .len = 3};
            auto* const request = owl::Request::make(&req);
            owl::detail::MatchedChains<App> chains{};
            const auto* const handler = router.match(owl::Method::Get, path, *request, &chains);
            ASSERT_NE(handler, nullptr);
            const owl::Context<App> ctx{};
            const owl::Next<App> next{chains.splice(nullptr), handler, &ctx};
            sending = coro::sync_wait(next(*request)).send(&req);
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

    owl::Router<App> guarded(owl::BasicAuth config) {
        return owl::Router<App>::make()
            .layer(owl::basic_auth(std::move(config)))
            .route<"/ping">(owl::get(counted));
    }

    constexpr const char* kVars[] = {"OWL_BASIC_USERS", "OWL_BASIC_REALM"};

    struct EnvIsolated : ::testing::Test {
        std::vector<std::pair<std::string, std::optional<std::string>>> saved;

        void SetUp() override {
            for (const char* const k : kVars) {
                if (const char* const v = std::getenv(k)) saved.emplace_back(k, std::string{v});
                else saved.emplace_back(k, std::nullopt);
                unsetenv(k);
            }
        }

        void TearDown() override {
            for (const auto& [k, v] : saved) {
                if (v) setenv(k.c_str(), v->c_str(), 1);
                else unsetenv(k.c_str());
            }
        }
    };
}

// ---- parse_basic -----------------------------------------------------------

TEST(ParseBasic, SplitsUserAndPassword) {
    const auto parsed = owl::parse_basic("Basic dXNlcjpwYXNz");
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->user, "user");
    EXPECT_EQ(parsed->password, "pass");
}

TEST(ParseBasic, SchemeIsCaseInsensitiveAndSpacesMayRepeat) {
    EXPECT_TRUE(owl::parse_basic("basic dXNlcjpwYXNz").has_value());
    EXPECT_TRUE(owl::parse_basic("BASIC   dXNlcjpwYXNz").has_value());
}

TEST(ParseBasic, PasswordKeepsItsColons) {
    const auto parsed = owl::parse_basic("Basic dXNlcjpwYTpzcw==");
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->user, "user");
    EXPECT_EQ(parsed->password, "pa:ss");
}

TEST(ParseBasic, RejectsOtherSchemesAndMalformedCredentials) {
    EXPECT_FALSE(owl::parse_basic("Bearer dXNlcjpwYXNz").has_value());
    EXPECT_FALSE(owl::parse_basic("Basicx dXNlcjpwYXNz").has_value());
    EXPECT_FALSE(owl::parse_basic("Basic").has_value());
    EXPECT_FALSE(owl::parse_basic("Basic ").has_value());
    EXPECT_FALSE(owl::parse_basic("Basic !!!!").has_value());
    EXPECT_FALSE(owl::parse_basic("Basic dXNlcnBhc3M=").has_value()); // "userpass": no colon
    EXPECT_FALSE(owl::parse_basic("").has_value());
}

// ---- the layer -------------------------------------------------------------

TEST(BasicAuthLayer, ChallengesWhenNoCredentialsAreSent) {
    const auto router = guarded({.users = {{"alice", "s3cret"}}});
    Fixture fixture;
    fixture.run(router);
    EXPECT_EQ(fixture.req.res.status, 401);
    EXPECT_EQ(fixture.capture.body, "unauthorized");
    EXPECT_EQ(fixture.header("www-authenticate"), "Basic realm=\"restricted\", charset=\"UTF-8\"");
    EXPECT_EQ(handler_calls, 0);
}

TEST(BasicAuthLayer, ChallengeCarriesTheConfiguredRealm) {
    const auto router = guarded({.realm = "admin", .users = {{"alice", "s3cret"}}});
    Fixture fixture;
    fixture.run(router);
    EXPECT_EQ(fixture.header("www-authenticate"), "Basic realm=\"admin\", charset=\"UTF-8\"");
}

TEST(BasicAuthLayer, AdmitsAListedUserWithTheRightPassword) {
    const auto router = guarded({.users = {{"alice", "s3cret"}}});
    Fixture fixture;
    fixture.add_header("authorization", "Basic YWxpY2U6czNjcmV0"); // alice:s3cret
    fixture.run(router);
    EXPECT_EQ(fixture.req.res.status, 200);
    EXPECT_EQ(fixture.capture.body, "ok");
    EXPECT_EQ(handler_calls, 1);
    EXPECT_TRUE(fixture.header("www-authenticate").empty());
}

TEST(BasicAuthLayer, RejectsTheWrongPassword) {
    const auto router = guarded({.users = {{"alice", "s3cret"}}});
    Fixture fixture;
    fixture.add_header("authorization", "Basic YWxpY2U6UzNjcmV0"); // alice:S3cret
    fixture.run(router);
    EXPECT_EQ(fixture.req.res.status, 401);
    EXPECT_EQ(handler_calls, 0);
}

TEST(BasicAuthLayer, RejectsAnUnknownUser) {
    const auto router = guarded({.users = {{"alice", "s3cret"}}});
    Fixture fixture;
    fixture.add_header("authorization", "Basic Ym9iOnMzY3JldA=="); // bob:s3cret
    fixture.run(router);
    EXPECT_EQ(fixture.req.res.status, 401);
    EXPECT_EQ(handler_calls, 0);
}

TEST(BasicAuthLayer, MalformedCredentialsAreChallengedNotBadRequest) {
    const auto router = guarded({.users = {{"alice", "s3cret"}}});
    Fixture fixture;
    fixture.add_header("authorization", "Bearer YWxpY2U6czNjcmV0");
    fixture.run(router);
    EXPECT_EQ(fixture.req.res.status, 401);
    EXPECT_FALSE(fixture.header("www-authenticate").empty());
}

TEST(BasicAuthLayer, VerifyCallbackDecides) {
    std::vector<std::pair<std::string, std::string>> seen;
    const auto router = guarded({
        .verify = [&seen](const std::string_view user, const std::string_view password) {
            seen.emplace_back(user, password);
            return password == "open-sesame";
        },
    });
    Fixture yes;
    yes.add_header("authorization", "Basic Ym9iOm9wZW4tc2VzYW1l"); // bob:open-sesame
    yes.run(router);
    EXPECT_EQ(yes.req.res.status, 200);

    Fixture no;
    no.add_header("authorization", "Basic Ym9iOm5vcGU="); // bob:nope
    no.run(router);
    EXPECT_EQ(no.req.res.status, 401);

    ASSERT_EQ(seen.size(), 2u);
    EXPECT_EQ(seen[0], (std::pair<std::string, std::string>{"bob", "open-sesame"}));
    EXPECT_EQ(seen[1], (std::pair<std::string, std::string>{"bob", "nope"}));
}

TEST(BasicAuthLayer, ListedUsersAndVerifyAreBothConsulted) {
    const auto router = guarded({
        .users = {{"alice", "s3cret"}},
        .verify = [](const std::string_view user, const std::string_view) { return user == "root"; },
    });
    Fixture listed;
    listed.add_header("authorization", "Basic YWxpY2U6czNjcmV0"); // alice:s3cret
    listed.run(router);
    EXPECT_EQ(listed.req.res.status, 200);

    Fixture verified;
    verified.add_header("authorization", "Basic cm9vdDp3aGF0ZXZlcg=="); // root:whatever
    verified.run(router);
    EXPECT_EQ(verified.req.res.status, 200);
}

TEST(BasicAuthLayer, RejectsAPolicyThatAdmitsNobody) {
    EXPECT_THROW((void)owl::basic_auth({}), std::invalid_argument);
    EXPECT_THROW((void)owl::basic_auth({.realm = "admin"}), std::invalid_argument);
}

TEST(BasicAuthLayer, RejectsARealmThatCannotBeQuoted) {
    EXPECT_THROW((void)owl::basic_auth({.realm = "say \"hi\"", .users = {{"a", "b"}}}), std::invalid_argument);
    EXPECT_THROW((void)owl::basic_auth({.realm = "line\nbreak", .users = {{"a", "b"}}}), std::invalid_argument);
    EXPECT_THROW((void)owl::basic_auth({.realm = "", .users = {{"a", "b"}}}), std::invalid_argument);
}

// ---- make() from the environment ---------------------------------------------

TEST_F(EnvIsolated, MakeRequiresUsers) {
    EXPECT_THROW((void)owl::BasicAuth::make(), std::invalid_argument);
}

TEST_F(EnvIsolated, MakeReadsUsersAndDefaultsTheRealm) {
    setenv("OWL_BASIC_USERS", "alice:s3cret,bob:hunter2", 1);
    const auto config = owl::BasicAuth::make();
    EXPECT_EQ(config.realm, "restricted");
    ASSERT_EQ(config.users.size(), 2u);
    EXPECT_EQ(config.users.at("alice"), "s3cret");
    EXPECT_EQ(config.users.at("bob"), "hunter2");
    EXPECT_FALSE(config.verify);
}

TEST_F(EnvIsolated, MakeReadsTheRealm) {
    setenv("OWL_BASIC_USERS", "alice:s3cret", 1);
    setenv("OWL_BASIC_REALM", "admin", 1);
    EXPECT_EQ(owl::BasicAuth::make().realm, "admin");
}

TEST_F(EnvIsolated, MakeSplitsAtTheFirstColon) {
    setenv("OWL_BASIC_USERS", "alice:pa:ss", 1);
    EXPECT_EQ(owl::BasicAuth::make().users.at("alice"), "pa:ss");
}

TEST_F(EnvIsolated, MakeRejectsMalformedEntries) {
    for (const char* const bad : {"", "alice", "alice:", ":s3cret", "alice:s3cret,,bob:x", "alice:s3cret,", "alice:a,alice:b"}) {
        setenv("OWL_BASIC_USERS", bad, 1);
        EXPECT_THROW((void)owl::BasicAuth::make(), std::invalid_argument) << bad;
    }
}

TEST_F(EnvIsolated, MadeConfigGuardsARouter) {
    setenv("OWL_BASIC_USERS", "alice:s3cret", 1);
    const auto router = guarded(owl::BasicAuth::make());
    Fixture fixture;
    fixture.add_header("authorization", "Basic YWxpY2U6czNjcmV0"); // alice:s3cret
    fixture.run(router);
    EXPECT_EQ(fixture.req.res.status, 200);
}
