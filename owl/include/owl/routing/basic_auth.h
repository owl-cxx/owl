#pragma once

// Basic authentication as one layer over the routes it is attached to.
// Missing, malformed and wrong credentials are all answered 401 with a
// challenge and never reach next: a 400 for a malformed header would
// give a browser nothing to prompt with. What passes is decided once,
// here -- a listed user with the matching password, or verify saying
// yes. The realm is a label for the browser's credential cache, not a
// scope; where the layer sits is the scope.

#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <format>
#include <functional>
#include <ranges>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

#include <openssl/crypto.h>

#include <coro/task.h>

#include "owl/http/auth.h"
#include "owl/http/request.h"
#include "owl/http/response.h"
#include "owl/routing/middleware.h"

namespace owl {
    struct BasicAuth final {
        // Sent in the challenge, so it must fit a quoted-string. Defaulted
        // rather than required: one gate on one user set never needs it.
        std::string realm{"restricted"};
        std::unordered_map<std::string, std::string> users;
        // Consulted when the user is not listed or the password differs.
        // Runs on the worker loop, so it must not block; a database check
        // belongs in a handler.
        std::function<bool(std::string_view user, std::string_view password)> verify;

        // OWL_BASIC_USERS is user:password pairs separated by commas. The
        // first colon splits, so a password may hold colons but not commas.
        // OWL_BASIC_REALM is optional. An empty or repeated entry throws
        // rather than being skipped: a typo here is a gate that admits the
        // wrong people. Messages name the entry's position, never its text.
        [[nodiscard]] static BasicAuth make() {
            BasicAuth config{};
            if (const char* const realm = std::getenv("OWL_BASIC_REALM")) config.realm = realm;
            const char* const users = std::getenv("OWL_BASIC_USERS");
            if (users == nullptr) throw std::invalid_argument("owl::BasicAuth: OWL_BASIC_USERS is not set");
            // split yields nothing for an empty string, so the loop below
            // would accept it as a gate with no users.
            if (*users == '\0') throw std::invalid_argument("owl::BasicAuth: OWL_BASIC_USERS is empty");

            std::size_t position = 0;
            for (const auto part : std::string_view{users} | std::views::split(',')) {
                ++position;
                const std::string_view entry{part};
                const auto colon = entry.find(':');
                if (colon == 0 || colon == std::string_view::npos || colon == entry.size() - 1) {
                    throw std::invalid_argument(std::format("owl::BasicAuth: OWL_BASIC_USERS entry {} is not user:password", position));
                }
                if (!config.users.emplace(entry.substr(0, colon), entry.substr(colon + 1)).second) {
                    throw std::invalid_argument(std::format("owl::BasicAuth: OWL_BASIC_USERS lists {} twice", entry.substr(0, colon)));
                }
            }
            return config;
        }
    };

    namespace detail {
        class BasicAuthLayer final {
        public:
            explicit BasicAuthLayer(BasicAuth config)
                : users_(std::move(config.users)),
                  verify_(std::move(config.verify)) {
                if (users_.empty() && !verify_) {
                    throw std::invalid_argument("owl::basic_auth: no users and no verify, so nobody could get in");
                }
                const auto unquotable = [](const char c) {
                    return c == '"' || c == '\\' || static_cast<unsigned char>(c) < 0x20 || c == 0x7f;
                };
                if (config.realm.empty() || std::ranges::any_of(config.realm, unquotable)) {
                    throw std::invalid_argument("owl::basic_auth: realm must be non-empty and free of quotes, backslashes and control characters");
                }
                challenge_ = std::format(R"(Basic realm="{}", charset="UTF-8")", config.realm);
            }

            template <typename S>
            coro::task<Response> operator()(const Request& req, Next<S> next) const {
                if (const auto header = req.header("authorization")) {
                    if (const auto credentials = parse_basic(*header); credentials && admits(*credentials)) {
                        co_return co_await next(req);
                    }
                }
                co_return Response::ok("unauthorized", 401).header("www-authenticate", challenge_);
            }

        private:
            [[nodiscard]] bool admits(const Basic& credentials) const {
                if (const auto found = users_.find(credentials.user); found != users_.end() && same(found->second, credentials.password)) {
                    return true;
                }
                return verify_ && verify_(credentials.user, credentials.password);
            }

            // Constant time over equal lengths, so where a wrong password
            // first differs is not measurable from outside.
            [[nodiscard]] static bool same(const std::string_view expected, const std::string_view given) noexcept {
                return expected.size() == given.size() && CRYPTO_memcmp(expected.data(), given.data(), expected.size()) == 0;
            }

            std::unordered_map<std::string, std::string> users_;
            std::function<bool(std::string_view, std::string_view)> verify_;
            std::string challenge_;
        };
    }

    // Validates once, here, so an empty gate or an unquotable realm fails
    // at startup rather than on the first request.
    [[nodiscard]] inline detail::BasicAuthLayer basic_auth(BasicAuth config) {
        return detail::BasicAuthLayer{std::move(config)};
    }
}
