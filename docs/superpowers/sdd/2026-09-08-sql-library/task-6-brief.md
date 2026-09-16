### Task 6: `pool.h` — `lease<D>`, `source`, `pool<D>`

**Files:**
- Create: `sql/include/sql/pool.h`, `sql/tests/test_pool.cpp`
- Modify: `sql/CMakeLists.txt` (FILE_SET + `sql_add_test(test_pool)`), `sql/include/sql/sql.h` (add `#include "sql/pool.h"`)

**Interfaces:**
- Consumes: `sql::driver`, `sql::error`, `sql_test::fake` (tests).
- Produces: `template <driver D> class sql::lease final` (move-only; `lease(connection*, const pool<D>* owner)`, `operator->`, `operator*`, `explicit operator bool`; a null owner returns nothing); concept `sql::source<S>` (`typename S::driver`, `checkout() const -> coro::task<std::expected<lease<driver>, error>>`); `template <driver D> class sql::pool final` with `using driver = D; using connection = D::connection;`, `pool(D::config, D::io)`, `checkout() const`, `void close()`, `std::size_t size() const`, `idle() const`, `waiting() const`. The lease's release path is `pool::give_back(connection*) const noexcept` (private, `friend class lease<D>`).

- [ ] **Step 1: Write the failing tests**

`sql/tests/test_pool.cpp`:

```cpp
#include <gtest/gtest.h>

#include <expected>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <coro/task.h>

#include <sql/error.h>
#include <sql/pool.h>

#include "support/fake_driver.h"

namespace {
    using fake = sql_test::fake;
    using pool_t = sql::pool<fake>;
    using lease_t = sql::lease<fake>;

    struct fixture final {
        fake::script s;
        pool_t pool;

        explicit fixture(const unsigned connections) : pool({.connections = connections, .script_ = &s}, {}) {
        }
    };

    // Starts a checkout and hands the lease out through `slot` when it lands.
    coro::task<> take(const pool_t& pool, std::optional<lease_t>& slot, std::optional<sql::error>& failure) {
        auto r = co_await pool.checkout();
        if (r) slot.emplace(std::move(*r));
        else failure.emplace(std::move(r.error()));
    }

    coro::task<> run_one(const pool_t& pool, int& done) {
        auto l = co_await pool.checkout();
        EXPECT_TRUE(l.has_value());
        if (!l) co_return;
        const auto r = co_await (*l)->execute<"X">();
        EXPECT_TRUE(r.has_value());
        ++done;
    }
}

TEST(Pool, SatisfiesSource) {
    static_assert(sql::source<pool_t>);
}

TEST(Pool, IdleConnectionIsReusedWithoutReopening) {
    fixture fx{1};
    std::optional<lease_t> a;
    std::optional<sql::error> err;
    auto t1 = take(fx.pool, a, err);
    t1.start();
    ASSERT_TRUE(a.has_value());
    EXPECT_EQ(fx.pool.size(), 1u);
    EXPECT_EQ(fx.pool.idle(), 0u);
    a.reset();
    EXPECT_EQ(fx.pool.idle(), 1u);

    std::optional<lease_t> b;
    auto t2 = take(fx.pool, b, err);
    t2.start();
    ASSERT_TRUE(b.has_value());
    EXPECT_EQ(fx.s.opened, 1);
    b.reset();
}

TEST(Pool, OpensLazilyUpToTheLimitThenParks) {
    fixture fx{2};
    std::optional<lease_t> a, b, c;
    std::optional<sql::error> err;
    auto t1 = take(fx.pool, a, err);
    auto t2 = take(fx.pool, b, err);
    auto t3 = take(fx.pool, c, err);
    t1.start();
    t2.start();
    t3.start();
    EXPECT_TRUE(a && b);
    EXPECT_FALSE(c);
    EXPECT_EQ(fx.s.opened, 2);
    EXPECT_EQ(fx.pool.waiting(), 1u);

    a.reset();
    EXPECT_TRUE(c);
    EXPECT_EQ(fx.s.opened, 2);
    EXPECT_EQ(fx.pool.waiting(), 0u);
    EXPECT_EQ(fx.pool.idle(), 0u);
    b.reset();
    c.reset();
}

TEST(Pool, WaitersAreServedFifoByDirectHandoff) {
    fixture fx{1};
    std::optional<lease_t> a, b, c;
    std::optional<sql::error> err;
    auto t1 = take(fx.pool, a, err);
    auto t2 = take(fx.pool, b, err);
    auto t3 = take(fx.pool, c, err);
    t1.start();
    t2.start();
    t3.start();
    ASSERT_TRUE(a);
    EXPECT_EQ(fx.pool.waiting(), 2u);

    a.reset();
    EXPECT_TRUE(b);
    EXPECT_FALSE(c);
    b.reset();
    EXPECT_TRUE(c);
    EXPECT_EQ(fx.pool.idle(), 0u);
    c.reset();
    EXPECT_EQ(fx.pool.idle(), 1u);
    EXPECT_EQ(fx.s.opened, 1);
}

TEST(Pool, BrokenConnectionIsDestroyedOnReleaseAndReplacedLazily) {
    fixture fx{1};
    fx.s.runs.emplace_back(std::unexpected(sql::error{sql::error_kind::connection, "gone"}));
    auto body = [&]() -> coro::task<> {
        auto l = co_await fx.pool.checkout();
        EXPECT_EQ((co_await (*l)->execute<"X">()).error().kind(), sql::error_kind::connection);
    };
    auto t = body();
    t.start();
    EXPECT_EQ(fx.s.destroyed, 1);
    EXPECT_EQ(fx.pool.size(), 0u);

    int done = 0;
    auto t2 = run_one(fx.pool, done);
    t2.start();
    EXPECT_EQ(done, 1);
    EXPECT_EQ(fx.s.opened, 2);
}

TEST(Pool, BrokenReleaseWakesAWaiterToOpenAFreshOne) {
    fixture fx{1};
    fx.s.runs.emplace_back(std::unexpected(sql::error{sql::error_kind::connection, "gone"}));
    std::optional<lease_t> b;
    std::optional<sql::error> err;
    auto body = [&]() -> coro::task<> {
        auto l = co_await fx.pool.checkout();
        (void)co_await (*l)->execute<"X">();
        fx.s.suspend = true;
        co_await (*l)->execute<"Y">();
        fx.s.suspend = false;
    };
    auto t1 = body();
    t1.start();
    auto t2 = take(fx.pool, b, err);
    t2.start();
    EXPECT_EQ(fx.pool.waiting(), 1u);
    fx.s.resume_all();
    ASSERT_TRUE(b);
    EXPECT_EQ(fx.s.opened, 2);
    EXPECT_EQ(fx.s.destroyed, 1);
    b.reset();
}

TEST(Pool, OpenFailureIsReportedAndNotCached) {
    fixture fx{1};
    fx.s.opens.emplace_back(std::unexpected(sql::error{sql::error_kind::connect, "refused"}));
    std::optional<lease_t> a;
    std::optional<sql::error> err;
    auto t1 = take(fx.pool, a, err);
    t1.start();
    ASSERT_TRUE(err);
    EXPECT_EQ(err->kind(), sql::error_kind::connect);
    EXPECT_EQ(fx.pool.size(), 0u);

    err.reset();
    auto t2 = take(fx.pool, a, err);
    t2.start();
    EXPECT_TRUE(a);
    EXPECT_EQ(fx.s.opened, 2);
    a.reset();
}

TEST(Pool, OpenFailureWakesAParkedWaiter) {
    fixture fx{1};
    fx.s.suspend_open = true;
    fx.s.opens.emplace_back(std::unexpected(sql::error{sql::error_kind::connect, "refused"}));
    std::optional<lease_t> a, b;
    std::optional<sql::error> err_a, err_b;
    auto t1 = take(fx.pool, a, err_a);
    auto t2 = take(fx.pool, b, err_b);
    t1.start();
    t2.start();
    EXPECT_EQ(fx.pool.waiting(), 1u);
    fx.s.suspend_open = false;
    fx.s.resume_all();
    EXPECT_TRUE(err_a);
    EXPECT_TRUE(b);
    EXPECT_EQ(fx.s.opened, 2);
    b.reset();
}

TEST(Pool, CloseFailsWaitersDestroysIdleAndRefusesCheckout) {
    fixture fx{1};
    std::optional<lease_t> a, b, c;
    std::optional<sql::error> err_b, err_c;
    auto t1 = take(fx.pool, a, err_b);
    auto t2 = take(fx.pool, b, err_b);
    t1.start();
    t2.start();
    fx.pool.close();
    ASSERT_TRUE(err_b);
    EXPECT_EQ(err_b->kind(), sql::error_kind::closed);
    a.reset();
    EXPECT_EQ(fx.s.destroyed, 1);
    EXPECT_EQ(fx.pool.size(), 0u);
    auto t3 = take(fx.pool, c, err_c);
    t3.start();
    EXPECT_EQ(err_c->kind(), sql::error_kind::closed);
}

TEST(Pool, DrainLoopServesAChainOfImmediateCompletionsWithoutRecursion) {
    fixture fx{1};
    fx.s.suspend = true;
    int done = 0;
    std::vector<coro::task<>> tasks;
    for (int i = 0; i < 5000; ++i) tasks.push_back(run_one(fx.pool, done));
    for (auto& t : tasks) t.start();
    EXPECT_EQ(done, 0);
    EXPECT_EQ(fx.pool.waiting(), 4999u);
    fx.s.suspend = false;
    fx.s.resume_all();
    EXPECT_EQ(done, 5000);
    EXPECT_EQ(fx.s.opened, 1);
    EXPECT_EQ(fx.s.log.size(), 5000u);
}

TEST(Pool, LeaseMoveTransfersOwnership) {
    fixture fx{1};
    std::optional<lease_t> a;
    std::optional<sql::error> err;
    auto t = take(fx.pool, a, err);
    t.start();
    lease_t moved = std::move(*a);
    a.reset();
    EXPECT_EQ(fx.pool.idle(), 0u);
    EXPECT_TRUE(moved);
    moved = lease_t{};
    EXPECT_EQ(fx.pool.idle(), 1u);
}
```

- [ ] **Step 2: Register and run to see it fail**

Add `include/sql/pool.h` to the FILE_SET, `sql_add_test(test_pool)`, and `#include "sql/pool.h"` to `sql.h`.

Run: `cmake --build cmake-build-debug --target sql_test_pool`
Expected: FAIL, header not found.

- [ ] **Step 3: Write `sql/include/sql/pool.h`**

```cpp
#pragma once

// pool<D>: the connections of one driver, handed out as leases, on ONE
// thread. Single-threaded by contract -- in owl every pool is per worker
// and lives on that worker's loop -- so there is no mutex, and the
// capability API is const with mutable internals: a handler extracts a
// const pool&, because a source is something you use, not something you
// own.
//
// Connections open lazily up to config.connections. A checkout takes an
// idle one, opens one if there is room, or parks in a FIFO of intrusive
// waiter nodes (no allocation). A released connection goes straight to
// the first waiter. One that reports !ok() is destroyed on release and
// replaced lazily; the first waiter is woken to do the replacing.
// Resumption runs a drain loop rather than a recursion, the way
// async_mutex::unlock does, so a chain of waiters that complete without
// suspending never deepens the stack.
//
// close() fails parked checkouts with error_kind::closed and destroys idle
// connections; a leased one dies on release. Destroying a pool with a
// lease outstanding is a contract violation, like coro's executors, and
// asserts in debug builds.

#include <algorithm>
#include <cassert>
#include <concepts>
#include <coroutine>
#include <cstddef>
#include <expected>
#include <memory>
#include <utility>
#include <vector>

#include <coro/task.h>

#include "sql/concepts.h"
#include "sql/error.h"

namespace sql {
    template <driver D>
    class pool;

    // RAII checkout. Move-only; the return happens in the destructor. A
    // lease with no owner -- a transaction's -- returns nothing.
    template <driver D>
    class lease final {
    public:
        using connection = typename D::connection;

        lease() = default;

        lease(connection* const conn, const pool<D>* const owner) noexcept : conn_(conn), owner_(owner) {
        }

        lease(lease&& o) noexcept : conn_(std::exchange(o.conn_, nullptr)), owner_(std::exchange(o.owner_, nullptr)) {
        }

        lease& operator=(lease&& o) noexcept {
            if (this != &o) {
                release();
                conn_ = std::exchange(o.conn_, nullptr);
                owner_ = std::exchange(o.owner_, nullptr);
            }
            return *this;
        }

        lease(const lease&) = delete;
        lease& operator=(const lease&) = delete;

        ~lease() {
            release();
        }

        [[nodiscard]] connection* operator->() const noexcept {
            return conn_;
        }

        [[nodiscard]] connection& operator*() const noexcept {
            return *conn_;
        }

        [[nodiscard]] explicit operator bool() const noexcept {
            return conn_ != nullptr;
        }

    private:
        void release() noexcept {
            if (conn_ != nullptr && owner_ != nullptr) owner_->give_back(conn_);
            conn_ = nullptr;
            owner_ = nullptr;
        }

        connection* conn_{};
        const pool<D>* owner_{};
    };

    // Anything query<> can run against: a pool, or a transaction on one.
    template <typename S>
    concept source = driver<typename S::driver> && requires(const S& s) {
        { s.checkout() } -> std::same_as<coro::task<std::expected<lease<typename S::driver>, error>>>;
    };

    template <driver D>
    class pool final {
    public:
        using driver = D;
        using connection = typename D::connection;

        pool(typename D::config cfg, typename D::io io)
            : cfg_(std::move(cfg)),
              io_(std::move(io)),
              limit_(cfg_.connections == 0 ? 1 : cfg_.connections) {
        }

        ~pool() {
            close();
            assert(connections_.empty() && "sql::pool destroyed with a lease outstanding");
        }

        pool(const pool&) = delete;
        pool& operator=(const pool&) = delete;

        [[nodiscard]] coro::task<std::expected<lease<D>, error>> checkout() const {
            for (;;) {
                if (closed_) co_return std::unexpected(error{error_kind::closed, "pool is closed"});
                if (!idle_.empty()) {
                    connection* const c = idle_.back();
                    idle_.pop_back();
                    co_return lease<D>{c, this};
                }
                if (connections_.size() + opening_ < limit_) {
                    ++opening_;
                    auto opened = co_await connection::open(cfg_, io_);
                    --opening_;
                    if (closed_) co_return std::unexpected(error{error_kind::closed, "pool is closed"});
                    if (!opened) {
                        // The slot is free again; a waiter parked meanwhile
                        // must get its own chance to open.
                        wake_one();
                        co_return std::unexpected(std::move(opened.error()));
                    }
                    connection* const c = opened->get();
                    connections_.push_back(std::move(*opened));
                    co_return lease<D>{c, this};
                }
                waiter w{};
                co_await park{this, &w};
                if (w.closed) co_return std::unexpected(error{error_kind::closed, "pool is closed"});
                if (w.handed != nullptr) co_return lease<D>{w.handed, this};
                // Woken because a slot freed up: loop and open.
            }
        }

        void close() {
            if (closed_) return;
            closed_ = true;
            while (waiter* const w = pop_waiter()) {
                w->closed = true;
                resume(w);
            }
            for (connection* const c : idle_) destroy(c);
            idle_.clear();
        }

        [[nodiscard]] std::size_t size() const noexcept {
            return connections_.size();
        }

        [[nodiscard]] std::size_t idle() const noexcept {
            return idle_.size();
        }

        [[nodiscard]] std::size_t waiting() const noexcept {
            std::size_t n = 0;
            for (const waiter* w = head_; w != nullptr; w = w->next) ++n;
            return n;
        }

    private:
        friend class lease<D>;

        struct waiter final {
            waiter* next{};
            std::coroutine_handle<> h{};
            connection* handed{};
            bool closed = false;
        };

        struct park final {
            const pool* self;
            waiter* w;

            [[nodiscard]] bool await_ready() const noexcept {
                return false;
            }

            void await_suspend(const std::coroutine_handle<> h) const {
                w->h = h;
                self->push_waiter(w);
            }

            void await_resume() const noexcept {
            }
        };

        void give_back(connection* const c) const noexcept {
            if (closed_ || !c->ok()) {
                destroy(c);
                wake_one();
                return;
            }
            if (waiter* const w = pop_waiter()) {
                w->handed = c;
                resume(w);
                return;
            }
            idle_.push_back(c);
        }

        void wake_one() const noexcept {
            if (waiter* const w = pop_waiter()) resume(w);
        }

        // The drain loop. A resumption that happens while one is already
        // running -- a resumed waiter releasing its lease before it
        // suspends -- is queued and run by the outer call, so the depth
        // stays one whatever the chain length.
        void resume(waiter* const w) const noexcept {
            if (draining_) {
                push_ready(w);
                return;
            }
            draining_ = true;
            w->h.resume();
            while (waiter* const next = pop_ready()) next->h.resume();
            draining_ = false;
        }

        void destroy(connection* const c) const noexcept {
            std::erase_if(connections_, [c](const auto& owned) {
                return owned.get() == c;
            });
        }

        void push_waiter(waiter* const w) const noexcept {
            w->next = nullptr;
            if (tail_ != nullptr) tail_->next = w;
            else head_ = w;
            tail_ = w;
        }

        [[nodiscard]] waiter* pop_waiter() const noexcept {
            waiter* const w = head_;
            if (w == nullptr) return nullptr;
            head_ = w->next;
            if (head_ == nullptr) tail_ = nullptr;
            w->next = nullptr;
            return w;
        }

        void push_ready(waiter* const w) const noexcept {
            w->next = nullptr;
            if (ready_tail_ != nullptr) ready_tail_->next = w;
            else ready_head_ = w;
            ready_tail_ = w;
        }

        [[nodiscard]] waiter* pop_ready() const noexcept {
            waiter* const w = ready_head_;
            if (w == nullptr) return nullptr;
            ready_head_ = w->next;
            if (ready_head_ == nullptr) ready_tail_ = nullptr;
            w->next = nullptr;
            return w;
        }

        typename D::config cfg_;
        typename D::io io_;
        unsigned limit_;
        mutable std::vector<std::unique_ptr<connection>> connections_;
        mutable std::vector<connection*> idle_;
        mutable waiter* head_{};
        mutable waiter* tail_{};
        mutable waiter* ready_head_{};
        mutable waiter* ready_tail_{};
        mutable unsigned opening_ = 0;
        mutable bool closed_ = false;
        mutable bool draining_ = false;
    };
}
```

- [ ] **Step 4: Run the test**

Run: `cmake --build cmake-build-debug --target sql_test_pool sql_headers && ctest --test-dir cmake-build-debug -R '^sql_test_pool$' --output-on-failure`
Expected: PASS, 11 tests. If `DrainLoopServesAChain...` crashes with a stack overflow, the drain loop is not engaged: check that `give_back` reaches `resume(w)` and that `resume` defers when `draining_` is set.

- [ ] **Step 5: Commit**

```bash
git add sql/include/sql/pool.h sql/tests/test_pool.cpp sql/CMakeLists.txt sql/include/sql/sql.h
git commit -m "sql: pool<D> with leases, FIFO waiters, and a drain loop

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_013iahp2MjjdDsXN7sxWSNyJ"
```

---

