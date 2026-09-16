### Task 8: `transaction.h`

**Files:**
- Create: `sql/include/sql/transaction.h`, `sql/tests/test_transaction.cpp`, `sql/tests/compile_fail/rejects_nested_transaction.cpp`, `sql/tests/compile_fail/rejects_transaction_body_returning_task_t.cpp`, `sql/tests/compile_fail/rejects_transaction_body_not_a_task.cpp`
- Modify: `sql/CMakeLists.txt` (FILE_SET, `sql_add_test(test_transaction)`, three compile-fail registrations), `sql/include/sql/sql.h` (add `#include "sql/transaction.h"`)

**Interfaces:**
- Consumes: `sql::pool`, `sql::lease`, `sql::driver`, `sql::error`, `connection::execute<Q>()`, `connection::abandon()`.
- Produces: `template <driver D> class sql::transaction_source final` (copyable; `using driver = D`; `checkout() const`); `sql::transaction_result<Task>` (specialized for `coro::task<std::expected<T, error>>` with `::type = T`); `sql::transaction_body_result_t<F, D>`; concept `sql::transaction_body<F, D>`; `sql::transaction_value_t<F, D>`; `template <driver D, transaction_body<D> F> coro::task<std::expected<transaction_value_t<F, D>, error>> sql::transaction(const pool<D>&, F body)`.

- [ ] **Step 1: Write the failing tests**

`sql/tests/test_transaction.cpp`:

```cpp
#include <gtest/gtest.h>

#include <expected>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

#include <coro/task.h>

#include <sql/error.h>
#include <sql/pool.h>
#include <sql/query.h>
#include <sql/transaction.h>

#include "support/fake_driver.h"

namespace {
    using fake = sql_test::fake;
    using pool_t = sql::pool<fake>;
    using tx_t = sql::transaction_source<fake>;
    using outcome = std::expected<int, sql::error>;

    struct fixture final {
        fake::script s;
        pool_t pool{{.connections = 1, .script_ = &s}, {}};
    };

    void drive(coro::task<>&& t) {
        t.start();
        EXPECT_TRUE(t.done());
    }

    const std::vector<std::string> begin_commit{"BEGIN", "SELECT 1", "COMMIT"};
    const std::vector<std::string> begin_rollback{"BEGIN", "SELECT 1", "ROLLBACK"};
}

TEST(TransactionBody, AcceptsEverySpellingOfTheSourceParameter) {
    const auto by_value = [](auto tx) -> coro::task<outcome> { (void)tx; co_return 1; };
    const auto by_ref = [](auto& tx) -> coro::task<outcome> { (void)tx; co_return 1; };
    const auto by_cref = [](const auto& tx) -> coro::task<outcome> { (void)tx; co_return 1; };
    const auto typed = [](tx_t& tx) -> coro::task<std::expected<std::string, sql::error>> { (void)tx; co_return "x"; };
    static_assert(sql::transaction_body<decltype(by_value), fake>);
    static_assert(sql::transaction_body<decltype(by_ref), fake>);
    static_assert(sql::transaction_body<decltype(by_cref), fake>);
    static_assert(sql::transaction_body<decltype(typed), fake>);
    static_assert(std::is_same_v<sql::transaction_value_t<decltype(by_value), fake>, int>);
    static_assert(std::is_same_v<sql::transaction_value_t<decltype(typed), fake>, std::string>);
    static_assert(sql::source<tx_t>);
    static_assert(std::is_copy_constructible_v<tx_t>);
}

TEST(TransactionBody, RejectsOtherShapes) {
    const auto plain_task = [](tx_t&) -> coro::task<int> { co_return 1; };
    const auto not_a_task = [](tx_t&) -> outcome { return 1; };
    const auto wrong_arg = [](int) -> coro::task<outcome> { co_return 1; };
    static_assert(!sql::transaction_body<decltype(plain_task), fake>);
    static_assert(!sql::transaction_body<decltype(not_a_task), fake>);
    static_assert(!sql::transaction_body<decltype(wrong_arg), fake>);
}

TEST(Transaction, CommitsOnAValue) {
    fixture fx;
    fx.s.rows.push_back({});
    fx.s.rows.push_back({{"42"}});
    drive([&]() -> coro::task<> {
        const auto r = co_await sql::transaction(fx.pool, [&](auto tx) -> coro::task<outcome> {
            EXPECT_EQ(fx.pool.idle(), 0u);
            const auto q = co_await sql::try_query<"SELECT 1">(tx);
            if (!q) co_return std::unexpected(q.error());
            co_return (*q)[0][0].template as<int>();
        });
        EXPECT_EQ(r.value(), 42);
        EXPECT_EQ(fx.s.log, begin_commit);
        EXPECT_EQ(fx.s.opened, 1);
        EXPECT_EQ(fx.pool.idle(), 1u);
    }());
}

TEST(Transaction, RollsBackOnABodyError) {
    fixture fx;
    drive([&]() -> coro::task<> {
        const auto r = co_await sql::transaction(fx.pool, [](auto& tx) -> coro::task<outcome> {
            (void)co_await sql::try_query<"SELECT 1">(tx);
            co_return std::unexpected(sql::error{sql::error_kind::query, "nope"});
        });
        EXPECT_EQ(r.error().kind(), sql::error_kind::query);
        EXPECT_STREQ(r.error().what(), "nope");
        EXPECT_EQ(fx.s.log, begin_rollback);
        EXPECT_EQ(fx.pool.idle(), 1u);
        EXPECT_EQ(fx.s.destroyed, 0);
    }());
}

TEST(Transaction, RollbackFailureLosesToTheBodyErrorAndAbandons) {
    fixture fx;
    fx.s.runs.emplace_back(std::uint64_t{0});   // BEGIN
    fx.s.runs.emplace_back(std::uint64_t{0});   // SELECT 1
    fx.s.runs.emplace_back(std::unexpected(sql::error{sql::error_kind::query, "rollback failed"}));
    drive([&]() -> coro::task<> {
        const auto r = co_await sql::transaction(fx.pool, [](auto tx) -> coro::task<outcome> {
            (void)co_await sql::try_query<"SELECT 1">(tx);
            co_return std::unexpected(sql::error{sql::error_kind::query, "body"});
        });
        EXPECT_STREQ(r.error().what(), "body");
        EXPECT_EQ(fx.s.log, begin_rollback);
        EXPECT_EQ(fx.s.destroyed, 1);
        EXPECT_EQ(fx.pool.size(), 0u);
    }());
}

TEST(Transaction, BeginFailureIsReportedAndTheBodyNeverRuns) {
    fixture fx;
    fx.s.runs.emplace_back(std::unexpected(sql::error{sql::error_kind::query, "no begin"}));
    drive([&]() -> coro::task<> {
        bool ran = false;
        const auto r = co_await sql::transaction(fx.pool, [&](auto) -> coro::task<outcome> {
            ran = true;
            co_return 1;
        });
        EXPECT_STREQ(r.error().what(), "no begin");
        EXPECT_FALSE(ran);
        EXPECT_EQ(fx.s.log, (std::vector<std::string>{"BEGIN"}));
    }());
}

TEST(Transaction, CommitFailureIsReported) {
    fixture fx;
    fx.s.runs.emplace_back(std::uint64_t{0});
    fx.s.runs.emplace_back(std::unexpected(sql::error{sql::error_kind::query, "no commit"}));
    drive([&]() -> coro::task<> {
        const auto r = co_await sql::transaction(fx.pool, [](auto) -> coro::task<outcome> { co_return 1; });
        EXPECT_STREQ(r.error().what(), "no commit");
        EXPECT_EQ(fx.s.log, (std::vector<std::string>{"BEGIN", "COMMIT"}));
    }());
}

TEST(Transaction, ThrowingBodyRollsBackRethrowsAndKeepsTheConnection) {
    fixture fx;
    drive([&]() -> coro::task<> {
        bool thrown = false;
        try {
            (void)co_await sql::transaction(fx.pool, [](auto tx) -> coro::task<outcome> {
                (void)co_await sql::try_query<"SELECT 1">(tx);
                throw std::runtime_error("boom");
                co_return 1;
            });
        } catch (const std::runtime_error& e) {
            thrown = std::string{e.what()} == "boom";
        }
        EXPECT_TRUE(thrown);
        EXPECT_EQ(fx.s.log, begin_rollback);
        EXPECT_EQ(fx.pool.idle(), 1u);
        EXPECT_EQ(fx.s.destroyed, 0);
    }());
}

TEST(Transaction, RollbackFailureAfterAThrowAbandons) {
    fixture fx;
    fx.s.runs.emplace_back(std::uint64_t{0});
    fx.s.runs.emplace_back(std::unexpected(sql::error{sql::error_kind::query, "rollback failed"}));
    drive([&]() -> coro::task<> {
        bool thrown = false;
        try {
            (void)co_await sql::transaction(fx.pool, [](auto) -> coro::task<outcome> {
                throw std::runtime_error("boom");
                co_return 1;
            });
        } catch (const std::runtime_error&) {
            thrown = true;
        }
        EXPECT_TRUE(thrown);
        EXPECT_EQ(fx.s.destroyed, 1);
        EXPECT_EQ(fx.pool.size(), 0u);
    }());
}

TEST(Transaction, VoidBody) {
    fixture fx;
    drive([&]() -> coro::task<> {
        const auto r = co_await sql::transaction(fx.pool, [](auto tx) -> coro::task<std::expected<void, sql::error>> {
            co_return (co_await sql::try_execute<"DELETE FROM t">(tx)).transform([](auto) {});
        });
        EXPECT_TRUE(r.has_value());
        EXPECT_EQ(fx.s.log, (std::vector<std::string>{"BEGIN", "DELETE FROM t", "COMMIT"}));
    }());
}

TEST(Transaction, CheckoutFailurePropagates) {
    fixture fx;
    fx.s.opens.emplace_back(std::unexpected(sql::error{sql::error_kind::connect, "refused"}));
    drive([&]() -> coro::task<> {
        const auto r = co_await sql::transaction(fx.pool, [](auto) -> coro::task<outcome> { co_return 1; });
        EXPECT_EQ(r.error().kind(), sql::error_kind::connect);
        EXPECT_TRUE(fx.s.log.empty());
    }());
}
```

`sql/tests/compile_fail/rejects_nested_transaction.cpp`:

```cpp
// transaction takes only a pool; a transaction_source is not one, so
// BEGIN inside BEGIN cannot be spelled.
#include <expected>

#include <coro/task.h>

#include <sql/pool.h>
#include <sql/transaction.h>

#include "support/fake_driver.h"

int main() {
    sql_test::fake::script s;
    const sql::pool<sql_test::fake> pool{{.connections = 1, .script_ = &s}, {}};
    auto t = sql::transaction(pool, [](auto tx) -> coro::task<std::expected<int, sql::error>> {
        auto inner = sql::transaction(tx, [](auto) -> coro::task<std::expected<int, sql::error>> { co_return 1; });
        (void)inner;
        co_return 1;
    });
    (void)t;
}
```

`sql/tests/compile_fail/rejects_transaction_body_returning_task_t.cpp`:

```cpp
// The body must return task<expected<T, error>>; task<int> fails the
// transaction_body concept.
#include <coro/task.h>

#include <sql/pool.h>
#include <sql/transaction.h>

#include "support/fake_driver.h"

int main() {
    sql_test::fake::script s;
    const sql::pool<sql_test::fake> pool{{.connections = 1, .script_ = &s}, {}};
    auto t = sql::transaction(pool, [](auto) -> coro::task<int> { co_return 1; });
    (void)t;
}
```

`sql/tests/compile_fail/rejects_transaction_body_not_a_task.cpp`:

```cpp
// A body returning a bare expected (no coroutine) fails the concept.
#include <expected>

#include <sql/pool.h>
#include <sql/transaction.h>

#include "support/fake_driver.h"

int main() {
    sql_test::fake::script s;
    const sql::pool<sql_test::fake> pool{{.connections = 1, .script_ = &s}, {}};
    auto t = sql::transaction(pool, [](auto) -> std::expected<int, sql::error> { return 1; });
    (void)t;
}
```

- [ ] **Step 2: Register and run to see it fail**

Add `include/sql/transaction.h` to the FILE_SET, `sql_add_test(test_transaction)`, the three compile-fail registrations, and `#include "sql/transaction.h"` at the end of `sql.h`'s list.

Run: `cmake --build cmake-build-debug --target sql_test_transaction`
Expected: FAIL, header not found.

- [ ] **Step 3: Write `sql/include/sql/transaction.h`**

```cpp
#pragma once

// transaction: BEGIN, the body, then COMMIT on a value or ROLLBACK on an
// error, all on one leased connection. The body sees that connection as a
// transaction_source -- a source whose checkout is the connection itself
// -- so query<> and friends run inside the transaction unchanged.
//
// Errors are data (spec requirement 8) and the body's expected is the
// contract: an error in it rolls back and comes back as-is, a rollback
// failure losing to it. The library's one catch lives here: a body that
// throws (a throwing query<> inside it, say) is rolled back and rethrown
// with the connection kept, because abandoning a connection per exception
// would turn every routine SQL error into a reconnect. Only a ROLLBACK
// that itself fails abandons the connection, so a dirty one never returns
// to the pool. co_await is illegal inside a catch handler, which is why
// the exception is captured there and the rollback runs after the try.
//
// transaction_body<F, D> is the callback contract: invocable with an
// lvalue transaction_source<D>, returning exactly task<expected<T, error>>.
// A wrong shape fails at the call site with the concept in the
// diagnostic. transaction takes only a pool, so a nested transaction is a
// compile error rather than a runtime BEGIN inside BEGIN.

#include <concepts>
#include <exception>
#include <expected>
#include <functional>
#include <optional>
#include <type_traits>
#include <utility>

#include <coro/task.h>
#include <fstr/fstr.h>

#include "sql/concepts.h"
#include "sql/error.h"
#include "sql/pool.h"

namespace sql {
    // The body's view of the connection: copyable, so `auto tx` works, with
    // a checkout that answers a non-returning lease. Its guard state lives
    // in transaction's frame, not here.
    template <driver D>
    class transaction_source final {
    public:
        using driver = D;

        explicit transaction_source(typename D::connection& conn) noexcept : conn_(&conn) {
        }

        [[nodiscard]] coro::task<std::expected<lease<D>, error>> checkout() const {
            co_return lease<D>{conn_, nullptr};
        }

    private:
        typename D::connection* conn_;
    };

    // The body's return type unwrapped to T. Left undefined for anything
    // but task<expected<T, error>>, which is what lets the concept below
    // name it as the whole shape check.
    template <typename Task>
    struct transaction_result;

    template <typename T>
    struct transaction_result<coro::task<std::expected<T, error>>> {
        using type = T;
    };

    template <typename F, typename D>
    using transaction_body_result_t = std::invoke_result_t<F&, transaction_source<D>&>;

    template <typename F, typename D>
    concept transaction_body = driver<D>
        && std::invocable<F&, transaction_source<D>&>
        && requires { typename transaction_result<transaction_body_result_t<F, D>>::type; };

    template <typename F, typename D>
    using transaction_value_t = typename transaction_result<transaction_body_result_t<F, D>>::type;

    namespace detail {
        inline constexpr fstr::fstr begin_sql{"BEGIN"};
        inline constexpr fstr::fstr commit_sql{"COMMIT"};
        inline constexpr fstr::fstr rollback_sql{"ROLLBACK"};

        // ROLLBACK, and if even that fails, make sure the connection can
        // never go back to the pool carrying an open transaction.
        template <driver D>
        coro::task<> rollback(typename D::connection& conn) {
            const auto r = co_await conn.template execute<rollback_sql>();
            if (!r) conn.abandon();
        }
    }

    template <driver D, transaction_body<D> F>
    [[nodiscard]] coro::task<std::expected<transaction_value_t<F, D>, error>>
    transaction(const pool<D>& src, F body) {
        using T = transaction_value_t<F, D>;
        using outcome_t = std::expected<T, error>;

        auto lease = co_await src.checkout();
        if (!lease) co_return std::unexpected(std::move(lease.error()));
        auto& conn = **lease;

        if (auto begun = co_await conn.template execute<detail::begin_sql>(); !begun) {
            co_return std::unexpected(std::move(begun.error()));
        }

        transaction_source<D> tx{conn};
        std::optional<outcome_t> outcome;
        std::exception_ptr thrown;
        try {
            outcome.emplace(co_await std::invoke(body, tx));
        } catch (...) {
            thrown = std::current_exception();
        }
        if (thrown) {
            co_await detail::rollback<D>(conn);
            std::rethrow_exception(thrown);
        }
        if (!*outcome) {
            co_await detail::rollback<D>(conn);
            co_return std::unexpected(std::move(outcome->error()));
        }
        if (auto committed = co_await conn.template execute<detail::commit_sql>(); !committed) {
            co_return std::unexpected(std::move(committed.error()));
        }
        if constexpr (std::is_void_v<T>) {
            co_return outcome_t{};
        } else {
            co_return std::move(**outcome);
        }
    }
}
```

- [ ] **Step 4: Run the tests**

Run: `cmake --build cmake-build-debug --target sql_test_transaction sql_headers && ctest --test-dir cmake-build-debug -R '^sql_test_transaction$|^rejects_nested_transaction$|^rejects_transaction_body' --output-on-failure`
Expected: 4 tests passed (11 cases in `sql_test_transaction`, three `rejects_*`).

- [ ] **Step 5: Commit**

```bash
git add sql/include/sql/transaction.h sql/tests/test_transaction.cpp \
        sql/tests/compile_fail/rejects_nested_transaction.cpp \
        sql/tests/compile_fail/rejects_transaction_body_returning_task_t.cpp \
        sql/tests/compile_fail/rejects_transaction_body_not_a_task.cpp \
        sql/CMakeLists.txt sql/include/sql/sql.h
git commit -m "sql: transaction with a transaction_body concept, rollback on error or throw

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_013iahp2MjjdDsXN7sxWSNyJ"
```

---

