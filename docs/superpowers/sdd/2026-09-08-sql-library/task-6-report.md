# Task 6 Report: `pool.h` — `lease<D>`, `source`, `pool<D>`

## What I implemented

Transcribed the brief's complete code with TDD discipline:

- `sql/include/sql/pool.h` — `sql::lease<D>` (move-only RAII checkout; null-owner leases return nothing), the `sql::source<S>` concept, and `sql::pool<D>` (lazy open to `config.connections`, FIFO intrusive waiters, direct handoff on release, broken-connection destroy-on-release with lazy replace, open-failure reporting without caching, `close()` failing waiters with `error_kind::closed`, and a drain loop so waiter resumption never deepens the stack). Single-threaded by contract — no mutex; the capability API is `const` with `mutable` internals.
- `sql/tests/test_pool.cpp` — the 11 GTest cases from the brief, verbatim.
- `sql/CMakeLists.txt` — `include/sql/pool.h` added to the FILE_SET (public headers before `detail/`, matching existing layout) and `sql_add_test(test_pool)`.
- `sql/include/sql/sql.h` — `#include "sql/pool.h"` in alphabetical position (last, after io.h). Configure-time tree/umbrella guards passed.

## TDD Evidence

### RED

Command: `cmake --build cmake-build-debug --target sql_test_pool`

Registration order followed the established resolution: the FILE_SET entry goes in the same edit as the header (the tree guard FATAL_ERRORs on a manifest entry with no file on disk), so for RED only `sql_add_test(test_pool)` was registered. Output:

```
FAILED: [code=1] sql/CMakeFiles/sql_test_pool.dir/tests/test_pool.cpp.o
/Users/professor/CLionProjects/owl/sql/tests/test_pool.cpp:12:10: fatal error: 'sql/pool.h' file not found
   12 | #include <sql/pool.h>
      |          ^~~~~~~~~~~~
1 error generated.
ninja: build stopped: subcommand failed.
```

Expected failure per the brief ("FAIL, header not found").

### GREEN

Command: `cmake --build cmake-build-debug --target sql_test_pool sql_headers && ctest --test-dir cmake-build-debug -R '^sql_test_pool$' --output-on-failure`

```
[1/4] Building CXX object sql/CMakeFiles/sql_headers.dir/header_checks/sql_pool.cpp.o
[2/4] Building CXX object sql/CMakeFiles/sql_test_pool.dir/tests/test_pool.cpp.o
[3/4] Linking CXX executable sql/sql_test_pool
1/1 Test #7: sql_test_pool ....................   Passed    0.95 sec
100% tests passed out of 1
```

All 11 test cases pass inside the binary; the generated header-check TU (`sql_pool.cpp`) also compiled, so the umbrella include and FILE_SET entry are consistent.

### Full sql suite

`ctest --test-dir cmake-build-debug -R '^sql_' --output-on-failure` → 6/6 passed (test_error, test_convert, test_placeholders, test_io, test_concepts, test_pool).

## Files changed (all committed)

- `sql/include/sql/pool.h` (new)
- `sql/tests/test_pool.cpp` (new)
- `sql/CMakeLists.txt` (modified: FILE_SET + test registration)
- `sql/include/sql/sql.h` (modified: umbrella include)

## Self-review findings

- RED observed and recorded before GREEN: yes (header-not-found compile failure).
- `pool.h` is in the FILE_SET and the umbrella (alphabetical position); configure-time tree/manifest/umbrella guards all passed as part of the build.
- No mutex anywhere in pool.h — grep confirms the only "mutex" occurrences are the contract comment and the `async_mutex::unlock` precedent reference in the drain-loop comment.
- Waiters are resumed via the drain loop (`resume` defers to a ready queue while `draining_`), so no resumption happens under pool bookkeeping and no recursion deepens the stack — verified by `DrainLoopServesAChainOfImmediateCompletionsWithoutRecursion` (5000 chained completions, one open).
- No `ASSERT_*` inside any coroutine body (plain `TEST` bodies use `ASSERT_*` freely; `take`/`run_one`/`body` use `EXPECT_*` + `co_return` where needed).
- Full sql suite green before the commit.
- Staged exactly the four task paths (`git add sql/include/sql/pool.h sql/tests/test_pool.cpp sql/CMakeLists.txt sql/include/sql/sql.h`); the user's in-flight edits (coro/, owl/, sql/README.md) were never staged. Post-commit `git status` shows only ` M sql/README.md` remaining.
- Commit message verbatim from the brief, including the required trailers.

## Issues or concerns

None. The brief's code compiled against the real coro/fstr headers without modification.
