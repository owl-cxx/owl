### Task 4: `io.h` — `reactor_ref` and `scheduler_ref`

**Files:**
- Create: `sql/include/sql/io.h`, `sql/tests/test_io.cpp`
- Modify: `sql/CMakeLists.txt` (FILE_SET + `sql_add_test(test_io)`), `sql/include/sql/sql.h` (add `#include "sql/io.h"`)

**Interfaces:**
- Consumes: `coro::io_reactor`, `coro::postable`, `coro::task`, `coro::wait_status`, `coro::interest`.
- Produces: `class sql::reactor_ref final` — default ctor (null), `template <coro::io_reactor R> explicit reactor_ref(R&)`, `coro::task<coro::wait_status> wait(int fd, coro::interest, std::chrono::milliseconds) const`, `void release(int fd) const`, `explicit operator bool`. `class sql::scheduler_ref final` — default ctor (null), `template <coro::postable S> explicit scheduler_ref(S&)`, `void post(std::coroutine_handle<>) const`, `auto schedule() const` (awaiter), `explicit operator bool`.

- [ ] **Step 1: Write the failing tests**

`sql/tests/test_io.cpp`:

```cpp
#include <gtest/gtest.h>

#include <chrono>
#include <coroutine>
#include <thread>
#include <unistd.h>
#include <vector>

#include <coro/executors/static_thread_pool.h>
#include <coro/io/reactor.h>
#include <coro/run/sync_wait.h>
#include <coro/task.h>

#include <sql/io.h>

using namespace std::chrono_literals;

namespace {
    struct recording_reactor final {
        std::vector<int> released;
        coro::wait_status answer = coro::wait_status::ready;

        coro::task<coro::wait_status> wait(int, coro::interest, std::chrono::milliseconds) {
            co_return answer;
        }

        coro::task<coro::wait_status> sleep(std::chrono::milliseconds) {
            co_return answer;
        }

        void release(const int fd) {
            released.push_back(fd);
        }
    };

    struct plain_reactor final {
        coro::task<coro::wait_status> wait(int, coro::interest, std::chrono::milliseconds) {
            co_return coro::wait_status::timeout;
        }

        coro::task<coro::wait_status> sleep(std::chrono::milliseconds) {
            co_return coro::wait_status::timeout;
        }
    };

    static_assert(coro::io_reactor<recording_reactor>);
    static_assert(coro::io_reactor<plain_reactor>);
}

TEST(ReactorRef, NullAnswersErrorWithoutParking) {
    const sql::reactor_ref io;
    EXPECT_FALSE(io);
    EXPECT_EQ(io.wait(0, coro::interest::read, 1ms).get(), coro::wait_status::error);
    io.release(0);
}

TEST(ReactorRef, ForwardsWaitAndRelease) {
    recording_reactor r;
    const sql::reactor_ref io{r};
    EXPECT_TRUE(io);
    EXPECT_EQ(io.wait(3, coro::interest::write, 1ms).get(), coro::wait_status::ready);
    r.answer = coro::wait_status::timeout;
    EXPECT_EQ(io.wait(3, coro::interest::write, 1ms).get(), coro::wait_status::timeout);
    io.release(7);
    io.release(8);
    EXPECT_EQ(r.released, (std::vector<int>{7, 8}));
}

TEST(ReactorRef, ReleaseIsANoOpForAReactorWithoutOne) {
    plain_reactor r;
    const sql::reactor_ref io{r};
    io.release(5);
    EXPECT_EQ(io.wait(5, coro::interest::read, 1ms).get(), coro::wait_status::timeout);
}

TEST(ReactorRef, DrivesANativeReactor) {
    coro::native_reactor reactor;
    const sql::reactor_ref io{reactor};
    int fds[2];
    ASSERT_EQ(::pipe(fds), 0);

    EXPECT_EQ(coro::sync_wait(io.wait(fds[0], coro::interest::read, 20ms)), coro::wait_status::timeout);
    EXPECT_EQ(::write(fds[1], "x", 1), 1);
    EXPECT_EQ(coro::sync_wait(io.wait(fds[0], coro::interest::read, 2s)), coro::wait_status::ready);

    ::close(fds[0]);
    ::close(fds[1]);
}

TEST(SchedulerRef, NullScheduleIsReadyAndPostResumesInline) {
    const sql::scheduler_ref s;
    EXPECT_FALSE(s);
    bool resumed = false;
    auto body = [&]() -> coro::task<> {
        co_await s.schedule();
        resumed = true;
    };
    auto t = body();
    t.start();
    EXPECT_TRUE(t.done());
    EXPECT_TRUE(resumed);
}

TEST(SchedulerRef, ScheduleHopsToThePool) {
    coro::static_thread_pool pool{1};
    const sql::scheduler_ref s{pool};
    EXPECT_TRUE(s);
    const auto main_id = std::this_thread::get_id();
    auto body = [&]() -> coro::task<bool> {
        co_await s.schedule();
        co_return std::this_thread::get_id() != main_id;
    };
    EXPECT_TRUE(coro::sync_wait(body()));
}

TEST(SchedulerRef, PostDeliversAHandle) {
    coro::static_thread_pool pool{1};
    const sql::scheduler_ref s{pool};
    struct manual final {
        sql::scheduler_ref s;
        bool await_ready() const noexcept { return false; }
        void await_suspend(const std::coroutine_handle<> h) const { s.post(h); }
        void await_resume() const noexcept {}
    };
    const auto main_id = std::this_thread::get_id();
    auto body = [&]() -> coro::task<bool> {
        co_await manual{s};
        co_return std::this_thread::get_id() != main_id;
    };
    EXPECT_TRUE(coro::sync_wait(body()));
}
```

- [ ] **Step 2: Register and run to see it fail**

Add `include/sql/io.h` to the FILE_SET, `sql_add_test(test_io)`, and `#include "sql/io.h"` to `sql.h` (keep the umbrella's includes alphabetical: `convert.h`, `error.h`, `io.h`).

Run: `cmake --build cmake-build-debug --target sql_test_io`
Expected: FAIL, header not found.

- [ ] **Step 3: Write `sql/include/sql/io.h`**

```cpp
#pragma once

// Two non-owning handles that let an un-templated driver reach whatever
// reactor or scheduler its pool was built on: a context pointer plus
// function pointers, filled in by a template constructor from any
// coro::io_reactor or coro::postable. This is the one indirection the
// library allows itself (spec requirement 4): a function-pointer call at
// the wait / hop boundary, next to a syscall, in exchange for keeping
// sql::pool<sql::psql> the spelling in every handler and keeping sql free
// of h2o. Neither handle owns anything; the reactor or scheduler must
// outlive every pool built on it. A default-constructed handle is null:
// wait answers error without parking, schedule() is ready at once, and
// post() resumes inline because "here" is the only place it has.

#include <chrono>
#include <coroutine>

#include <coro/concepts/io_reactor.h>
#include <coro/concepts/scheduler.h>
#include <coro/task.h>

namespace sql {
    class reactor_ref final {
    public:
        reactor_ref() = default;

        template <coro::io_reactor R>
        explicit reactor_ref(R& reactor) noexcept
            : ctx_(&reactor), wait_(&wait_thunk<R>), release_(&release_thunk<R>) {
        }

        [[nodiscard]] coro::task<coro::wait_status>
        wait(const int fd, const coro::interest want, const std::chrono::milliseconds timeout) const {
            if (ctx_ == nullptr) return null_wait();
            return wait_(ctx_, fd, want, timeout);
        }

        // Hands a descriptor back before its owner closes it. A reactor
        // that keeps no per-fd state (native_reactor) has no release; the
        // thunk is then a no-op, so drivers can call this unconditionally.
        void release(const int fd) const {
            if (ctx_ != nullptr) release_(ctx_, fd);
        }

        [[nodiscard]] explicit operator bool() const noexcept {
            return ctx_ != nullptr;
        }

    private:
        using wait_fn = coro::task<coro::wait_status> (*)(void*, int, coro::interest, std::chrono::milliseconds);
        using release_fn = void (*)(void*, int);

        static coro::task<coro::wait_status> null_wait() {
            co_return coro::wait_status::error;
        }

        template <typename R>
        static coro::task<coro::wait_status>
        wait_thunk(void* const ctx, const int fd, const coro::interest want, const std::chrono::milliseconds timeout) {
            return static_cast<R*>(ctx)->wait(fd, want, timeout);
        }

        template <typename R>
        static void release_thunk(void* const ctx, const int fd) {
            if constexpr (requires(R& r, const int f) { r.release(f); }) {
                static_cast<R*>(ctx)->release(fd);
            }
        }

        void* ctx_{};
        wait_fn wait_{};
        release_fn release_{};
    };

    class scheduler_ref final {
    public:
        scheduler_ref() = default;

        template <coro::postable S>
        explicit scheduler_ref(S& scheduler) noexcept : ctx_(&scheduler), post_(&post_thunk<S>) {
        }

        void post(const std::coroutine_handle<> h) const {
            if (ctx_ != nullptr) post_(ctx_, h);
            else h.resume();
        }

        [[nodiscard]] auto schedule() const noexcept {
            struct awaiter final {
                scheduler_ref self;

                [[nodiscard]] bool await_ready() const noexcept {
                    return !self;
                }

                void await_suspend(const std::coroutine_handle<> h) const {
                    self.post_(self.ctx_, h);
                }

                void await_resume() const noexcept {
                }
            };
            return awaiter{*this};
        }

        [[nodiscard]] explicit operator bool() const noexcept {
            return ctx_ != nullptr;
        }

    private:
        using post_fn = void (*)(void*, std::coroutine_handle<>);

        template <typename S>
        static void post_thunk(void* const ctx, const std::coroutine_handle<> h) {
            static_cast<S*>(ctx)->post(h);
        }

        void* ctx_{};
        post_fn post_{};
    };
}
```

- [ ] **Step 4: Run the test**

Run: `cmake --build cmake-build-debug --target sql_test_io sql_headers && ctest --test-dir cmake-build-debug -R '^sql_test_io$' --output-on-failure`
Expected: PASS, 7 tests.

- [ ] **Step 5: Commit**

```bash
git add sql/include/sql/io.h sql/tests/test_io.cpp sql/CMakeLists.txt sql/include/sql/sql.h
git commit -m "sql: reactor_ref and scheduler_ref -- function-pointer handles to the loop

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_013iahp2MjjdDsXN7sxWSNyJ"
```

---

