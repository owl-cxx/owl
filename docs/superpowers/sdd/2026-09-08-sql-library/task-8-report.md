# Task 8 Report: `transaction.h`

**Status: DONE** — commit `e477f5b` on `sql-library`.

## What I Implemented

`sql::transaction(pool, body)` per the brief, verbatim:

- `sql/include/sql/transaction.h` — `transaction_source<D>` (copyable source whose `checkout()` answers a non-returning lease on the transaction's connection), `transaction_result<Task>` (specialized for `coro::task<std::expected<T, error>>`), `transaction_body_result_t`, the `transaction_body<F, D>` concept, `transaction_value_t`, `detail::begin_sql/commit_sql/rollback_sql` + `detail::rollback<D>` (abandons only when ROLLBACK itself fails), and `transaction()` (BEGIN → body → COMMIT on value / ROLLBACK on body error or throw / checkout & BEGIN failures propagate).
- The library's one `catch` lives here per spec 8/4.6: it captures `std::current_exception()`, runs the ROLLBACK coroutine **after** the try block (co_await is illegal in a catch handler), then `std::rethrow_exception`. No co_await inside catch.
- `sql/tests/test_transaction.cpp` — 11 cases: 2 concept-shape suites + commit/rollback/rollback-failure-loses-to-body-error/begin-failure/commit-failure/throw+rollback/rollback-fail-after-throw/void body/checkout-failure.
- Three compile-fail TUs: `rejects_nested_transaction.cpp`, `rejects_transaction_body_returning_task_t.cpp`, `rejects_transaction_body_not_a_task.cpp`.
- `sql/CMakeLists.txt`: `include/sql/transaction.h` added to FILE_SET, `sql_add_test(test_transaction)`, three `sql_add_compile_fail_test` registrations.
- `sql/include/sql/sql.h`: `#include "sql/transaction.h"` in alphabetical position (after query.h).

## TDD Evidence

### RED (before the header existed)

Command: `cmake --build cmake-build-debug --target sql_test_transaction`

```
CMake Error at sql/CMakeLists.txt:93 (message):
  sql: FILE_SET does not match the header tree.
    in FILE_SET but not on disk: sql/transaction.h
ninja: error: rebuilding 'build.ninja': subcommand failed
EXIT=1
```

Expected FAIL per brief Step 2 ("header not found"). The failure actually surfaced one step earlier, at configure time: the tree/manifest guard fires because the header is registered in the FILE_SET but not on disk. Same root cause — the header does not exist — and the build fails before any compilation, so RED is genuine. (The brief's Step 2 registers the FILE_SET entry before the header exists, which is what routes the failure through the guard rather than the preprocessor.)

### GREEN (after writing transaction.h)

Command: `cmake --build cmake-build-debug --target sql_test_transaction sql_headers && ctest --test-dir cmake-build-debug -R '^sql_test_transaction$|^rejects_nested_transaction$|^rejects_transaction_body' --output-on-failure`

```
1/4 Test  #9: sql_test_transaction ........................   Passed    1.02 sec
2/4 Test #13: rejects_nested_transaction ..................   Passed    0.36 sec
3/4 Test #14: rejects_transaction_body_returning_task_t ...   Passed    0.31 sec
4/4 Test #15: rejects_transaction_body_not_a_task .........   Passed    0.31 sec
100% tests passed out of 4
```

`sql_test_transaction` binary directly: `[==========] 11 tests from 2 test suites ran. [ PASSED ] 11 tests.`

Compile-fail tests fail for the right reason — each target built directly emits exactly one error, all three the same shape:

```
rejects_nested_transaction.cpp:16:22: error: no matching function for call to 'transaction'
rejects_transaction_body_returning_task_t.cpp:13:14: error: no matching function for call to 'transaction'
rejects_transaction_body_not_a_task.cpp:12:14: error: no matching function for call to 'transaction'
```

i.e. the `transaction_body<D>` constraint / pool-only first parameter rejecting the call, not an incidental error.

### Full sql suite before commit

- `ctest -R '^sql_'` → 8/8 pass (error, convert, placeholders, io, concepts, pool, query, transaction).
- `ctest -R '^rejects_'` → 14/14 pass (all 6 sql compile-fail tests + fstr/owl ones), WILL_FAIL conditions met.

## Files Changed (all in commit e477f5b)

- Created: `sql/include/sql/transaction.h`, `sql/tests/test_transaction.cpp`, `sql/tests/compile_fail/rejects_nested_transaction.cpp`, `sql/tests/compile_fail/rejects_transaction_body_returning_task_t.cpp`, `sql/tests/compile_fail/rejects_transaction_body_not_a_task.cpp`
- Modified: `sql/CMakeLists.txt`, `sql/include/sql/sql.h`

The user's unrelated in-flight edits (coro/, owl/ headers, sql/README.md, tasks/, owl/bench/) were not staged or committed.

## Self-Review Findings

- RED observed and recorded before GREEN. ✔
- Compile-fail tests verified failing for the right reason (concept/deduction rejection), not missing-header or unrelated errors. ✔
- transaction.h in FILE_SET and umbrella (alphabetical); three compile-fail tests registered. ✔
- Catch/rollback/rethrow structure exactly the brief's: capture in catch, rollback coroutine after the try, rethrow after; abandon only inside `detail::rollback` when ROLLBACK fails. No co_await in catch. ✔
- Full sql suite green (sql_test_* 8/8 and rejects_* 14/14) before commit. ✔
- Only the task's 7 paths staged; commit message verbatim with the required trailers. ✔
- Header/test code transcribed verbatim from the brief; no API improvisation was needed.

## Issues or Concerns

- None blocking. Two non-issues worth noting: (1) the RED failure message was the FILE_SET/manifest guard rather than "header not found" (explained above — same root cause); (2) pre-existing LSP/IDE noise (h2o.h not found, chrono duration diagnostics) appeared throughout, as the task note predicted; CMake is the source of truth and all CMake builds/tests pass.
