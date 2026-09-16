### Task 3: `detail/placeholders.h` and `detail/stmt_key.h`

**Files:**
- Create: `sql/include/sql/detail/placeholders.h`, `sql/include/sql/detail/stmt_key.h`, `sql/tests/test_placeholders.cpp`
- Modify: `sql/CMakeLists.txt` (FILE_SET both headers + `sql_add_test(test_placeholders)`)

**Interfaces:**
- Produces: `struct sql::detail::placeholder_scan final { unsigned count; const char* error; }`; `constexpr placeholder_scan sql::detail::scan_placeholders(std::string_view) noexcept`; `template <fstr::fstr Q> struct sql::detail::stmt_key final` with `static constexpr const void* id`, `static constexpr std::string_view text`, `static constexpr const char* c_str` (NUL-terminated), `static constexpr placeholder_scan scan`.
- detail/ headers are not in the umbrella (the guard excludes `/detail/`).

- [ ] **Step 1: Write the failing tests**

`sql/tests/test_placeholders.cpp`:

```cpp
#include <gtest/gtest.h>

#include <string_view>

#include <fstr/fstr.h>

#include <sql/detail/placeholders.h>
#include <sql/detail/stmt_key.h>

using sql::detail::scan_placeholders;

TEST(Placeholders, CountsContiguousNumbers) {
    static_assert(scan_placeholders("SELECT 1").count == 0);
    static_assert(scan_placeholders("SELECT 1").error == nullptr);
    static_assert(scan_placeholders("SELECT $1").count == 1);
    static_assert(scan_placeholders("SELECT $1, $2, $3").count == 3);
    static_assert(scan_placeholders("SELECT $2, $1").count == 2);
    static_assert(scan_placeholders("SELECT $1 + $1").count == 1);
    static_assert(scan_placeholders("SELECT $10, $1, $2, $3, $4, $5, $6, $7, $8, $9").count == 10);
    EXPECT_EQ(scan_placeholders("SELECT $1, $2").count, 2u);
}

TEST(Placeholders, QuotedTextIsSkipped) {
    static_assert(scan_placeholders("SELECT '$1'").count == 0);
    static_assert(scan_placeholders("SELECT 'it''s $9', $1").count == 1);
    static_assert(scan_placeholders("SELECT '$2', $1").error == nullptr);
}

TEST(Placeholders, BareDollarIsNotAPlaceholder) {
    static_assert(scan_placeholders("SELECT $$ a $$").count == 0);
    static_assert(scan_placeholders("SELECT $abc").count == 0);
    static_assert(scan_placeholders("SELECT $").count == 0);
}

TEST(Placeholders, HolesAndBadNumbersAreErrors) {
    static_assert(scan_placeholders("SELECT $1, $3").error != nullptr);
    static_assert(scan_placeholders("SELECT $2").error != nullptr);
    static_assert(scan_placeholders("SELECT $0").error != nullptr);
    static_assert(scan_placeholders("SELECT $65").error != nullptr);
    EXPECT_STREQ(scan_placeholders("SELECT $1, $3").error, "placeholders must be contiguous from $1");
    EXPECT_STREQ(scan_placeholders("SELECT $0").error, "placeholder numbering starts at $1");
    EXPECT_STREQ(scan_placeholders("SELECT $65").error, "at most 64 placeholders");
}

TEST(StmtKey, OneIdentityPerLiteral) {
    using a = sql::detail::stmt_key<"SELECT $1">;
    using b = sql::detail::stmt_key<"SELECT $1">;
    using c = sql::detail::stmt_key<"SELECT $2, $1">;
    static_assert(a::id == b::id);
    static_assert(a::id != c::id);
    static_assert(a::text == std::string_view{"SELECT $1"});
    static_assert(a::scan.count == 1);
    static_assert(c::scan.count == 2);
    EXPECT_STREQ(a::c_str, "SELECT $1");
    EXPECT_EQ(a::c_str[a::text.size()], '\0');
}
```

- [ ] **Step 2: Register and run to see it fail**

Add both headers to the FILE_SET and `sql_add_test(test_placeholders)`.

Run: `cmake --build cmake-build-debug --target sql_test_placeholders`
Expected: FAIL, header not found.

- [ ] **Step 3: Write `sql/include/sql/detail/placeholders.h`**

```cpp
#pragma once

// Counts $N placeholders in a query literal at compile time. A constexpr
// function returning a status rather than a consteval that fails, so the
// rejection lives in query.h's static_asserts (where the literal is in the
// diagnostic) while the scanner itself stays unit-testable.
//
// Postgres reads $N natively; sqlite accepts it too (verified 2026-09-08),
// which is why one scanner serves both dialects. Text between single
// quotes is skipped ('' is the escape, which simply toggles twice). A $
// not followed by digits -- dollar quoting, $identifier -- is ignored;
// dollar-quoted bodies are documented as unsupported inside a literal.

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace sql::detail {
    struct placeholder_scan final {
        unsigned count = 0;
        const char* error = nullptr;
    };

    [[nodiscard]] constexpr placeholder_scan scan_placeholders(const std::string_view q) noexcept {
        std::uint64_t seen = 0;
        unsigned max = 0;
        bool quoted = false;
        for (std::size_t i = 0; i < q.size(); ++i) {
            const char c = q[i];
            if (quoted) {
                if (c == '\'') quoted = false;
                continue;
            }
            if (c == '\'') {
                quoted = true;
                continue;
            }
            if (c != '$') continue;
            std::size_t j = i + 1;
            unsigned n = 0;
            bool digits = false;
            while (j < q.size() && q[j] >= '0' && q[j] <= '9' && n <= 64) {
                n = n * 10 + static_cast<unsigned>(q[j] - '0');
                digits = true;
                ++j;
            }
            if (!digits) continue;
            if (n == 0) return {.count = 0, .error = "placeholder numbering starts at $1"};
            if (n > 64) return {.count = 0, .error = "at most 64 placeholders"};
            seen |= std::uint64_t{1} << (n - 1);
            if (n > max) max = n;
            i = j - 1;
        }
        for (unsigned k = 1; k <= max; ++k) {
            if ((seen & (std::uint64_t{1} << (k - 1))) == 0) {
                return {.count = max, .error = "placeholders must be contiguous from $1"};
            }
        }
        return {.count = max, .error = nullptr};
    }
}
```

- [ ] **Step 4: Write `sql/include/sql/detail/stmt_key.h`**

```cpp
#pragma once

// One identity per query literal. Each distinct fstr NTTP instantiates
// this template once, and the address of its tag is a constant expression
// unique to that instantiation -- a key for per-connection statement
// caches that costs nothing to compute and needs no hashing of the text.
// c_str exists because libpq wants a NUL-terminated command; fstr stores
// the NUL, so the literal's own buffer serves.

#include <string_view>

#include <fstr/fstr.h>

#include "sql/detail/placeholders.h"

namespace sql::detail {
    template <fstr::fstr Q>
    struct stmt_key final {
        static constexpr char tag = 0;
        static constexpr const void* id = &tag;
        static constexpr std::string_view text = Q.view();
        static constexpr const char* c_str = Q.data;
        static constexpr placeholder_scan scan = scan_placeholders(Q.view());
    };
}
```

- [ ] **Step 5: Run the test**

Run: `cmake --build cmake-build-debug --target sql_test_placeholders && ctest --test-dir cmake-build-debug -R '^sql_test_placeholders$' --output-on-failure`
Expected: PASS, 5 tests.

- [ ] **Step 6: Commit**

```bash
git add sql/include/sql/detail/placeholders.h sql/include/sql/detail/stmt_key.h sql/tests/test_placeholders.cpp sql/CMakeLists.txt
git commit -m "sql: compile-time placeholder scanner and per-literal stmt_key

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_013iahp2MjjdDsXN7sxWSNyJ"
```

---

