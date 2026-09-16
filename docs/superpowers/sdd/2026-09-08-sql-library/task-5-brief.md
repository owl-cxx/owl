### Task 5: `concepts.h` and the scripted fake driver

**Files:**
- Create: `sql/include/sql/concepts.h`, `sql/tests/support/fake_driver.h`, `sql/tests/test_concepts.cpp`
- Modify: `sql/CMakeLists.txt` (FILE_SET + `sql_add_test(test_concepts)`), `sql/include/sql/sql.h` (add `#include "sql/concepts.h"` between `config`-less `concepts.h` and `convert.h` alphabetically: `concepts.h`, `convert.h`, `error.h`, `io.h`)

**Interfaces:**
- Consumes: `sql::error`, `coro::task`, `fstr::fstr`.
- Produces: `sql::detail::probe_query` (`fstr::fstr{"SELECT 1"}`); concept `sql::result_like<R>`; concept `sql::driver<D>` requiring `D::config` (with a `connections` member convertible to `unsigned`), `D::io`, `D::connection`, `D::result`, `static coro::task<std::expected<std::unique_ptr<connection>, error>> connection::open(const config&, const io&)`, `bool connection::ok() const`, `void connection::abandon() noexcept`, `template <fstr::fstr Q, bindable... Args> coro::task<std::expected<result, error>> connection::execute(const Args&...)`.
- Produces (tests only): `sql_test::fake` with `fake::script` (`opens`, `runs`, `rows`, `log`, `opened`, `destroyed`, `suspend`, `suspend_open`, `parked`, `resume_all()`), `fake::config{connections, script_}`, `fake::io{}`, `fake::result`, `fake::connection`.

- [ ] **Step 1: Write the failing test**

`sql/tests/test_concepts.cpp`:

```cpp
#include <gtest/gtest.h>

#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <tuple>

#include <coro/task.h>

#include <sql/concepts.h>
#include <sql/error.h>

#include "support/fake_driver.h"

namespace {
    struct no_abandon final {
        struct config final { unsigned connections = 1; };
        struct io final {};
        using result = sql_test::fake::result;
        class connection final {
        public:
            static coro::task<std::expected<std::unique_ptr<connection>, sql::error>> open(const config&, const io&);
            bool ok() const noexcept { return true; }
            template <fstr::fstr Q, sql::bindable... Args>
            coro::task<std::expected<result, sql::error>> execute(const Args&...);
        };
    };
}

TEST(Concepts, FakeIsADriverAndItsResultIsResultLike) {
    static_assert(sql::driver<sql_test::fake>);
    static_assert(sql::result_like<sql_test::fake::result>);
    static_assert(!sql::driver<no_abandon>);
    static_assert(!sql::driver<int>);
    static_assert(!sql::result_like<int>);
}

TEST(FakeDriver, ScriptedRowsIndexAndConvert) {
    sql_test::fake::script s;
    s.rows.push_back({{"1", "a"}, {"2", std::nullopt}});
    auto body = [&]() -> coro::task<> {
        auto c = co_await sql_test::fake::connection::open({.connections = 1, .script_ = &s}, {});
        EXPECT_TRUE(c.has_value());
        if (!c) co_return;
        const auto r = co_await (*c)->execute<"SELECT 1">();
        EXPECT_TRUE(r.has_value());
        if (!r) co_return;
        EXPECT_EQ(r->rows(), 2u);
        EXPECT_EQ(r->columns(), 2u);
        EXPECT_EQ(r->affected(), 1u);
        EXPECT_EQ(((*r)[0].as<int, std::string>()), (std::tuple<int, std::string>{1, "a"}));
        EXPECT_EQ((*r)[0]["c1"].as<std::string>(), "a");
        EXPECT_EQ((*r)[1][1].as<std::optional<int>>(), std::nullopt);
        EXPECT_TRUE((*r)[1][1].is_null());
        EXPECT_THROW((void)(*r)[1][1].as<int>(), sql::error);
        EXPECT_THROW((void)(*r)[2], sql::error);
        EXPECT_THROW((void)(*r)[0][5], sql::error);
        EXPECT_THROW((void)(*r)[0]["nope"], sql::error);
        int seen = 0;
        for (const auto row : *r) seen += row[0].as<int>();
        EXPECT_EQ(seen, 3);
        EXPECT_EQ(s.log, (std::vector<std::string>{"SELECT 1"}));
    };
    auto t = body();
    t.start();
    EXPECT_TRUE(t.done());
}

TEST(FakeDriver, SuspendParksUntilResumed) {
    sql_test::fake::script s;
    s.suspend = true;
    bool finished = false;
    auto body = [&]() -> coro::task<> {
        auto c = co_await sql_test::fake::connection::open({.connections = 1, .script_ = &s}, {});
        (void)co_await (*c)->execute<"SELECT 2">();
        finished = true;
    };
    auto t = body();
    t.start();
    EXPECT_FALSE(finished);
    EXPECT_EQ(s.parked.size(), 1u);
    s.resume_all();
    EXPECT_TRUE(finished);
    EXPECT_TRUE(t.done());
}

TEST(FakeDriver, ScriptedFailuresAndBrokenConnections) {
    sql_test::fake::script s;
    s.runs.emplace_back(std::unexpected(sql::error{sql::error_kind::query, "bad"}));
    s.runs.emplace_back(std::unexpected(sql::error{sql::error_kind::connection, "gone"}));
    auto body = [&]() -> coro::task<> {
        auto c = co_await sql_test::fake::connection::open({.connections = 1, .script_ = &s}, {});
        EXPECT_EQ((co_await (*c)->execute<"A">()).error().kind(), sql::error_kind::query);
        EXPECT_TRUE((*c)->ok());
        EXPECT_EQ((co_await (*c)->execute<"B">()).error().kind(), sql::error_kind::connection);
        EXPECT_FALSE((*c)->ok());
    };
    auto t = body();
    t.start();
    EXPECT_TRUE(t.done());
    EXPECT_EQ(s.destroyed, 1);
}
```

- [ ] **Step 2: Register and run to see it fail**

Add `include/sql/concepts.h` to the FILE_SET, `sql_add_test(test_concepts)`, and the umbrella include.

Run: `cmake --build cmake-build-debug --target sql_test_concepts`
Expected: FAIL, header not found.

- [ ] **Step 3: Write `sql/include/sql/concepts.h`**

```cpp
#pragma once

// What a driver must provide, and what its result must look like, with
// none of how. pool.h, query.h and transaction.h are written against
// these two concepts alone, so a driver is anything that satisfies them
// -- the scripted fake in the tests included -- and generic handler code
// can constrain on result_like without naming a driver.

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>

#include <coro/task.h>
#include <fstr/fstr.h>

#include "sql/error.h"

namespace sql {
    namespace detail {
        // The literal the driver concept probes execute<> with. Named
        // rather than spelled inline so the requires-expression stays a
        // plain template-argument list.
        inline constexpr fstr::fstr probe_query{"SELECT 1"};
    }

    // The shape both drivers' results share. Cells convert through as<T>();
    // there is no text() because a non-text sqlite cell has no honest one.
    template <typename R>
    concept result_like = requires(const R& r, const std::size_t i, const std::string_view name) {
        { r.rows() } -> std::convertible_to<std::size_t>;
        { r.columns() } -> std::convertible_to<std::size_t>;
        { r.affected() } -> std::convertible_to<std::uint64_t>;
        { r[i][i].is_null() } -> std::same_as<bool>;
        { r[i][name].is_null() } -> std::same_as<bool>;
        { r[i][i].template as<int>() } -> std::same_as<int>;
        { r[i][i].template as<std::optional<int>>() } -> std::same_as<std::optional<int>>;
        { r[i].template as<int>() } -> std::same_as<int>;
        { r[i].template as<int, std::string>() } -> std::same_as<std::tuple<int, std::string>>;
        { r.begin() };
        { r.end() };
    };

    // A driver: a config with a connection count, the io handle it waits
    // through, a connection that opens asynchronously and runs a literal,
    // and the result that comes back. ok() is false once the link is
    // broken; abandon() forces that, for a transaction whose ROLLBACK
    // failed. Every failure is data: open and execute answer expected.
    template <typename D>
    concept driver = requires {
        typename D::config;
        typename D::io;
        typename D::connection;
        typename D::result;
    } && result_like<typename D::result>
      && std::convertible_to<decltype(std::declval<const typename D::config&>().connections), unsigned>
      && requires(typename D::connection& c, const typename D::config& cfg, const typename D::io& io) {
        { D::connection::open(cfg, io) }
            -> std::same_as<coro::task<std::expected<std::unique_ptr<typename D::connection>, error>>>;
        { c.ok() } -> std::same_as<bool>;
        { c.abandon() } noexcept;
        { c.template execute<detail::probe_query>() }
            -> std::same_as<coro::task<std::expected<typename D::result, error>>>;
    };
}
```

- [ ] **Step 4: Write `sql/tests/support/fake_driver.h`**

```cpp
#pragma once

// A driver whose behaviour is a script: what open() answers, what execute()
// answers, which rows come back, and whether a call parks until the test
// resumes it. Enough to drive pool, query and transaction without a
// database, on one thread, deterministically.

#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include <coro/task.h>
#include <fstr/fstr.h>

#include <sql/concepts.h>
#include <sql/convert.h>
#include <sql/detail/stmt_key.h>
#include <sql/error.h>

namespace sql_test {
    struct fake final {
        using cells = std::vector<std::optional<std::string>>;

        struct script final {
            std::deque<std::expected<void, sql::error>> opens;
            std::deque<std::expected<std::uint64_t, sql::error>> runs;
            std::deque<std::vector<cells>> rows;
            std::vector<std::string> log;
            int opened = 0;
            int destroyed = 0;
            bool suspend = false;
            bool suspend_open = false;
            std::vector<std::coroutine_handle<>> parked;

            void resume_all() {
                auto handles = std::move(parked);
                parked.clear();
                for (const auto h : handles) h.resume();
            }
        };

        struct config final {
            unsigned connections = 1;
            script* script_ = nullptr;
        };

        struct io final {
        };

        class cell final {
        public:
            explicit cell(const std::optional<std::string>* const v) noexcept : v_(v) {
            }

            [[nodiscard]] bool is_null() const noexcept {
                return !v_->has_value();
            }

            template <typename T>
            [[nodiscard]] T as() const {
                if constexpr (sql::detail::is_optional_v<T>) {
                    if (is_null()) return std::nullopt;
                    return T{as<typename T::value_type>()};
                } else {
                    if (is_null()) throw sql::error{sql::error_kind::conversion, "NULL into a non-optional"};
                    auto r = sql::from_text<T>(**v_);
                    if (!r) throw std::move(r.error());
                    return std::move(*r);
                }
            }

        private:
            const std::optional<std::string>* v_;
        };

        class result;

        class row final {
        public:
            row(const result* const r, const std::size_t i) noexcept : r_(r), i_(i) {
            }

            [[nodiscard]] cell operator[](std::size_t c) const;
            [[nodiscard]] cell operator[](std::string_view name) const;

            template <typename... Ts>
            [[nodiscard]] auto as() const {
                if (width() != sizeof...(Ts)) {
                    throw sql::error{sql::error_kind::conversion, "column count does not match the requested types"};
                }
                if constexpr (sizeof...(Ts) == 1) {
                    return (*this)[0].template as<Ts...>();
                } else {
                    return as_tuple<Ts...>(std::index_sequence_for<Ts...>{});
                }
            }

        private:
            [[nodiscard]] std::size_t width() const noexcept;

            template <typename... Ts, std::size_t... Is>
            std::tuple<Ts...> as_tuple(std::index_sequence<Is...>) const {
                return std::tuple<Ts...>{(*this)[Is].template as<Ts>()...};
            }

            const result* r_;
            std::size_t i_;
        };

        class result final {
        public:
            result() = default;

            result(std::vector<cells> rows, const std::uint64_t affected) : rows_(std::move(rows)), affected_(affected) {
                const std::size_t width = rows_.empty() ? 0 : rows_.front().size();
                for (std::size_t i = 0; i < width; ++i) names_.push_back("c" + std::to_string(i));
            }

            [[nodiscard]] std::size_t rows() const noexcept {
                return rows_.size();
            }

            [[nodiscard]] std::size_t columns() const noexcept {
                return names_.size();
            }

            [[nodiscard]] std::uint64_t affected() const noexcept {
                return affected_;
            }

            [[nodiscard]] row operator[](const std::size_t i) const {
                if (i >= rows_.size()) throw sql::error{sql::error_kind::conversion, "row index out of range"};
                return row{this, i};
            }

            struct iterator final {
                const result* r;
                std::size_t i;

                row operator*() const {
                    return row{r, i};
                }

                iterator& operator++() {
                    ++i;
                    return *this;
                }

                bool operator==(const iterator& o) const {
                    return i == o.i;
                }
            };

            [[nodiscard]] iterator begin() const noexcept {
                return {this, 0};
            }

            [[nodiscard]] iterator end() const noexcept {
                return {this, rows_.size()};
            }

            [[nodiscard]] const cells& at(const std::size_t i) const {
                return rows_[i];
            }

            [[nodiscard]] const std::vector<std::string>& names() const noexcept {
                return names_;
            }

        private:
            std::vector<cells> rows_;
            std::vector<std::string> names_;
            std::uint64_t affected_ = 0;
        };

        class connection final {
        public:
            explicit connection(script& s) noexcept : s_(&s) {
            }

            ~connection() {
                ++s_->destroyed;
            }

            static coro::task<std::expected<std::unique_ptr<connection>, sql::error>>
            open(const config& cfg, const io&) {
                script& s = *cfg.script_;
                ++s.opened;
                co_await park{&s, s.suspend_open};
                if (!s.opens.empty()) {
                    auto verdict = std::move(s.opens.front());
                    s.opens.pop_front();
                    if (!verdict) co_return std::unexpected(std::move(verdict.error()));
                }
                co_return std::make_unique<connection>(s);
            }

            [[nodiscard]] bool ok() const noexcept {
                return ok_;
            }

            void abandon() noexcept {
                ok_ = false;
            }

            template <fstr::fstr Q, sql::bindable... Args>
            [[nodiscard]] coro::task<std::expected<result, sql::error>> execute(const Args&...) {
                s_->log.emplace_back(sql::detail::stmt_key<Q>::text);
                co_await park{s_, s_->suspend};
                std::uint64_t affected = 1;
                if (!s_->runs.empty()) {
                    auto verdict = std::move(s_->runs.front());
                    s_->runs.pop_front();
                    if (!verdict) {
                        if (verdict.error().kind() == sql::error_kind::connection) ok_ = false;
                        co_return std::unexpected(std::move(verdict.error()));
                    }
                    affected = *verdict;
                }
                std::vector<cells> rows;
                if (!s_->rows.empty()) {
                    rows = std::move(s_->rows.front());
                    s_->rows.pop_front();
                }
                co_return result{std::move(rows), affected};
            }

        private:
            struct park final {
                script* s;
                bool should_park;

                bool await_ready() const noexcept {
                    return !should_park;
                }

                void await_suspend(const std::coroutine_handle<> h) const {
                    s->parked.push_back(h);
                }

                void await_resume() const noexcept {
                }
            };

            script* s_;
            bool ok_ = true;
        };
    };

    inline std::size_t fake::row::width() const noexcept {
        return r_->columns();
    }

    inline fake::cell fake::row::operator[](const std::size_t c) const {
        const auto& cs = r_->at(i_);
        if (c >= cs.size()) throw sql::error{sql::error_kind::conversion, "column index out of range"};
        return cell{&cs[c]};
    }

    inline fake::cell fake::row::operator[](const std::string_view name) const {
        const auto& n = r_->names();
        for (std::size_t i = 0; i < n.size(); ++i) {
            if (n[i] == name) return (*this)[i];
        }
        throw sql::error{sql::error_kind::conversion, "no such column: " + std::string{name}};
    }

    static_assert(sql::driver<fake>);
}
```

- [ ] **Step 5: Run the test**

Run: `cmake --build cmake-build-debug --target sql_test_concepts sql_headers && ctest --test-dir cmake-build-debug -R '^sql_test_concepts$' --output-on-failure`
Expected: PASS, 4 tests.

- [ ] **Step 6: Commit**

```bash
git add sql/include/sql/concepts.h sql/tests/support/fake_driver.h sql/tests/test_concepts.cpp sql/CMakeLists.txt sql/include/sql/sql.h
git commit -m "sql: driver and result_like concepts, scripted fake driver for tests

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_013iahp2MjjdDsXN7sxWSNyJ"
```

---

