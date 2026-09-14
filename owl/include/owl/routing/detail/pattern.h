#pragma once

// Route patterns: /users/{id}/posts/{pid}
//
// A segment is either a literal or exactly "{name}". Parsed at compile time
// so a bad pattern is a static_assert, not a startup exception.

#include <array>
#include <cstddef>
#include <string_view>

#include "owl/http/request.h"
#include "owl/util/util.h"

namespace owl {
    inline constexpr std::size_t max_path_segments = 32;
}

namespace owl::detail {
    struct Segment {
        std::string_view text; // literal text, or the parameter name
        bool is_param = false;
    };

    struct PatternInfo {
        bool ok = false;
        std::size_t count = 0;
        std::size_t params = 0;
        std::array<Segment, max_path_segments> segments{};
    };

    // Every early return is a rejected pattern; the comment on each says
    // why. The reason stays a comment: a static_assert cannot show a
    // runtime string, so nothing would read it.
    [[nodiscard]] constexpr PatternInfo
    parse_pattern(const std::string_view pattern) noexcept {
        PatternInfo info{};
        if (pattern.empty() || pattern.front() != '/') return info; // must start with '/'
        if (pattern.size() == 1) {
            // "/" is the root: zero segments, which is a valid route.
            info.ok = true;
            return info;
        }

        const std::size_t n = pattern.size();
        for (std::size_t i = 0; i < n;) {
            ++i; // skip the '/'
            const std::size_t start = i;
            while (i < n && pattern[i] != '/') ++i;
            const std::string_view seg = pattern.substr(start, i - start);

            if (seg.empty()) return info; // "//", or a trailing '/'
            if (info.count >= max_path_segments) return info; // too many segments

            if (seg.front() == '{') {
                if (seg.back() != '}') return info; // unterminated '{'
                const std::string_view name = seg.substr(1, seg.size() - 2);
                if (name.empty()) return info; // "{}"
                for (const char c : name) {
                    if (!util::is_word_char(c)) return info; // not a word character
                }
                for (std::size_t k = 0; k < info.count; ++k) {
                    if (info.segments[k].is_param && info.segments[k].text == name) return info; // duplicate name
                }
                if (info.params >= max_path_params) return info; // too many parameters
                ++info.params;
                info.segments[info.count++] = Segment{name, true};
            } else {
                for (const char c : seg) {
                    if (c == '{' || c == '}') return info; // a brace inside a literal
                }
                info.segments[info.count++] = Segment{seg, false};
            }
        }

        info.ok = true;
        return info;
    }

    // What split_path answers for a path that cannot match any route.
    inline constexpr std::size_t no_match = std::string_view::npos;

    // Splits a request path into segments. Returns no_match for anything
    // that cannot match: no leading '/', an empty segment, or too many.
    [[nodiscard]] inline std::size_t
    split_path(const std::string_view path, std::string_view* out, const std::size_t cap) noexcept {
        if (path.empty() || path.front() != '/') [[unlikely]] return no_match;
        if (path.size() == 1) return 0; // "/" is the root

        // Hand-rolled rather than `trimmed | std::views::split('/')`: this
        // runs on every request, almost always over one or two segments,
        // and the range adaptor's machinery does not survive that at -O3.
        // Semantics are unchanged, including that "//", a trailing '/' and
        // an over-long path all report no_match.
        const std::size_t n = path.size();
        std::size_t count = 0;
        std::size_t i = 1; // skip the leading '/'

        for (;;) {
            const std::size_t start = i;
            while (i < n && path[i] != '/') ++i;

            if (i == start) [[unlikely]] return no_match; // "//"
            if (count >= cap) [[unlikely]] return no_match;
            out[count++] = path.substr(start, i - start);

            if (i == n) return count;
            ++i; // step over the '/'
            if (i == n) [[unlikely]] return no_match; // trailing '/'
        }
    }

    template <fstr::fstr Pattern>
    [[nodiscard]] constexpr bool
    declares(const std::string_view name) noexcept {
        constexpr PatternInfo info = parse_pattern(Pattern.view());
        for (std::size_t i = 0; i < info.count; ++i) {
            if (info.segments[i].is_param && info.segments[i].text == name) return true;
        }
        return false;
    }
}
