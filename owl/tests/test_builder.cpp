#include <cstdint>
#include <gtest/gtest.h>
#include <memory>
#include <stdexcept>
#include <utility>

#include <owl/server.h>

namespace {
    struct AppState final {
        int n = 0;
    };
}

TEST(Builder, MissingRouterThrows) {
    EXPECT_THROW(
        (void)owl::Server<AppState>::builder().build_with(std::make_shared<AppState>()),
        std::invalid_argument);
}

TEST(Builder, MissingConfigThrows) {
    EXPECT_THROW(
        (void)owl::Server<AppState>::builder()
            .router(owl::Router<AppState>::make())
            .build_with(std::make_shared<AppState>()),
        std::invalid_argument);
}

TEST(Builder, DoubleRouterThrows) {
    EXPECT_THROW(
        (void)owl::Server<AppState>::builder().router(owl::Router<AppState>::make()).router(owl::Router<AppState>::make()),
        std::invalid_argument);
}

TEST(Builder, DoubleConfigThrows) {
    EXPECT_THROW(
        (void)owl::Server<AppState>::builder().config({}).config({}),
        std::invalid_argument);
}

TEST(Builder, ThreadThenConfigThrows) {
    EXPECT_THROW(
        (void)owl::Server<AppState>::builder().thread(4).config({}),
        std::invalid_argument);
}

TEST(Builder, ThreadAfterConfigOk) {
    EXPECT_NO_THROW((void)owl::Server<AppState>::builder().config({}).thread(4));
}

TEST(Builder, ThreadAloneOk) {
    EXPECT_NO_THROW((void)owl::Server<AppState>::builder().thread(4));
}

TEST(Builder, BuildWithNullStateThrows) {
    EXPECT_THROW(
        (void)owl::Server<AppState>::builder()
            .router(owl::Router<AppState>::make())
            .config({})
            .build_with(nullptr),
        std::invalid_argument);
}

TEST(Builder, DestroyedServerReleasesItsState) {
    auto state = std::make_shared<AppState>();
    const std::weak_ptr<AppState> watch = state;
    {
        const auto server = owl::Server<AppState>::builder()
                                .router(owl::Router<AppState>::make())
                                .config({.port = 0})
                                .build_with(std::move(state));
        EXPECT_NE(server.port(), 0);
    }
    EXPECT_TRUE(watch.expired()) << "the dispatcher inside the h2o config still holds the state";
}

TEST(Builder, DestroyedServerFreesItsPort) {
    std::uint16_t port = 0;
    {
        const auto server = owl::Server<AppState>::builder()
                                .router(owl::Router<AppState>::make())
                                .config({.port = 0})
                                .build_with(std::make_shared<AppState>());
        port = server.port();
    }
    EXPECT_NO_THROW((void)owl::Server<AppState>::builder()
                        .router(owl::Router<AppState>::make())
                        .config({.port = port})
                        .build_with(std::make_shared<AppState>()));
}

// A constructor that throws never runs its destructor, so this is what
// proves the teardown lives in the members rather than in ~Server.
TEST(Builder, FailedBindReleasesItsState) {
    auto state = std::make_shared<AppState>();
    const std::weak_ptr<AppState> watch = state;
    EXPECT_THROW((void)owl::Server<AppState>::builder()
                     .router(owl::Router<AppState>::make())
                     .config({.address = "not-an-address", .port = 0})
                     .build_with(std::move(state)),
                 std::invalid_argument);
    EXPECT_TRUE(watch.expired());
}
