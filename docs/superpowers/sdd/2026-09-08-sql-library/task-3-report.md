# Task 3 Report: `detail/placeholders.h` and `detail/stmt_key.h`

**Status: DONE** — commit `60e0b85` on `sql-library`.

## What I implemented

Transcribed from the brief verbatim:

- `sql/tests/test_placeholders.cpp` — 5 gtest cases (4 `Placeholders`, 1 `StmtKey`), heavy on `static_assert` so the constexpr path is checked at compile time.
- `sql/include/sql/detail/placeholders.h` — `sql::detail::placeholder_scan` (`unsigned count`, `const char* error`) and `constexpr` `scan_placeholders(std::string_view) noexcept`: single pass, skips single-quoted text (`''` toggles twice), ignores `$` not followed by digits (dollar quoting, `$identifier`), bitmap of seen numbers over `std::uint64_t`, errors for `$0`, n > 64, and non-contiguous numbering.
- `sql/include/sql/detail/stmt_key.h` — `template <fstr::fstr Q> struct stmt_key final`: `id` is the address of a static constexpr tag (one identity per literal instantiation), `text` = `Q.view()`, `c_str` = `Q.data` (fstr stores the NUL), `scan` = compile-time `scan_placeholders(Q.view())`.
- `sql/CMakeLists.txt` — both detail headers in the FILE_SET; `sql_add_test(test_placeholders)`. Umbrella `sql.h` untouched (the guard excludes `/detail/`, as intended).

## TDD Evidence

**RED** — after writing the test and registering `sql_add_test(test_placeholders)` (before the headers existed):

```
cmake --build cmake-build-debug --target sql_test_placeholders
...
FAILED: [code=1] sql/CMakeFiles/sql_test_placeholders.dir/tests/test_placeholders.cpp.o
/Users/professor/CLionProjects/owl/sql/tests/test_placeholders.cpp:7:10: fatal error: 'sql/detail/placeholders.h' file not found
ninja: build stopped: subcommand failed.
```

Exactly the failure the brief expects (header not found) — the test exercises code that does not exist yet.

**GREEN** — after writing both headers and adding them to the FILE_SET:

```
cmake --build cmake-build-debug --target sql_test_placeholders && ctest --test-dir cmake-build-debug -R '^sql_test_placeholders$' --output-on-failure
...
[2/5] Linking CXX executable sql/sql_test_placeholders
Test project /Users/professor/CLionProjects/owl/cmake-build-debug
1/1 Test #4: sql_test_placeholders ............   Passed    0.94 sec
100% tests passed out of 1
```

Direct binary run confirms all 5 cases: `[==========] 5 tests from 2 test suites ran. [  PASSED  ] 5 tests.`

## Full suite before commit

```
ctest --test-dir cmake-build-debug -R '^sql_' --output-on-failure
1/3 Test #2: sql_test_error ...................   Passed
2/3 Test #3: sql_test_convert .................   Passed
3/3 Test #4: sql_test_placeholders ............   Passed
100% tests passed out of 3
```

## Files changed (commit `60e0b85`)

- `sql/include/sql/detail/placeholders.h` (new, 61 lines)
- `sql/include/sql/detail/stmt_key.h` (new, 25 lines)
- `sql/tests/test_placeholders.cpp` (new, 56 lines)
- `sql/CMakeLists.txt` (modified, +3: two FILE_SET entries, one `sql_add_test`)

## Self-review findings

- RED observed before GREEN: yes, recorded above.
- Both headers in the FILE_SET: yes — the tree/manifest guard re-ran at build (`GLOB mismatch! ... Re-running CMake`) and configure succeeded with no FATAL_ERROR, proving disk and manifest agree. Test registered. `sql.h` untouched.
- Full sql suite green before commit: yes (3/3).
- Only task paths staged: yes — explicit `git add` of the 4 paths; `git show --stat` shows exactly those 4 files. The user's unrelated in-flight edits (coro/, owl/, sql/README.md) remain unstaged.

## Deviation from the brief's literal step order (one, deliberate)

Brief Step 2 said to add both headers to the FILE_SET before the RED run. That cannot work: the tree guard in `sql/CMakeLists.txt` compares the FILE_SET manifest against on-disk headers at configure time, so FILE_SET entries for nonexistent files would `FATAL_ERROR` at configure — a different failure than the brief's expected "header not found". I registered only the test for the RED run, then added the FILE_SET entries together with the headers for GREEN. End state is identical to the brief's.

## Issues / concerns

- None functional. CMake emits a pre-existing `-Wno-dev` warning about `SQLite::SQLite3` deprecation (sql/CMakeLists.txt:54, from Task 1's file, unrelated to this task). LSP noise about `h2o.h` in owl/ headers is the known pre-existing IDE issue; CMake builds are clean.
