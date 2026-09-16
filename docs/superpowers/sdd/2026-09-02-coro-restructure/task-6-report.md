# Task 6 report: Make timed_wait_queue a detail of timer_scheduler

**Status:** DONE
**Worktree:** `/var/folders/ql/ct1l73_51kzg9s5clmgjhz_r0000gn/T/opencode/owl-coro-restructure`
**Branch:** `coro-restructure-sdd`
**Commit:** `0be2346` Make timed_wait_queue a detail of timer_scheduler

## What landed

- Moved `coro/include/coro/executors/timed_wait_queue.h` → `coro/include/coro/executors/detail/timed_wait_queue.h`.
- Namespace `coro` → `coro::detail` in that header (closing brace count unchanged).
- Include paths rewritten to `coro/executors/detail/timed_wait_queue.h`.
- `timer_scheduler.h`: include intact; four type references are `detail::timed_wait_queue`. Public surface and behavior unchanged.
- `test_sleep_for.cpp`: five construction sites now `coro::detail::timed_wait_queue`.
- Dropped from `coro.h`. FILE_SET entry is `include/coro/executors/detail/timed_wait_queue.h`.

## Verification

```
export PATH="/Applications/CLion.app/Contents/bin/ninja/mac/aarch64:$PATH"
cmake --preset Debug && cmake --build --preset Debug && ctest --preset Debug
```

26/26 passed, including `test_sleep_for`.

Include path not corrupted: `#include "coro/executors/detail/timed_wait_queue.h"` (not `detail/detail::`).

## Self-review

- `timer_scheduler` schedule/schedule_after body unchanged aside from type qualification.
- Commit is `coro/` only; `cmake-build-debug/` untracked.

## Concerns

BSD `sed` does not honor `\b`, so the brief's qualify-sed no-ops on macOS. Replacements were applied with exact-string edits instead; results match the expected include + four `detail::timed_wait_queue` references.
