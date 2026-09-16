### Task 7: `query.h` — `try_query`, `query`, `try_execute`, `execute`

**Files:**
- Create: `sql/include/sql/query.h`, `sql/tests/test_query.cpp`, `sql/tests/compile_fail/rejects_arity_mismatch.cpp`, `sql/tests/compile_fail/rejects_placeholder_hole.cpp`, `sql/tests/compile_fail/rejects_unbindable_argument.cpp`
- Modify: `sql/CMakeLists.txt` (FILE_SET, `sql_add_test(test_query)`, three `sql_add_compile_fail_test(...)`), `sql/include/sql/sql.h` (add `#include "sql/query.h"`)

**Interfaces:**
- Consumes: `sql::source`, `sql::lease`, `sql::bindable`, `sql::detail::stmt_key<Q>::scan`, `sql::error`.
- Produces: `sql::result_t<S>` (= `S::driver::result`); the four functions with the exact signatures below.

```cpp
template <fstr::fstr Q, source S, bindable... Args>
[[nodiscard]] coro::task<std::expected<result_t<S>, error>> try_query(const S& src, const Args&... args);
template <fstr::fstr Q, source S, bindable... Args>
[[nodiscard]] coro::task<result_t<S>> query(const S& src, const Args&... args);            // throws sql::error
template <fstr::fstr Q, source S, bindable... Args>
[[nodiscard]] coro::task<std::expected<std::uint64_t, error>> try_execute(const S& src, const Args&... args);
template <fstr::fstr Q, source S, bindable... Args>
[[nodiscard]] coro::task<std::uint64_t> execute(const S& src, const Args&... args);        // throws sql::error
```

- [ ] **Step 1: Write the failing tests**

`sql/tests/test_query.cpp`:

```cpp
#include <gtest/gtest.h>

#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <type_traits>
#include <vector>

#include <coro/task.h>

#include <sql/error.h>
#include <sql/pool.h>
#include <sql/query.h>

#include "support/fake_driver.h"

namespace {
    using fake = sql_test::fake;
    using pool_t = sql::pool<fake>;

    struct fixture final {
        fake::script s;
        pool_t pool{{.connections = 1, .script_ = &s}, {}};
    };

    void drive(coro::task<>&& t) {
        t.start();
        EXPECT_TRUE(t.done());
    }
}

TEST(Query, TypesLineUpWithTheDriver) {
    static_assert(std::is_same_v<sql::result_t<pool_t>, fake::result>);
    static_assert(std::is_same_v<decltype(sql::try_query<"SELECT 1">(std::declval<const pool_t&>())),
                                 coro::task<std::expected<fake::result, sql::error>>>);
    static_assert(std::is_same_v<decltype(sql::query<"SELECT 1">(std::declval<const pool_t&>())),
                                 coro::task<fake::result>>);
    static_assert(std::is_same_v<decltype(sql::try_execute<"SELECT 1">(std::declval<const pool_t&>())),
                                 coro::task<std::expected<std::uint64_t, sql::error>>>);
    static_assert(std::is_same_v<decltype(sql::execute<"SELECT 1">(std::declval<const pool_t&>())),
                                 coro::task<std::uint64_t>>);
}

TEST(Query, TryQueryChecksOutRunsAndReleasesBeforeReturning) {
    fixture fx;
    fx.s.rows.push_back({{"7"}});
    drive([&]() -> coro::task<> {
        const auto r = co_await sql::try_query<"SELECT $1">(fx.pool, 7);
        EXPECT_TRUE(r.has_value());
        EXPECT_EQ(fx.pool.idle(), 1u);
        EXPECT_EQ((*r)[0][0].as<int>(), 7);
        EXPECT_EQ(fx.s.log, (std::vector<std::string>{"SELECT $1"}));
    }());
}

TEST(Query, TryQueryReportsCheckoutFailure) {
    fixture fx;
    fx.s.opens.emplace_back(std::unexpected(sql::error{sql::error_kind::connect, "refused"}));
    drive([&]() -> coro::task<> {
        const auto r = co_await sql::try_query<"SELECT 1">(fx.pool);
        EXPECT_FALSE(r.has_value());
        EXPECT_EQ(r.error().kind(), sql::error_kind::connect);
        EXPECT_TRUE(fx.s.log.empty());
    }());
}

TEST(Query, QueryThrowsTheDriversError) {
    fixture fx;
    fx.s.runs.emplace_back(std::unexpected(sql::error{sql::error_kind::query, "syntax", "42601"}));
    drive([&]() -> coro::task<> {
        bool thrown = false;
        try {
            (void)co_await sql::query<"SELECT 1">(fx.pool);
        } catch (const sql::error& e) {
            thrown = e.kind() == sql::error_kind::query && e.sqlstate() == "42601";
        }
        EXPECT_TRUE(thrown);
        EXPECT_EQ(fx.pool.idle(), 1u);
    }());
}

TEST(Query, TryExecuteMapsToAffected) {
    fixture fx;
    fx.s.runs.emplace_back(std::uint64_t{3});
    drive([&]() -> coro::task<> {
        const auto n = co_await sql::try_execute<"DELETE FROM t WHERE a = $1 AND b = $2">(fx.pool, 1, std::string{"x"});
        EXPECT_EQ(n.value(), 3u);
    }());
}

TEST(Query, ExecuteThrowsAndReturnsTheCount) {
    fixture fx;
    fx.s.runs.emplace_back(std::uint64_t{2});
    fx.s.runs.emplace_back(std::unexpected(sql::error{sql::error_kind::connection, "gone"}));
    drive([&]() -> coro::task<> {
        EXPECT_EQ(co_await sql::execute<"UPDATE t SET a = $1">(fx.pool, std::optional<int>{}), 2u);
        bool thrown = false;
        try {
            (void)co_await sql::execute<"UPDATE t SET a = $1">(fx.pool, 1);
        } catch (const sql::error& e) {
            thrown = e.kind() == sql::error_kind::connection;
        }
        EXPECT_TRUE(thrown);
        EXPECT_EQ(fx.pool.size(), 0u);
    }());
}

TEST(Query, EveryBindableSpellingIsAccepted) {
    fixture fx;
    const std::string owned = "s";
    const sql::blob bytes{std::byte{1}};
    drive([&]() -> coro::task<> {
        const auto r = co_await sql::try_query<"SELECT $1, $2, $3, $4, $5, $6, $7, $8">(
            fx.pool, true, 1, 2.5, owned, "lit", bytes, std::nullopt, std::optional<double>{1.0});
        EXPECT_TRUE(r.has_value());
    }());
}
```

`sql/tests/compile_fail/rejects_arity_mismatch.cpp`:

```cpp
// Two placeholders, one argument: the static_assert in try_query fires at
// the call site.
#include <sql/pool.h>
#include <sql/query.h>

#include "support/fake_driver.h"

int main() {
    sql_test::fake::script s;
    const sql::pool<sql_test::fake> pool{{.connections = 1, .script_ = &s}, {}};
    auto t = sql::try_query<"SELECT $1, $2">(pool, 1);
    (void)t;
}
```

`sql/tests/compile_fail/rejects_placeholder_hole.cpp`:

```cpp
// $1 and $3 with no $2: the scanner reports the hole and the first
// static_assert in try_query fires.
#include <sql/pool.h>
#include <sql/query.h>

#include "support/fake_driver.h"

int main() {
    sql_test::fake::script s;
    const sql::pool<sql_test::fake> pool{{.connections = 1, .script_ = &s}, {}};
    auto t = sql::try_query<"SELECT $1, $3">(pool, 1, 2);
    (void)t;
}
```

`sql/tests/compile_fail/rejects_unbindable_argument.cpp`:

```cpp
// A std::vector<int> is not in the bindable vocabulary, so the constraint
// on Args fails.
#include <vector>

#include <sql/pool.h>
#include <sql/query.h>

#include "support/fake_driver.h"

int main() {
    sql_test::fake::script s;
    const sql::pool<sql_test::fake> pool{{.connections = 1, .script_ = &s}, {}};
    auto t = sql::try_query<"SELECT $1">(pool, std::vector<int>{1});
    (void)t;
}
```

- [ ] **Step 2: Register and run to see it fail**

Add `include/sql/query.h` to the FILE_SET, `sql_add_test(test_query)`, the three `sql_add_compile_fail_test(rejects_arity_mismatch)`, `(rejects_placeholder_hole)`, `(rejects_unbindable_argument)`, and `#include "sql/query.h"` to `sql.h` (alphabetical: `concepts`, `convert`, `error`, `io`, `pool`, `query`).

Run: `cmake --build cmake-build-debug --target sql_test_query`
Expected: FAIL, header not found.

- [ ] **Step 3: Write `sql/include/sql/query.h`**

```cpp
#pragma once

// The four entry points. try_query is the primitive: check out a lease,
// run the literal on it, hand back the driver's expected untouched. The
// other three are thin over it: query throws that error, try_execute maps
// the result to affected(), execute throws. The two static_asserts make a
// malformed or mis-counted placeholder list a compile error at the call
// site, where the literal is in the diagnostic.
//
// Arguments are borrowed for the life of the returned task -- the frame
// holds references, not copies -- so await in the same full-expression
// that built the task, exactly as with when_all over a non-movable child;
// naming the task and awaiting it later with temporaries as arguments
// dangles. The lease is released before the result is handed back, which
// is correct because neither driver's result refers to its connection.

#include <cstdint>
#include <expected>
#include <utility>

#include <coro/task.h>
#include <fstr/fstr.h>

#include "sql/convert.h"
#include "sql/detail/stmt_key.h"
#include "sql/error.h"
#include "sql/pool.h"

namespace sql {
    template <source S>
    using result_t = typename S::driver::result;

    template <fstr::fstr Q, source S, bindable... Args>
    [[nodiscard]] coro::task<std::expected<result_t<S>, error>> try_query(const S& src, const Args&... args) {
        static_assert(detail::stmt_key<Q>::scan.error == nullptr,
                      "sql: malformed placeholders -- $N must run contiguously from $1, at most $64");
        static_assert(detail::stmt_key<Q>::scan.count == sizeof...(Args),
                      "sql: the query's $N placeholder count does not match the argument count");
        auto lease = co_await src.checkout();
        if (!lease) co_return std::unexpected(std::move(lease.error()));
        co_return co_await (*lease)->template execute<Q>(args...);
    }

    template <fstr::fstr Q, source S, bindable... Args>
    [[nodiscard]] coro::task<result_t<S>> query(const S& src, const Args&... args) {
        auto r = co_await try_query<Q>(src, args...);
        if (!r) throw std::move(r.error());
        co_return std::move(*r);
    }

    template <fstr::fstr Q, source S, bindable... Args>
    [[nodiscard]] coro::task<std::expected<std::uint64_t, error>> try_execute(const S& src, const Args&... args) {
        auto r = co_await try_query<Q>(src, args...);
        co_return std::move(r).transform([](const result_t<S>& res) {
            return static_cast<std::uint64_t>(res.affected());
        });
    }

    template <fstr::fstr Q, source S, bindable... Args>
    [[nodiscard]] coro::task<std::uint64_t> execute(const S& src, const Args&... args) {
        auto r = co_await try_execute<Q>(src, args...);
        if (!r) throw std::move(r.error());
        co_return *r;
    }
}
```

- [ ] **Step 4: Run the tests, including the three that must fail to compile**

Run: `cmake --build cmake-build-debug --target sql_test_query sql_headers && ctest --test-dir cmake-build-debug -R '^sql_test_query$|^rejects_arity_mismatch$|^rejects_placeholder_hole$|^rejects_unbindable_argument$' --output-on-failure`
Expected: 4 tests passed (the three `rejects_*` pass because their build fails, `WILL_FAIL`). Open the ctest log for one `rejects_*` and confirm the failure is the intended `static_assert` / constraint, not an unrelated error.

- [ ] **Step 5: Commit**

```bash
git add sql/include/sql/query.h sql/tests/test_query.cpp sql/tests/compile_fail/rejects_arity_mismatch.cpp \
        sql/tests/compile_fail/rejects_placeholder_hole.cpp sql/tests/compile_fail/rejects_unbindable_argument.cpp \
        sql/CMakeLists.txt sql/include/sql/sql.h
git commit -m "sql: query front end -- try_query, query, try_execute, execute

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_013iahp2MjjdDsXN7sxWSNyJ"
```

---

