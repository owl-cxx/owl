#pragma once

// CORS as one layer. A preflight is answered here and never reaches what
// is inside: an auth layer would 401 an OPTIONS that, by design, carries
// no credentials, and the browser would then refuse the real request. So
// this layer goes outermost. Every other request with an Origin runs
// through and has the grant stamped on the way out. One without an Origin
// runs untouched, and so does one from an origin not listed: CORS is the
// browser's gate, and a request the server can see was already sent.
//
// The trie answers OPTIONS on any route that registered methods, which is
// what lets a preflight reach a layer at all: dispatch sends 405 before
// any chain runs.

#include <algorithm>
#include <chrono>
#include <format>
#include <optional>
#include <ranges>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <coro/task.h>

#include "owl/core/method.h"
#include "owl/http/request.h"
#include "owl/http/response.h"
#include "owl/routing/middleware.h"
#include "owl/util/util.h"

namespace owl {
    struct Cors final {
        // Exact origins, scheme://host[:port]; "*" grants any. The browser
        // compares what comes back byte for byte, so nothing is normalised.
        std::vector<std::string> origins{"*"};
        // What a preflight is told. Empty mirrors the requested method, so a
        // method the route lacks goes on to the route's own 405 rather than
        // dying as an opaque CORS error in the browser.
        std::vector<Method> methods;
        // Same, for headers: empty mirrors Access-Control-Request-Headers.
        std::vector<std::string> headers;
        // Response headers a script may read beyond the safelisted few.
        std::vector<std::string> expose;
        bool credentials = false;
        std::optional<std::chrono::seconds> max_age;
    };

    namespace detail {
        template <typename R>
        [[nodiscard]] std::string comma_list(const R& items) {
            std::string out;
            for (const auto& item : items) {
                if (!out.empty()) out += ", ";
                out += item;
            }
            return out;
        }

        // Appends to Vary rather than replacing it: a handler's own Vary
        // is still the truth about its response.
        inline void add_vary(Response& res, const std::string_view token) {
            const auto current = res.header("vary");
            if (!current) {
                res.header("vary", token);
                return;
            }
            for (const auto part : *current | std::views::split(',')) {
                auto piece = std::string_view{part};
                while (!piece.empty() && piece.front() == ' ') piece.remove_prefix(1);
                if (util::eq_ci(piece, token)) return;
            }
            res.header("vary", std::format("{}, {}", *current, token));
        }

        class CorsLayer final {
        public:
            explicit CorsLayer(Cors config)
                : origins_(std::move(config.origins)),
                  any_origin_(std::ranges::contains(origins_, "*")),
                  methods_(comma_list(config.methods | std::views::transform([](const Method m) { return to_string(m); }))),
                  headers_(comma_list(config.headers)),
                  expose_(comma_list(config.expose)),
                  credentials_(config.credentials),
                  max_age_(config.max_age ? std::to_string(config.max_age->count()) : std::string{}) {
                // The Fetch spec refuses "*" with credentials, and echoing the
                // origin instead would grant every site the user's cookies. A
                // list of origins is the only honest spelling.
                if (credentials_ && any_origin_) {
                    throw std::invalid_argument("owl::cors: credentials cannot be granted to any origin (\"*\"); list the origins");
                }
            }

            template <typename S>
            coro::task<Response> operator()(const Request& req, Next<S> next) const {
                const auto origin = req.header("origin");
                if (!origin) co_return co_await next(req);

                const bool granted = allows(*origin);
                if (req.method() == Method::Options) {
                    if (const auto requested = req.header("access-control-request-method")) {
                        co_return preflight(*origin, granted, *requested, req.header("access-control-request-headers"));
                    }
                }

                Response res = co_await next(req);
                if (granted) grant(res, *origin);
                co_return std::move(res);
            }

        private:
            [[nodiscard]] bool allows(const std::string_view origin) const noexcept {
                return any_origin_ || std::ranges::contains(origins_, origin);
            }

            // The part every granted response carries.
            void grant(Response& res, const std::string_view origin) const {
                if (any_origin_) {
                    res.header("access-control-allow-origin", "*");
                } else {
                    res.header("access-control-allow-origin", origin);
                    add_vary(res,"Origin");
                }
                if (credentials_) res.header("access-control-allow-credentials", "true");
                if (!expose_.empty()) res.header("access-control-expose-headers", expose_);
            }

            [[nodiscard]] Response preflight(
                const std::string_view origin,
                const bool granted,
                const std::string_view requested_method,
                const std::optional<std::string_view> requested_headers
            ) const {
                Response res = Response::no_content();
                if (!granted) return res;

                grant(res, origin);
                // Mirrored answers depend on the request, so a cache must key
                // on the header they mirror; configured ones do not.
                if (methods_.empty()) {
                    res.header("access-control-allow-methods", requested_method);
                    add_vary(res,"Access-Control-Request-Method");
                } else {
                    res.header("access-control-allow-methods", methods_);
                }
                if (headers_.empty()) {
                    if (requested_headers) res.header("access-control-allow-headers", *requested_headers);
                    add_vary(res,"Access-Control-Request-Headers");
                } else {
                    res.header("access-control-allow-headers", headers_);
                }
                if (!max_age_.empty()) res.header("access-control-max-age", max_age_);
                return res;
            }

            std::vector<std::string> origins_;
            bool any_origin_;
            std::string methods_;
            std::string headers_;
            std::string expose_;
            bool credentials_;
            std::string max_age_;
        };
    }

    // Validates once, here, so a bad policy fails at startup rather than
    // as a 500 on the first cross-origin request.
    [[nodiscard]] inline detail::CorsLayer cors(Cors config) {
        return detail::CorsLayer{std::move(config)};
    }
}
