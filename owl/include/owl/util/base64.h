#pragma once

// Base64 with the standard alphabet (RFC 4648 §4), both directions, on
// plain strings. h2o's pair is not used: its decoder reads the URL-safe
// alphabet and allocates from a request pool, and its encoder writes
// into a caller's buffer, which suits a handshake digest and little else.
//
// Table-driven and sized up front: one lookup per symbol, one write per
// byte, no growth checks, and no branch per symbol -- a bad one sets
// high bits that are tested once at the end. Full blocks are spelled
// out, which is what the compiler turns into the tightest loop; the
// short tail is one fold, so neither direction repeats itself for the
// two ways a block can be short. views::chunk would say all of this in
// one line, but Apple's libc++ does not have it yet, and folding every
// block was measured at two to four times the cost.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>

namespace owl::util::base64 {
    inline constexpr std::string_view alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

    // Indexed by byte, 256 entries so a lookup needs no range check. 0xff
    // marks a byte outside the alphabet; '=' is one, since padding is
    // stripped before decoding. A symbol's value is below 64, so its two
    // high bits are clear, and OR-ing values together leaves them clear
    // exactly when every byte was a symbol. Sixteen per row, the row's
    // first byte at the end of the line.
    inline constexpr std::array<std::uint8_t, 256> values = {
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, // 0x00
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, // 0x10
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,   62, 0xff, 0xff, 0xff,   63, // 0x20  + /
          52,   53,   54,   55,   56,   57,   58,   59,   60,   61, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, // 0x30  0-9
        0xff,    0,    1,    2,    3,    4,    5,    6,    7,    8,    9,   10,   11,   12,   13,   14, // 0x40  A-O
          15,   16,   17,   18,   19,   20,   21,   22,   23,   24,   25, 0xff, 0xff, 0xff, 0xff, 0xff, // 0x50  P-Z
        0xff,   26,   27,   28,   29,   30,   31,   32,   33,   34,   35,   36,   37,   38,   39,   40, // 0x60  a-o
          41,   42,   43,   44,   45,   46,   47,   48,   49,   50,   51, 0xff, 0xff, 0xff, 0xff, 0xff, // 0x70  p-z
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, // 0x80
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, // 0x90
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, // 0xa0
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, // 0xb0
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, // 0xc0
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, // 0xd0
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, // 0xe0
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, // 0xf0
    };

    // A typed-in table is checked, not trusted: every symbol maps to its
    // index, and nothing else maps at all.
    static_assert([] {
        std::size_t mapped = 0;
        for (const auto value : values) {
            if (value != 0xff) ++mapped;
        }
        if (mapped != alphabet.size()) return false;
        for (std::size_t i = 0; i < alphabet.size(); ++i) {
            if (static_cast<std::size_t>(values[static_cast<unsigned char>(alphabet[i])]) != i) return false;
        }
        return true;
    }(), "base64 decode table is not the inverse of the alphabet");

    namespace detail {
        [[nodiscard]] constexpr std::uint32_t value_of(const char symbol) noexcept {
            return values[static_cast<unsigned char>(symbol)];
        }

        [[nodiscard]] constexpr std::uint32_t shift_in_byte(const std::uint32_t acc, const char byte) noexcept {
            return (acc << 8u) | std::uint32_t{static_cast<unsigned char>(byte)};
        }

        [[nodiscard]] constexpr std::uint32_t shift_in_symbol(const std::uint32_t acc, const char symbol) noexcept {
            return (acc << 6u) | value_of(symbol);
        }

        [[nodiscard]] constexpr std::uint32_t mark(const std::uint32_t seen, const char symbol) noexcept {
            return seen | value_of(symbol);
        }
    }

    // Always padded: that is the form every decoder accepts.
    [[nodiscard]] inline std::string encode(const std::string_view bytes) {
        const auto at = [bytes](const std::size_t i) noexcept {
            return std::uint32_t{static_cast<unsigned char>(bytes[i])};
        };
        const std::size_t full = bytes.size() / 3;
        // Filled with '=' so the tail writes only its own symbols. Written
        // through a span rather than the string: a store through
        // string::operator[] may alias the small-string flag the next
        // operator[] must re-read, and that reload was measured at 1.7x.
        std::string out((bytes.size() + 2) / 3 * 4, '=');
        const std::span<char> symbols{out};

        for (const auto i : std::views::iota(std::size_t{0}, full)) {
            const std::size_t byte = 3 * i;
            const std::size_t symbol = 4 * i;
            const std::uint32_t triple = (at(byte) << 16u) | (at(byte + 1) << 8u) | at(byte + 2);
            symbols[symbol] = alphabet[triple >> 18u];
            symbols[symbol + 1] = alphabet[(triple >> 12u) & 63u];
            symbols[symbol + 2] = alphabet[(triple >> 6u) & 63u];
            symbols[symbol + 3] = alphabet[triple & 63u];
        }
        // One or two bytes left: shifted up as if the block were whole, then
        // only the symbols those bytes reach are written.
        if (const std::string_view tail = bytes.substr(full * 3); !tail.empty()) {
            const std::uint32_t triple = std::ranges::fold_left(tail, std::uint32_t{0}, detail::shift_in_byte) << (8u * (3 - tail.size()));
            const std::size_t symbol = 4 * full;
            std::uint32_t shift = 18;
            for (const auto s : std::views::iota(std::size_t{0}, tail.size() + 1)) {
                symbols[symbol + s] = alphabet[(triple >> shift) & 63u];
                shift -= 6;
            }
        }
        return out;
    }

    // Padding optional: clients pad, credentials typed by hand often do
    // not. Anything else -- '-' and '_', whitespace, '=' before the end --
    // is refused rather than guessed at.
    [[nodiscard]] inline std::optional<std::string> decode(std::string_view text) {
        std::size_t padding = 0;
        while (padding < 2 && !text.empty() && text.back() == '=') {
            text.remove_suffix(1);
            ++padding;
        }
        if (!text.empty() && text.back() == '=') return std::nullopt;
        if (padding != 0 && (text.size() + padding) % 4 != 0) return std::nullopt;
        const std::size_t rest = text.size() % 4;
        // Six bits on their own encode nothing.
        if (rest == 1) return std::nullopt;

        const auto at = [text](const std::size_t i) noexcept {
            return detail::value_of(text[i]);
        };
        const std::size_t full = text.size() / 4;
        const std::size_t tail_bytes = rest == 0 ? 0 : rest - 1;
        std::string out((full * 3) + tail_bytes, '\0');
        const std::span<char> bytes{out}; // see encode for why not the string

        // Output written for bad input is thrown away with the string.
        std::uint32_t seen = 0;
        for (const auto i : std::views::iota(std::size_t{0}, full)) {
            const std::size_t symbol = 4 * i;
            const std::size_t byte = 3 * i;
            const std::uint32_t a = at(symbol);
            const std::uint32_t b = at(symbol + 1);
            const std::uint32_t c = at(symbol + 2);
            const std::uint32_t d = at(symbol + 3);
            seen |= a | b | c | d;
            const std::uint32_t triple = (a << 18u) | (b << 12u) | (c << 6u) | d;
            bytes[byte] = static_cast<char>(triple >> 16u);
            bytes[byte + 1] = static_cast<char>(triple >> 8u);
            bytes[byte + 2] = static_cast<char>(triple);
        }
        // Two or three symbols left: the mirror of encode's tail.
        if (const std::string_view tail = text.substr(full * 4); !tail.empty()) {
            seen |= std::ranges::fold_left(tail, std::uint32_t{0}, detail::mark);
            const std::uint32_t triple = std::ranges::fold_left(tail, std::uint32_t{0}, detail::shift_in_symbol) << (6u * (4 - tail.size()));
            const std::size_t byte = 3 * full;
            std::uint32_t shift = 16;
            for (const auto k : std::views::iota(std::size_t{0}, tail_bytes)) {
                bytes[byte + k] = static_cast<char>(triple >> shift);
                shift -= 8;
            }
        }
        if ((seen & 0xc0u) != 0) return std::nullopt;
        return out;
    }
}
