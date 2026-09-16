# Task 9 Report: `sqlite.h` — the SQLite driver

**Status: DONE** (one documented fix to the brief's code, no API change)
**Commit:** `894a875` — sql: sqlite driver -- pinned thread per connection, native storage classes

## What I implemented

- `sql/include/sql/sqlite.h`: `struct sql::sqlite final` satisfying `sql::driver`, with
  `config{path, connections, busy_timeout}`, `io = scheduler_ref`, `null_t`/`value`/`cell`/`row`/`result`,
  and `connection` (`open`, `ok`, `abandon`, `execute<Q>`, `cached_statements`). Each connection pins a
  1-thread `coro::static_thread_pool`; `execute` hops on (`thread_.schedule()`), runs prepare / bind /
  step to completion in one noexcept on-thread section, copies rows into sqlite's own storage classes
  (INTEGER/FLOAT/TEXT/BLOB/NULL), and hops back (`io_.schedule()`). Placeholders `$1..$N` bind through
  `sqlite3_bind_parameter_index("$k")` (number wins over appearance order); statements are cached per
  connection keyed by `detail::stmt_key<Q>::id`. Failures on open (connect) and on prepare/bind/step
  (query/connection via the IOERR/MISUSE/NOMEM/CORRUPT families) are `std::unexpected(sql::error{...})`.
- `sql/include/sql/sql.h`: guarded `#ifdef OWL_ENABLE_SQLITE` include block, per brief.
- `sql/CMakeLists.txt`: `include/sql/sqlite.h` in the FILE_SET; `sql_add_test(sqlite/test_sqlite)`
  inside `if (OWL_ENABLE_SQLITE)` in the `BUILD_TESTING` block, per brief.
- `sql/tests/sqlite/test_sqlite.cpp`: the brief's 12 tests, verbatim.

## TDD Evidence

**RED** — command: `cmake --build cmake-build-debug --target sql_sqlite_test_sqlite`

    CMake Error at sql/CMakeLists.txt:94 (message):
      sql: FILE_SET does not match the header tree.
        in FILE_SET but not on disk: sql/sqlite.h

Expected: the brief predicted exactly this configure-guard failure after Step 1 wires the FILE_SET
before the header exists.

A second RED followed after the header was written (before my fix): the test TU failed to compile —
`'names_'/'rows_'/'affected_' are private member(s) of 'sql::sqlite::result'` from
`connection::run_on_thread`. Expected: the brief's `friend class connection;` sits before
`sqlite::connection` is declared, so name lookup mints a new `sql::connection` in the namespace
instead of befriending the nested class.

**GREEN** — command: `cmake --build cmake-build-debug --target sql_sqlite_test_sqlite sql_headers && ctest --test-dir cmake-build-debug -R '^sql_sqlite_test_sqlite$' --output-on-failure`

    1/1 Test #10: sql_sqlite_test_sqlite ...........   Passed    1.10 sec
    100% tests passed out of 1

Direct binary run confirms the brief's count: `[==========] 12 tests ... [  PASSED  ] 12 tests.`
`BusyTimeoutSurfacesAsSqliteBusy` at 135 ms (not seconds) shows `busy_timeout` is applied on open.

## Files changed (all committed; only these staged)

- `sql/include/sql/sqlite.h` (new, 465 lines)
- `sql/tests/sqlite/test_sqlite.cpp` (new, 222 lines)
- `sql/CMakeLists.txt` (+4)
- `sql/include/sql/sql.h` (+4)

## Deviation from the brief (one, minimal)

The brief's `friend class connection;` inside `sqlite::result` is encountered before
`sqlite::connection` is declared. Per [namespace.memdef], a friend declaration whose name is not yet
declared first declares it — in the innermost enclosing **namespace** — so the friend named
`sql::connection`, not the nested `sqlite::connection`, and `run_on_thread` could not touch
`names_`/`rows_`/`affected_`. Fix: forward-declare `class connection;` as a member of `sqlite`
before `class result;` (with a why-comment), so the existing friend text binds to the real nested
class. No public or private API changed; everything else transcribed verbatim.

## Self-review findings

- RED observed and recorded before GREEN: yes (both rounds).
- sqlite.h guarded in the umbrella and in the FILE_SET; test registered under `OWL_ENABLE_SQLITE`: yes.
- No throw/catch in the driver: zero `catch`, zero `try`; the only throws are the synchronous
  `as<T>()` conversion/index errors, which error.h defines as thrown-by-design. Every sqlite3 status
  check (open, prepare, bind, step) returns `std::unexpected(sql::error{...})`.
- Full sql suite green before commit: `sql_test_{error,convert,placeholders,io,concepts,pool,query,transaction}`
  + `sql_sqlite_test_sqlite` 9/9, plus 14/14 `rejects_*` compile-fail tests (sql's 6 included) since
  the umbrella/FILE_SET they link against changed.
- Only task paths staged: yes — the user's in-flight coro/owl/README edits (incl. `sql/README.md`)
  were not touched.

## Issues or concerns

None beyond the documented one-line friend fix. LSP/IDE diagnostics (`h2o.h` not found, module
mismatch noise) appeared throughout and were ignored per task instructions; CMake was the source of
truth at every step.
