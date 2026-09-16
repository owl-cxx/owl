# Task 7 report: Complete the umbrella and enforce it at configure time

**Status:** DONE
**Worktree:** `/var/folders/ql/ct1l73_51kzg9s5clmgjhz_r0000gn/T/opencode/owl-coro-restructure`
**Branch:** `coro-restructure-sdd`
**Commit:** `13f306f` Complete the umbrella and check it at configure time

## What landed

- `coro/include/coro/coro.h` rewritten: 21 public includes in tree order; `detail/` omitted; comment that CMake checks the list.
- `coro/CMakeLists.txt` FILE_SET regrouped to the same tree order (relative paths throughout).
- Three-way configure-time check immediately after FILE_SET: disk glob vs FILE_SET vs umbrella includes. `_coro_on_disk` and `_coro_public` produced for Task 8.

## GUARD (a) — public header missing from the umbrella

Deleted `#include "coro/algo/when_any.h"` from `coro.h`, then `cmake --preset Debug`:

```
CMake Error at coro/CMakeLists.txt:141 (message):
  coro: coro.h does not match the public header tree.

    public but not in the umbrella: coro/algo/when_any.h
    in the umbrella but not public (or a detail/ header):

-- Configuring incomplete, errors occurred!
GUARD (a) OK
```

Reverted with `git checkout coro/include/coro/coro.h`.

## GUARD (b) — header on disk missing from FILE_SET

Deleted `include/coro/algo/when_any.h` from FILE_SET, then `cmake --preset Debug`:

```
CMake Error at coro/CMakeLists.txt:111 (message):
  coro: FILE_SET does not match the header tree.

    on disk but not in FILE_SET (would not install): coro/algo/when_any.h
    in FILE_SET but not on disk:

-- Configuring incomplete, errors occurred!
GUARD (b) OK
```

Reverted with `git checkout coro/CMakeLists.txt`. Clean configure after both reverts.

## Verification

```
export PATH="/Applications/CLion.app/Contents/bin/ninja/mac/aarch64:$PATH"
cmake --preset Debug && cmake --build --preset Debug && ctest --preset Debug
```

26/26 passed, including `test_umbrella`.

## Concerns

None. Guards fire on the strings the brief greps for (`not in the umbrella`, `would not install`).
