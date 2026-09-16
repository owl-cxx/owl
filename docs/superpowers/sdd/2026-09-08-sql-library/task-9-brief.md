### Task 9: `sqlite.h` — the SQLite driver

**Files:**
- Create: `sql/include/sql/sqlite.h`, `sql/tests/sqlite/test_sqlite.cpp`
- Modify: `sql/CMakeLists.txt` (FILE_SET; `if (OWL_ENABLE_SQLITE) sql_add_test(sqlite/test_sqlite) endif ()`), `sql/include/sql/sql.h` (add the guarded include)

**Interfaces:**
- Consumes: `sql::scheduler_ref`, `sql::bindable`, `sql::from_text`, `sql::detail::stmt_key`, `sql::error`, `coro::static_thread_pool`.
- Produces: `struct sql::sqlite final` with `config{std::string path; unsigned connections = 1; std::chrono::milliseconds busy_timeout{5000};}`, `using io = scheduler_ref`, `null_t`, `value` (variant), `cell`, `row`, `result`, `connection` (with `open`, `ok`, `abandon`, `execute<Q>`, and `std::size_t cached_statements() const`). `static_assert(sql::driver<sql::sqlite>)` at the end of the header.

- [ ] **Step 1: Add the guarded umbrella include and the FILE_SET entry**

In `sql.h`, after the plain includes:

```cpp
#ifdef OWL_ENABLE_SQLITE
#include "sql/sqlite.h"
#endif
```

In `sql/CMakeLists.txt`, add `include/sql/sqlite.h` to the FILE_SET and, inside `if (BUILD_TESTING)` after the other `sql_add_test` lines:

```cmake
    if (OWL_ENABLE_SQLITE)
        sql_add_test(sqlite/test_sqlite)
    endif ()
```

- [ ] **Step 2: Write the failing tests**

`sql/tests/sqlite/test_sqlite.cpp`:

```cpp
#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <thread>
#include <tuple>
#include <unistd.h>

#include <sqlite3.h>

#include <coro/executors/static_thread_pool.h>
#include <coro/run/sync_wait.h>
#include <coro/task.h>

#include <sql/error.h>
#include <sql/pool.h>
#include <sql/query.h>
#include <sql/sqlite.h>
#include <sql/transaction.h>

using namespace std::chrono_literals;

namespace {
    struct temp_db final {
        std::filesystem::path path;

        explicit temp_db(const std::string& name)
            : path(std::filesystem::temp_directory_path() / ("owl_sql_" + std::to_string(::getpid()) + "_" + name + ".db")) {
            std::filesystem::remove(path);
        }

        ~temp_db() {
            std::filesystem::remove(path);
            std::filesystem::remove(path.string() + "-journal");
            std::filesystem::remove(path.string() + "-wal");
            std::filesystem::remove(path.string() + "-shm");
        }
    };

    struct fixture final {
        temp_db file;
        coro::static_thread_pool hop{1};
        sql::scheduler_ref io{hop};
        sql::pool<sql::sqlite> db;

        explicit fixture(const std::string& name, const unsigned connections = 1)
            : file(name), db({.path = file.path.string(), .connections = connections}, io) {
        }
    };

    coro::task<> create_t(const sql::pool<sql::sqlite>& db) {
        (void)co_await sql::execute<"CREATE TABLE t(i INTEGER, f REAL, s TEXT, b BLOB, n)">(db);
    }
}

TEST(Sqlite, IsADriver) {
    static_assert(sql::driver<sql::sqlite>);
    static_assert(sql::result_like<sql::sqlite::result>);
}

TEST(Sqlite, RoundTripsEveryStorageClass) {
    fixture fx{"roundtrip"};
    const sql::blob bytes{std::byte{0}, std::byte{0xab}};
    coro::sync_wait([&]() -> coro::task<> {
        co_await create_t(fx.db);
        const auto n = co_await sql::execute<"INSERT INTO t VALUES ($1, $2, $3, $4, $5)">(
            fx.db, std::int64_t{-5}, 2.5, std::string{"hi"}, bytes, std::nullopt);
        EXPECT_EQ(n, 1u);
        const auto r = co_await sql::query<"SELECT i, f, s, b, n FROM t">(fx.db);
        EXPECT_EQ(r.rows(), 1u);
        EXPECT_EQ(r.columns(), 5u);
        EXPECT_EQ(r.affected(), 0u);
        EXPECT_EQ(r[0][0].as<std::int64_t>(), -5);
        EXPECT_DOUBLE_EQ(r[0][1].as<double>(), 2.5);
        EXPECT_EQ(r[0][2].as<std::string>(), "hi");
        EXPECT_EQ(r[0][2].as<std::string_view>(), "hi");
        EXPECT_EQ(r[0][3].as<sql::blob>(), bytes);
        EXPECT_TRUE(r[0][4].is_null());
        EXPECT_EQ(r[0][4].as<std::optional<int>>(), std::nullopt);
        EXPECT_EQ(r[0]["s"].as<std::string>(), "hi");
        EXPECT_EQ((r[0].as<std::int64_t, double, std::string, sql::blob, std::optional<int>>()),
                  (std::tuple<std::int64_t, double, std::string, sql::blob, std::optional<int>>{-5, 2.5, "hi", bytes, std::nullopt}));
    }());
}

TEST(Sqlite, StorageClassesArePreservedAndConversionsAreStrict) {
    fixture fx{"classes"};
    coro::sync_wait([&]() -> coro::task<> {
        co_await create_t(fx.db);
        (void)co_await sql::execute<"INSERT INTO t(i, f, s, b, n) VALUES (300, 1.5, '42', x'00', 1)">(fx.db);
        const auto r = co_await sql::query<"SELECT i, f, s, b, n FROM t">(fx.db);
        EXPECT_EQ(r[0][0].as<int>(), 300);
        EXPECT_DOUBLE_EQ(r[0][0].as<double>(), 300.0);
        EXPECT_TRUE(r[0][0].as<bool>());
        EXPECT_THROW((void)r[0][0].as<std::int8_t>(), sql::error);
        EXPECT_THROW((void)r[0][0].as<std::string>(), sql::error);
        EXPECT_EQ(r[0][2].as<int>(), 42);
        EXPECT_EQ(r[0][2].as<std::string>(), "42");
        EXPECT_THROW((void)r[0][1].as<int>(), sql::error);
        EXPECT_THROW((void)r[0][3].as<std::string>(), sql::error);
        EXPECT_TRUE(r[0][4].as<bool>());
        EXPECT_THROW((void)r[0][4].as<std::string>(), sql::error);
    }());
}

TEST(Sqlite, PlaceholderNumberWinsOverAppearanceOrder) {
    fixture fx{"order"};
    coro::sync_wait([&]() -> coro::task<> {
        const auto r = co_await sql::query<"SELECT $2, $1">(fx.db, 1, 2);
        EXPECT_EQ((r[0].as<int, int>()), (std::tuple<int, int>{2, 1}));
    }());
}

TEST(Sqlite, StatementsAreCachedPerLiteral) {
    fixture fx{"cache"};
    coro::sync_wait([&]() -> coro::task<> {
        (void)co_await sql::query<"SELECT 1">(fx.db);
        (void)co_await sql::query<"SELECT 1">(fx.db);
        (void)co_await sql::query<"SELECT 2">(fx.db);
        const auto l = co_await fx.db.checkout();
        EXPECT_EQ((*l)->cached_statements(), 2u);
    }());
}

TEST(Sqlite, QueryErrorsCarryTheCodeAndKeepTheConnection) {
    fixture fx{"errors"};
    coro::sync_wait([&]() -> coro::task<> {
        const auto r = co_await sql::try_query<"SELECT * FROM nope">(fx.db);
        EXPECT_EQ(r.error().kind(), sql::error_kind::query);
        EXPECT_EQ(r.error().code(), SQLITE_ERROR);
        EXPECT_NE(std::string{r.error().what()}.find("nope"), std::string::npos);
        EXPECT_EQ(fx.db.size(), 1u);
        EXPECT_EQ(fx.db.idle(), 1u);
    }());
}

TEST(Sqlite, OpenFailureIsConnect) {
    coro::static_thread_pool hop{1};
    sql::scheduler_ref io{hop};
    sql::pool<sql::sqlite> db{{.path = "/nonexistent-dir-owl/x.db"}, io};
    coro::sync_wait([&]() -> coro::task<> {
        const auto r = co_await sql::try_query<"SELECT 1">(db);
        EXPECT_EQ(r.error().kind(), sql::error_kind::connect);
        EXPECT_EQ(r.error().code() & 0xff, SQLITE_CANTOPEN);
        EXPECT_EQ(db.size(), 0u);
    }());
}

TEST(Sqlite, BusyTimeoutSurfacesAsSqliteBusy) {
    fixture writer{"busy"};
    coro::static_thread_pool hop{1};
    sql::scheduler_ref io{hop};
    sql::pool<sql::sqlite> reader{{.path = writer.file.path.string(), .busy_timeout = 100ms}, io};
    coro::sync_wait([&]() -> coro::task<> {
        co_await create_t(writer.db);
        const auto held = co_await writer.db.checkout();
        EXPECT_TRUE((co_await (*held)->execute<"BEGIN IMMEDIATE">()).has_value());
        const auto r = co_await sql::try_execute<"INSERT INTO t(i) VALUES (1)">(reader);
        EXPECT_EQ(r.error().kind(), sql::error_kind::query);
        EXPECT_EQ(r.error().code() & 0xff, SQLITE_BUSY);
        EXPECT_TRUE((co_await (*held)->execute<"ROLLBACK">()).has_value());
    }());
}

TEST(Sqlite, AffectedIsZeroForReadOnlyStatements) {
    fixture fx{"affected"};
    coro::sync_wait([&]() -> coro::task<> {
        co_await create_t(fx.db);
        EXPECT_EQ(co_await sql::execute<"INSERT INTO t(i) VALUES (1), (2)">(fx.db), 2u);
        EXPECT_EQ(co_await sql::execute<"SELECT i FROM t">(fx.db), 0u);
        EXPECT_EQ(co_await sql::execute<"DELETE FROM t WHERE i = $1">(fx.db, 1), 1u);
    }());
}

TEST(Sqlite, RowAsTupleRequiresTheExactWidth) {
    fixture fx{"width"};
    coro::sync_wait([&]() -> coro::task<> {
        const auto r = co_await sql::query<"SELECT 1, 2">(fx.db);
        EXPECT_THROW((void)r[0].as<int>(), sql::error);
        EXPECT_THROW((void)(r[0].as<int, int, int>()), sql::error);
        EXPECT_THROW((void)r[1], sql::error);
        EXPECT_THROW((void)r[0][2], sql::error);
        EXPECT_THROW((void)r[0]["zzz"], sql::error);
        int seen = 0;
        for (const auto row : r) seen += row[1].as<int>();
        EXPECT_EQ(seen, 2);
    }());
}

TEST(Sqlite, CompletionsResumeOnTheScheduler) {
    fixture fx{"resume"};
    coro::sync_wait([&]() -> coro::task<> {
        co_await fx.io.schedule();
        const auto hop_id = std::this_thread::get_id();
        (void)co_await sql::try_query<"SELECT 1">(fx.db);
        EXPECT_EQ(std::this_thread::get_id(), hop_id);
    }());
}

TEST(Sqlite, TransactionsCommitAndRollBack) {
    fixture fx{"tx"};
    coro::sync_wait([&]() -> coro::task<> {
        co_await create_t(fx.db);
        const auto rolled = co_await sql::transaction(fx.db, [](auto tx) -> coro::task<std::expected<int, sql::error>> {
            (void)co_await sql::execute<"INSERT INTO t(i) VALUES (1)">(tx);
            co_return std::unexpected(sql::error{sql::error_kind::query, "changed my mind"});
        });
        EXPECT_FALSE(rolled.has_value());
        const auto committed = co_await sql::transaction(fx.db, [](auto tx) -> coro::task<std::expected<int, sql::error>> {
            (void)co_await sql::execute<"INSERT INTO t(i) VALUES (2)">(tx);
            const auto r = co_await sql::query<"SELECT count(*) FROM t">(tx);
            co_return r[0][0].template as<int>();
        });
        EXPECT_EQ(committed.value(), 1);
        const auto r = co_await sql::query<"SELECT i FROM t">(fx.db);
        EXPECT_EQ(r.rows(), 1u);
        EXPECT_EQ(r[0][0].as<int>(), 2);
    }());
}
```

- [ ] **Step 3: Run to see it fail**

Run: `cmake --build cmake-build-debug --target sql_sqlite_test_sqlite`
Expected: FAIL, `sql/sqlite.h` not found (the configure guard will also complain that `sqlite.h` is in the FILE_SET but not on disk — create the header next).

- [ ] **Step 4: Write `sql/include/sql/sqlite.h`**

```cpp
#pragma once

// sql::sqlite -- the SQLite driver. sqlite3_* calls are synchronous file
// I/O, so each pooled connection is pinned to its own thread: execute hops
// there, runs prepare / bind / step to completion, copies the rows out,
// and hops back through the scheduler_ref the pool was built with. The
// loop never blocks; the price is that rows are materialized on that
// thread -- in sqlite's own storage classes (INTEGER, FLOAT, TEXT, BLOB,
// NULL) rather than flattened to text, because sqlite already hands over
// a typed value and a text round trip would be work for nothing.
//
// The on-thread section is one noexcept member writing an expected. That
// is what guarantees the coroutine frame never completes off-thread:
// nothing on that path throws except allocation, and allocation failure
// terminates, as it would on the loop.
//
// Placeholders are $1..$N, shared with psql. sqlite numbers them by order
// of first appearance -- `select $2, $1` gives $1 index 2 -- so argument k
// is bound through sqlite3_bind_parameter_index("$k"), never by position.
// Statements are cached per connection, keyed by the literal's identity.
// One statement per literal: sqlite3_prepare_v2 stops at the first
// semicolon and the tail is ignored.

#include <chrono>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

#include <sqlite3.h>

#include <coro/executors/static_thread_pool.h>
#include <coro/task.h>
#include <fstr/fstr.h>

#include "sql/concepts.h"
#include "sql/convert.h"
#include "sql/detail/stmt_key.h"
#include "sql/error.h"
#include "sql/io.h"

namespace sql {
    struct sqlite final {
        struct config final {
            std::string path;
            unsigned connections = 1;
            std::chrono::milliseconds busy_timeout{5000};
        };

        using io = scheduler_ref;

        struct null_t final {
            bool operator==(const null_t&) const noexcept = default;
        };

        using value = std::variant<null_t, std::int64_t, double, std::string, blob>;

        class cell final {
        public:
            explicit cell(const value* const v) noexcept : v_(v) {
            }

            [[nodiscard]] bool is_null() const noexcept {
                return std::holds_alternative<null_t>(*v_);
            }

            // Direct when the storage classes match; from_text for TEXT into
            // a number or bool, because a column without affinity can hold
            // '42'; a conversion error for every other pairing, rather than
            // a guess.
            template <typename T>
            [[nodiscard]] T as() const {
                if constexpr (detail::is_optional_v<T>) {
                    if (is_null()) return std::nullopt;
                    return T{as<typename T::value_type>()};
                } else {
                    return std::visit([](const auto& held) -> T {
                        return convert<T>(held);
                    }, *v_);
                }
            }

        private:
            template <typename T>
            static T convert(const null_t&) {
                throw error{error_kind::conversion, "NULL into a non-optional type"};
            }

            template <typename T>
            static T convert(const std::int64_t held) {
                if constexpr (std::same_as<T, bool>) {
                    return held != 0;
                } else if constexpr (detail::sql_integral<T>) {
                    if (!std::in_range<T>(held)) throw error{error_kind::conversion, "INTEGER out of range for the requested type"};
                    return static_cast<T>(held);
                } else if constexpr (std::floating_point<T>) {
                    return static_cast<T>(held);
                } else {
                    throw error{error_kind::conversion, "INTEGER cannot convert to the requested type"};
                }
            }

            template <typename T>
            static T convert(const double held) {
                if constexpr (std::floating_point<T>) {
                    return static_cast<T>(held);
                } else {
                    throw error{error_kind::conversion, "FLOAT cannot convert to the requested type"};
                }
            }

            template <typename T>
            static T convert(const std::string& held) {
                if constexpr (std::same_as<T, std::string>) {
                    return held;
                } else if constexpr (std::same_as<T, std::string_view>) {
                    return std::string_view{held};
                } else if constexpr (std::same_as<T, bool> || detail::sql_integral<T> || std::floating_point<T>) {
                    auto r = from_text<T>(held);
                    if (!r) throw std::move(r.error());
                    return std::move(*r);
                } else {
                    throw error{error_kind::conversion, "TEXT cannot convert to the requested type"};
                }
            }

            template <typename T>
            static T convert(const blob& held) {
                if constexpr (std::same_as<T, blob>) {
                    return held;
                } else {
                    throw error{error_kind::conversion, "BLOB cannot convert to the requested type"};
                }
            }

            const value* v_;
        };

        class result;

        class row final {
        public:
            row(const result* const r, const std::size_t i) noexcept : r_(r), i_(i) {
            }

            [[nodiscard]] cell operator[](std::size_t c) const;
            [[nodiscard]] cell operator[](std::string_view name) const;

            // Exactly the row's width: a tuple narrower than the row is a
            // mistake this cannot tell from the intended one, so it refuses.
            template <typename... Ts>
            [[nodiscard]] auto as() const {
                if (width() != sizeof...(Ts)) {
                    throw error{error_kind::conversion, "column count does not match the requested types"};
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
                if (i >= rows_.size()) throw error{error_kind::conversion, "row index out of range"};
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

        private:
            friend class connection;
            friend class row;

            std::vector<std::string> names_;
            std::vector<std::vector<value>> rows_;
            std::uint64_t affected_ = 0;
        };

        class connection final {
        public:
            static coro::task<std::expected<std::unique_ptr<connection>, error>>
            open(const config& cfg, const io& io) {
                auto c = std::unique_ptr<connection>(new connection{io});
                co_await c->thread_.schedule();
                auto opened = c->open_on_thread(cfg);
                co_await io.schedule();
                if (!opened) co_return std::unexpected(std::move(opened.error()));
                co_return std::move(c);
            }

            // Runs on the loop after every query has hopped back, so the
            // thread is idle: sequential use from another thread is what
            // SQLITE_OPEN_NOMUTEX permits.
            ~connection() {
                for (const auto& [id, stmt] : stmts_) sqlite3_finalize(stmt);
                if (db_ != nullptr) sqlite3_close(db_);
            }

            connection(const connection&) = delete;
            connection& operator=(const connection&) = delete;

            [[nodiscard]] bool ok() const noexcept {
                return ok_;
            }

            void abandon() noexcept {
                ok_ = false;
            }

            [[nodiscard]] std::size_t cached_statements() const noexcept {
                return stmts_.size();
            }

            template <fstr::fstr Q, bindable... Args>
            [[nodiscard]] coro::task<std::expected<result, error>> execute(const Args&... args) {
                co_await thread_.schedule();
                auto out = run_on_thread<Q>(args...);
                co_await io_.schedule();
                co_return out;
            }

        private:
            explicit connection(const io& io) noexcept : io_(io), thread_(1) {
            }

            [[nodiscard]] std::expected<void, error> open_on_thread(const config& cfg) noexcept {
                const int rc = sqlite3_open_v2(cfg.path.c_str(), &db_,
                                               SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_NOMUTEX, nullptr);
                if (rc != SQLITE_OK) {
                    error e{error_kind::connect, db_ != nullptr ? sqlite3_errmsg(db_) : sqlite3_errstr(rc), {}, rc};
                    if (db_ != nullptr) {
                        sqlite3_close(db_);
                        db_ = nullptr;
                    }
                    return std::unexpected(std::move(e));
                }
                sqlite3_busy_timeout(db_, static_cast<int>(cfg.busy_timeout.count()));
                sqlite3_extended_result_codes(db_, 1);
                return {};
            }

            template <fstr::fstr Q, bindable... Args>
            [[nodiscard]] std::expected<result, error> run_on_thread(const Args&... args) noexcept {
                sqlite3_stmt* const stmt = prepared<Q>();
                if (stmt == nullptr) return std::unexpected(failure(sqlite3_extended_errcode(db_)));

                [[maybe_unused]] int k = 0;
                int rc = SQLITE_OK;
                ((rc = rc == SQLITE_OK ? bind_one(stmt, ++k, args) : rc), ...);
                if (rc != SQLITE_OK) {
                    error e = failure(rc);
                    sqlite3_reset(stmt);
                    sqlite3_clear_bindings(stmt);
                    return std::unexpected(std::move(e));
                }

                result out;
                const int ncol = sqlite3_column_count(stmt);
                out.names_.reserve(static_cast<std::size_t>(ncol));
                for (int i = 0; i < ncol; ++i) out.names_.emplace_back(sqlite3_column_name(stmt, i));
                for (;;) {
                    rc = sqlite3_step(stmt);
                    if (rc == SQLITE_ROW) {
                        out.rows_.push_back(read_row(stmt, ncol));
                        continue;
                    }
                    if (rc == SQLITE_DONE) break;
                    error e = failure(rc);
                    sqlite3_reset(stmt);
                    sqlite3_clear_bindings(stmt);
                    return std::unexpected(std::move(e));
                }
                if (!sqlite3_stmt_readonly(stmt)) out.affected_ = static_cast<std::uint64_t>(sqlite3_changes64(db_));
                sqlite3_reset(stmt);
                sqlite3_clear_bindings(stmt);
                return out;
            }

            template <fstr::fstr Q>
            [[nodiscard]] sqlite3_stmt* prepared() noexcept {
                const auto it = stmts_.find(detail::stmt_key<Q>::id);
                if (it != stmts_.end()) return it->second;
                sqlite3_stmt* stmt = nullptr;
                const int rc = sqlite3_prepare_v2(db_, detail::stmt_key<Q>::c_str,
                                                  static_cast<int>(detail::stmt_key<Q>::text.size()), &stmt, nullptr);
                if (rc != SQLITE_OK || stmt == nullptr) return nullptr;
                stmts_.emplace(detail::stmt_key<Q>::id, stmt);
                return stmt;
            }

            template <bindable T>
            [[nodiscard]] static int bind_one(sqlite3_stmt* const stmt, const int k, const T& v) noexcept {
                char name[8];
                std::snprintf(name, sizeof name, "$%d", k);
                const int idx = sqlite3_bind_parameter_index(stmt, name);
                if (idx == 0) return SQLITE_RANGE;
                return bind_at(stmt, idx, v);
            }

            // SQLITE_STATIC everywhere: the argument outlives the step loop
            // because the caller's frame is suspended across the hop.
            template <bindable T>
            [[nodiscard]] static int bind_at(sqlite3_stmt* const stmt, const int idx, const T& v) noexcept {
                using U = std::remove_cvref_t<T>;
                if constexpr (std::same_as<U, std::nullopt_t>) {
                    return sqlite3_bind_null(stmt, idx);
                } else if constexpr (detail::is_optional_v<U>) {
                    if (!v) return sqlite3_bind_null(stmt, idx);
                    return bind_at(stmt, idx, *v);
                } else if constexpr (std::same_as<U, bool>) {
                    return sqlite3_bind_int(stmt, idx, v ? 1 : 0);
                } else if constexpr (detail::sql_integral<U>) {
                    return sqlite3_bind_int64(stmt, idx, static_cast<sqlite3_int64>(v));
                } else if constexpr (std::floating_point<U>) {
                    return sqlite3_bind_double(stmt, idx, static_cast<double>(v));
                } else if constexpr (detail::text_like<U>) {
                    const std::string_view s = detail::text_of(v);
                    return sqlite3_bind_text64(stmt, idx, s.data(), s.size(), SQLITE_STATIC, SQLITE_UTF8);
                } else {
                    const blob_view b{v};
                    // A null pointer binds NULL, so an empty blob must be spelled
                    // as a zero-length zeroblob.
                    if (b.empty()) return sqlite3_bind_zeroblob(stmt, idx, 0);
                    return sqlite3_bind_blob64(stmt, idx, b.data(), b.size(), SQLITE_STATIC);
                }
            }

            [[nodiscard]] static std::vector<value> read_row(sqlite3_stmt* const stmt, const int ncol) {
                std::vector<value> cells;
                cells.reserve(static_cast<std::size_t>(ncol));
                for (int i = 0; i < ncol; ++i) {
                    switch (sqlite3_column_type(stmt, i)) {
                        case SQLITE_INTEGER:
                            cells.emplace_back(std::in_place_type<std::int64_t>, static_cast<std::int64_t>(sqlite3_column_int64(stmt, i)));
                            break;
                        case SQLITE_FLOAT:
                            cells.emplace_back(std::in_place_type<double>, sqlite3_column_double(stmt, i));
                            break;
                        case SQLITE_TEXT: {
                            const auto* const t = reinterpret_cast<const char*>(sqlite3_column_text(stmt, i));
                            const auto n = static_cast<std::size_t>(sqlite3_column_bytes(stmt, i));
                            cells.emplace_back(std::in_place_type<std::string>, t, n);
                            break;
                        }
                        case SQLITE_BLOB: {
                            const auto* const b = static_cast<const std::byte*>(sqlite3_column_blob(stmt, i));
                            const auto n = static_cast<std::size_t>(sqlite3_column_bytes(stmt, i));
                            cells.emplace_back(std::in_place_type<blob>, b, b + n);
                            break;
                        }
                        default:
                            cells.emplace_back(std::in_place_type<null_t>);
                            break;
                    }
                }
                return cells;
            }

            // The families that mean the handle itself is no longer trustworthy
            // mark the connection broken so the pool replaces it; everything
            // else is the statement's problem and the connection stays.
            [[nodiscard]] error failure(const int rc) noexcept {
                const int primary = rc & 0xff;
                const bool broken = primary == SQLITE_IOERR || primary == SQLITE_MISUSE
                    || primary == SQLITE_NOMEM || primary == SQLITE_CORRUPT;
                if (broken) ok_ = false;
                return error{broken ? error_kind::connection : error_kind::query, sqlite3_errmsg(db_), {}, rc};
            }

            io io_;
            coro::static_thread_pool thread_;
            sqlite3* db_ = nullptr;
            std::unordered_map<const void*, sqlite3_stmt*> stmts_;
            bool ok_ = true;
        };
    };

    inline std::size_t sqlite::row::width() const noexcept {
        return r_->columns();
    }

    inline sqlite::cell sqlite::row::operator[](const std::size_t c) const {
        const auto& cells = r_->rows_[i_];
        if (c >= cells.size()) throw error{error_kind::conversion, "column index out of range"};
        return cell{&cells[c]};
    }

    inline sqlite::cell sqlite::row::operator[](const std::string_view name) const {
        const auto& names = r_->names_;
        for (std::size_t i = 0; i < names.size(); ++i) {
            if (names[i] == name) return (*this)[i];
        }
        throw error{error_kind::conversion, "no such column: " + std::string{name}};
    }

    static_assert(driver<sqlite>);
}
```

- [ ] **Step 5: Run the tests**

Run: `cmake --build cmake-build-debug --target sql_sqlite_test_sqlite sql_headers && ctest --test-dir cmake-build-debug -R '^sql_sqlite_test_sqlite$' --output-on-failure`
Expected: PASS, 12 tests. If `CompletionsResumeOnTheScheduler` fails, `execute` is missing its `co_await io_.schedule()` hop-back. If `BusyTimeoutSurfacesAsSqliteBusy` takes seconds, `busy_timeout` was not applied on open.

- [ ] **Step 6: Commit**

```bash
git add sql/include/sql/sqlite.h sql/tests/sqlite/test_sqlite.cpp sql/CMakeLists.txt sql/include/sql/sql.h
git commit -m "sql: sqlite driver -- pinned thread per connection, native storage classes

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_013iahp2MjjdDsXN7sxWSNyJ"
```

---

