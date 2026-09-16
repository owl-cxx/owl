# Task 7 Report: `query.h` — `try_query`, `query`, `try_execute`, `execute`

**Status: DONE** — commit `d143a08` on `sql-library`.

## What I implemented

Transcribed the brief verbatim:

- `sql/include/sql/query.h` — `sql::result_t<S>` alias plus the four entry points, all generic over `source<S>`. `try_query` is the primitive (checkout → `(*lease)->template execute<Q>(args...)` → driver's `expected` untouched, with the two `static_assert`s on `detail::stmt_key<Q>::scan` firing at the call site); `query` and `execute` are the throwing wrappers; `try_execute` maps the result through `.affected()` via `std::move(r).transform(...)`.
- `sql/tests/test_query.cpp` — 7 tests: type identity static_asserts, checkout/run/release round trip, checkout failure propagation, driver-error throw (with sqlstate), `try_execute` → affected mapping, `execute` throw + count, and every `bindable` spelling accepted (bool, int, double, string, literal, blob, nullopt, optional<double>).
- Three compile-fail TUs under `sql/tests/compile_fail/`: arity mismatch, placeholder hole ($1,$3), unbindable argument (vector<int>).
- `sql/CMakeLists.txt`: `include/sql/query.h` in the FILE_SET, `sql_add_test(test_query)`, three `sql_add_compile_fail_test(...)` registrations.
- `sql/include/sql/sql.h`: `#include "sql/query.h"` in alphabetical position after `pool.h`.

## TDD Evidence

**RED** — `cmake --build cmake-build-debug --target sql_test_query` (after writing the four test files and registering the tests, before creating `query.h`):

```
FAILED: [code=1] sql/CMakeFiles/sql_test_query.dir/tests/test_query.cpp.o
/Users/professor/CLionProjects/owl/sql/tests/test_query.cpp:14:10: fatal error: 'sql/query.h' file not found
   14 | #include <sql/query.h>
```

Expected per brief Step 2 ("FAIL, header not found"). The tree guard did not fire because `query.h` was not yet on disk (FILE_SET and disk tree still matched at RED time).

**GREEN** — `cmake --build cmake-build-debug --target sql_test_query sql_headers && ctest --test-dir cmake-build-debug -R '^sql_test_query$|^rejects_arity_mismatch$|^rejects_placeholder_hole$|^rejects_unbindable_argument$' --output-on-failure`:

```
1/4 Test  #8: sql_test_query ...................   Passed    0.96 sec
2/4 Test  #9: rejects_arity_mismatch ...........   Passed    0.36 sec
3/4 Test #10: rejects_placeholder_hole .........   Passed    0.34 sec
4/4 Test #11: rejects_unbindable_argument ......   Passed    0.31 sec
100% tests passed out of 4
```

`sql_headers` built the new `sql_query.cpp.o` header-check TU cleanly (CMake tree/umbrella guards both passed at reconfigure).

**Compile-fail failure reasons verified** (built each target manually and read the compiler output):

- `rejects_arity_mismatch`: `static assertion failed due to requirement 'detail::stmt_key<fstr::fstr<14>{"SELECT $1, $2"}>::scan.count == sizeof...(Args)': sql: the query's $N placeholder count does not match the argument count` — the intended second static_assert (query.h:37).
- `rejects_placeholder_hole`: `static assertion failed due to requirement '...{"SELECT $1, $3"}>::scan.error == nullptr': sql: malformed placeholders -- $N must run contiguously from $1, at most $64` — the intended first static_assert (query.h:35). (The count assert fires as a knock-on; the intended one is present.)
- `rejects_unbindable_argument`: `no matching function for call to 'try_query' ... candidate template ignored: constraints not satisfied [with ... Args = <std::vector<int>>]` — the intended `bindable` constraint rejection.

**Full sql suite before commit** — `cmake --build cmake-build-debug --target sql_test_error sql_test_convert sql_test_placeholders sql_test_io sql_test_concepts sql_test_pool sql_test_query sql_headers && ctest --test-dir cmake-build-debug -R '^sql_test_|^rejects_' --output-on-failure`: **18/18 passed** (all 7 sql_test_* including test_query, plus the 3 new rejects_* and the pre-existing fstr/owl rejects_* caught by the regex).

## Files changed

- Created: `sql/include/sql/query.h`, `sql/tests/test_query.cpp`, `sql/tests/compile_fail/rejects_arity_mismatch.cpp`, `sql/tests/compile_fail/rejects_placeholder_hole.cpp`, `sql/tests/compile_fail/rejects_unbindable_argument.cpp`
- Modified: `sql/CMakeLists.txt`, `sql/include/sql/sql.h`
- Commit: `d143a08` "sql: query front end -- try_query, query, try_execute, execute" (7 files, 230 insertions; exact brief paths staged only — the user's unrelated in-flight edits in coro/, owl/, sql/README.md were left unstaged)

## Self-review findings

- RED observed and recorded before GREEN: yes, with the exact expected message.
- Compile-fail tests verified failing for the right reason: yes, all three outputs quoted above.
- `query.h` in the FILE_SET and umbrella in alphabetical position; three compile-fail tests registered: yes.
- Full sql suite green before commit: yes (18/18).
- Only task paths staged: yes — `git status` before commit showed the sql/* paths staged and everything else untouched.
- Brief code transcribed verbatim; no API improvisation was needed — the brief's code compiled as written on the first GREEN attempt.
- Spec req 8 holds: the `try_` path contains no `throw`/`catch`; only `query` and `execute` throw.

## Issues or concerns

None blocking. Two non-issues worth noting:

1. clangd/IDE diagnostics on the new test files (`'sql/query.h' file not found` before GREEN; a stale-PCH "file modified since precompiled" warning; a `result_like` constraint complaint against the fake driver) — all IDE noise; CMake is the source of truth and `sql_test_pool`/`sql_test_query` compile and run the same headers fine. The pre-existing `h2o.h not found` noise in owl headers was ignored as instructed.
2. A pre-existing CMake dev warning about the deprecated `SQLite::SQLite3` target name at `sql/CMakeLists.txt:58` surfaced during reconfigure — untouched by this task.
