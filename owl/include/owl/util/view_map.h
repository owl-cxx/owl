#pragma once

// A map of views: keys and values are string_views into memory that
// outlives the map -- a request's pool, for the query string -- so it
// owns nothing and copies nothing. The name says what it holds, like
// PathView and HeaderView, rather than who owns the bytes.

#include <concepts>
#include <functional>
#include <ranges>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace owl {
    class view_map final : public std::unordered_map<std::string_view, std::string_view> {
        using base = std::unordered_map<std::string_view, std::string_view>;

    public:
        using base::at;
        using base::contains;
        using base::end;
        using base::find;

        view_map() = default;

        // The from_range constructor ranges::to looks for. Filled by hand
        // rather than forwarded to the base: libstdc++ 14 has no from_range
        // constructor on unordered_map yet, and one insert per pair is what
        // it would do anyway.
        template <std::ranges::input_range R> requires std::convertible_to<std::ranges::range_reference_t<R>, value_type>
        view_map(std::from_range_t, R&& r) {
            for (auto&& kv : r) base::insert(static_cast<value_type>(std::forward<decltype(kv)>(kv)));
        }

        [[nodiscard]] const std::string_view* find_value(const std::string_view key) const noexcept {
            if (const auto it = find(key); it != end()) {
                return &it->second;
            }
            return nullptr;
        }

        template <typename Func>
        std::string_view& compute_if_absent(const std::string_view key, Func&& compute) {
            if (const auto it = find(key); it != end()) {
                return it->second;
            }
            return emplace(key, std::invoke(std::forward<Func>(compute))).first->second;
        }
    };
}
