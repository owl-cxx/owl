#pragma once

// Basic credentials as they arrive: RFC 7617's `Basic <base64(user:password)>`.
// The parser lives here, below routing/, so a layer and a handler read the
// header the same way, and an extractor added later needs no second one.

#include <optional>
#include <string>
#include <string_view>

#include "owl/util/base64.h"
#include "owl/util/util.h"

namespace owl {
    struct Basic final {
        std::string user;
        std::string password;
    };

    // The scheme case-insensitively, one or more spaces (RFC 7235 allows
    // several), then the token. The first colon ends the user-id, which
    // RFC 7617 forbids from holding one, so a password may hold any. An
    // empty user-id is handed back as such: whether it admits anyone is
    // the verifier's call, not the parser's.
    [[nodiscard]] inline std::optional<Basic> parse_basic(std::string_view authorization) {
        constexpr std::string_view scheme = "Basic";
        if (authorization.size() <= scheme.size() || !util::eq_ci(authorization.substr(0, scheme.size()), scheme)) {
            return std::nullopt;
        }
        authorization.remove_prefix(scheme.size());
        if (authorization.front() != ' ') return std::nullopt;
        while (!authorization.empty() && authorization.front() == ' ') authorization.remove_prefix(1);
        if (authorization.empty()) return std::nullopt;

        const auto decoded = util::base64::decode(authorization);
        if (!decoded) return std::nullopt;
        const auto colon = decoded->find(':');
        if (colon == std::string::npos) return std::nullopt;
        return Basic{.user = decoded->substr(0, colon), .password = decoded->substr(colon + 1)};
    }
}
