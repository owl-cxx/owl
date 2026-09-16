# Task 5 report: Split the `io_reactor` concept out of `io/reactor.h`

**Status:** DONE
**Worktree:** `/var/folders/ql/ct1l73_51kzg9s5clmgjhz_r0000gn/T/opencode/owl-coro-restructure`
**Branch:** `coro-restructure-sdd`
**Commit:** `9776bc1` Split the io_reactor concept out of io/reactor.h

## What landed

- Created `coro/include/coro/concepts/io_reactor.h` with `coro::interest`, `coro::wait_status`, and `coro::io_reactor<R>` (depends on `task.h`).
- Removed those two enums and the concept from `coro/include/coro/io/reactor.h`. `native_reactor` is unchanged. Replaced `#include <coro/task.h>` with `#include "coro/concepts/io_reactor.h"`. Left `<chrono>`, `<concepts>`, `<cstdint>` in `reactor.h`.
- Umbrella: `coro.h` includes `coro/concepts/io_reactor.h` before `coro/io/reactor.h`.
- FILE_SET: added `include/coro/concepts/io_reactor.h` next to `io/reactor.h`.

Spellings unchanged: no consumer rename.

## Verification

```
c++ -std=c++23 -fsyntax-only -Icoro/include /tmp/reactor_concept_check.cpp && echo "OK"
```

`OK` — `static_assert(coro::io_reactor<coro::native_reactor>)`.

```
echo '#include <coro/concepts/io_reactor.h>' | c++ -std=c++23 -fsyntax-only -Icoro/include -x c++ - && echo "OK"
```

`OK` — concepts header compiles without platform headers.

```
export PATH="/Applications/CLion.app/Contents/bin/ninja/mac/aarch64:$PATH"
cmake --preset Debug && cmake --build --preset Debug && ctest --preset Debug
```

26/26 passed. `test_umbrella` rebuilt.

## Self-review

- `native_reactor` body not touched.
- Include rule: `"coro/…"` in the new header and in `reactor.h`.
- Commit is `coro/` only; `cmake-build-debug/` untracked.

## Concerns

None. Line numbers in the brief matched the enums/concept block (34–41).
