# Task 4 Report: `io.h` — `reactor_ref` and `scheduler_ref`

## What I implemented

- `sql/include/sql/io.h` — `sql::reactor_ref` and `sql::scheduler_ref`, transcribed verbatim
  from the brief: non-owning context-pointer + function-pointer handles filled in by a
  constrained template constructor from any `coro::io_reactor` / `coro::postable`. Null
  handles: `wait` answers `error` without parking, `schedule()` is ready at once, `post`
  resumes inline. `release` is a no-op thunk for reactors without per-fd state
  (`if constexpr requires`).
- `sql/tests/test_io.cpp` — 7 GoogleTest cases (4 ReactorRef, 3 SchedulerRef), verbatim
  from the brief, including the `static_assert(coro::io_reactor<...>)` checks and the
  real-`native_reactor` pipe/timeout test.
- `sql/CMakeLists.txt` — FILE_SET entry `include/sql/io.h` + `sql_add_test(test_io)`.
- `sql/include/sql/sql.h` — `#include "sql/io.h"` in alphabetical position
  (`convert.h`, `error.h`, `io.h`); the configure-time tree/manifest/umbrella guards
  passed, confirming set equality.

## TDD Evidence

### RED

Command: `cmake --build cmake-build-debug --target sql_test_io`
(after writing `sql/tests/test_io.cpp` and registering `sql_add_test(test_io)`)

```
FAILED: [code=1] sql/CMakeFiles/sql_test_io.dir/tests/test_io.cpp.o
/Users/professor/CLionProjects/owl/sql/tests/test_io.cpp:14:10: fatal error: 'sql/io.h' file not found
   14 | #include <sql/io.h>
      |          ^~~~~~~~~~
1 error generated.
ninja: build stopped: subcommand failed.
```

Expected: the test is the only thing on disk; the header does not exist yet, so the
include fails. Matches the brief's Step 2 expectation ("FAIL, header not found").

### GREEN

Command: `cmake --build cmake-build-debug --target sql_test_io sql_headers && ctest --test-dir cmake-build-debug -R '^sql_test_io$' --output-on-failure`

```
[1/4] Building CXX object sql/CMakeFiles/sql_headers.dir/header_checks/sql_io.cpp.o
[2/4] Building CXX object sql/CMakeFiles/sql_test_io.dir/tests/test_io.cpp.o
[3/4] Linking CXX executable sql/sql_test_io
Test project /Users/professor/CLionProjects/owl/cmake-build-debug
    Start 5: sql_test_io
1/1 Test #5: sql_test_io ......................   Passed    0.95 sec
100% tests passed out of 1
```

The generated `sql_headers` TU (`sql_io.cpp.o` compiled) additionally proves the header
is self-contained. Direct binary run confirms 7/7 cases passed.

### Full sql suite before commit

Command: `cmake --build cmake-build-debug --target sql_test_error sql_test_convert sql_test_placeholders sql_test_io sql_headers && ctest --test-dir cmake-build-debug -R '^sql_' --output-on-failure`

```
1/4 Test #2: sql_test_error ...................   Passed    0.01 sec
2/4 Test #3: sql_test_convert .................   Passed    0.01 sec
3/4 Test #4: sql_test_placeholders ............   Passed    0.01 sec
4/4 Test #5: sql_test_io ......................   Passed    0.03 sec
100% tests passed out of 4
```

## Files changed (all committed in fc1e173)

- `sql/include/sql/io.h` (new, 121 lines)
- `sql/tests/test_io.cpp` (new, 131 lines)
- `sql/CMakeLists.txt` (FILE_SET entry + `sql_add_test(test_io)`)
- `sql/include/sql/sql.h` (umbrella include, alphabetical)

## Self-review findings

- RED recorded before GREEN: yes (output above).
- FILE_SET / test registration / umbrella include: yes; guards passed at configure.
- Full sql suite green before commit: yes (4/4).
- Only task paths staged: yes — the user's in-flight WIP (coro/, owl/, sql/README.md,
  tasks/) remains unstaged; commit contains exactly the four task files.
- Pre-verified all consumed coro APIs exist as the brief assumes: `io_reactor` concept +
  `interest` + `wait_status` (`coro/concepts/io_reactor.h`), `postable`
  (`coro/concepts/scheduler.h:26`), `native_reactor` (`coro/io/reactor.h:51`),
  `static_thread_pool{1}` + `post` (`coro/executors/static_thread_pool.h:35,106`),
  `task::start()/done()` (`coro/task.h:220,225`).

## Deviation from the brief's letter (with rationale)

Brief Step 2 says to add the FILE_SET entry and the umbrella include *before* the RED
run. That cannot configure: the tree/manifest guard FATAL_ERRORs on a FILE_SET entry for
a nonexistent file, and the umbrella guard would likewise fail (io.h listed in sql.h but
absent from the tree). Per the established Task 3 resolution, both registrations went
into the same edit as the header (Step 3). The RED run therefore had only
`sql_add_test(test_io)` registered and still failed exactly as the brief expected, so
the TDD signal is unaffected.

## Issues or concerns

None. The h2o LSP diagnostics in owl/ headers are the documented pre-existing IDE noise;
CMake is the source of truth and the sql tree is clean.
