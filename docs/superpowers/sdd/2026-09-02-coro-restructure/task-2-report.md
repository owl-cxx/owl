# Task 2 report: Dissolve `scheduler/`

**Status:** DONE
**Worktree:** `/var/folders/ql/ct1l73_51kzg9s5clmgjhz_r0000gn/T/opencode/owl-coro-restructure`
**Branch:** `coro-restructure-sdd`
**Commit:** `2f0b089` Dissolve coro/scheduler into concepts and run

## What landed

`coro/include/coro/scheduler/` is gone. The four headers were `git mv`'d:

| From | To |
|---|---|
| `scheduler/scheduler.h` | `concepts/scheduler.h` |
| `scheduler/delayed_scheduler.h` | `concepts/delayed_scheduler.h` |
| `scheduler/schedule_on.h` | `run/schedule_on.h` |
| `scheduler/resume_on.h` | `run/resume_on.h` |

Symbol names are unchanged: `coro::scheduler`, `coro::postable`, `coro::delayed_scheduler`, `coro::schedule_on`, `coro::resume_on`.

Include-path rewrites (quotes inside `coro/include/coro/**`, angles elsewhere):

- `coro/include/coro/coro.h` — 4 include lines
- `coro/include/coro/concepts/delayed_scheduler.h` — `scheduler.h`
- `coro/include/coro/run/schedule_on.h`, `run/resume_on.h` — `scheduler.h`
- `coro/include/coro/executors/static_thread_pool.h` — `scheduler.h`
- `coro/include/coro/executors/timer_scheduler.h` — `delayed_scheduler.h`
- `owl/include/owl/coro/loop_scheduler.h:8` — `delayed_scheduler.h`
- `coro/tests/test_async_mutex.cpp`, `test_scheduling.cpp`, `test_sleep_for.cpp`

`coro/CMakeLists.txt`: FILE_SET paths rewritten in place (same listing order as the old `scheduler/` block). Taxonomy comment replaced with the layering invariant from the brief; the header-only / include-prefix intro above it was kept.

`AGENTS.md` and `docs/superpowers/plans/…` still mention `coro/scheduler/` (outside Step 2's `coro owl main.cpp` `*.h`/`*.cpp`/`CMakeLists.txt` scope). Same class of leftover as Task 1's README — Task 12.

## Verification

Step 3: `grep -rn --include='*.h' --include='*.cpp' --include='CMakeLists.txt' 'coro/scheduler/' coro owl main.cpp` — no output. `scheduler/` directory gone.

Step 4: four `OK` lines — each moved file differs from `HEAD` original only in `#include` lines.

Step 6:

```
export PATH="/Applications/CLion.app/Contents/bin/ninja/mac/aarch64:$PATH"
cmake --preset Debug && cmake --build --preset Debug && ctest --preset Debug
```

26/26 passed.

## Self-review

- Pure relocation. No logic edits. No new `.cpp`. No symbol renames.
- Include rule held: `"coro/…"` inside `coro/include/coro/**`, `<coro/…>` in tests and owl.
- FILE_SET still lists every public header. `schedule_on.h` / `resume_on.h` remain in the old scheduler slot (before `algo/`) rather than next to `run/sync_wait.h`; the brief rewrote paths only, so they were not regrouped.
- Commit contains only `coro` and `owl`; `cmake-build-debug/` untracked.
- Brief Step 2's `grep -rl -- PAT dirs --include=…` is GNU argument order; BSD grep treated `--include` as extra paths (stderr) but still searched `coro owl main.cpp`, so the rewrite applied correctly.

## Concerns

None that affect later tasks. FILE_SET reading order still interleaves `run/schedule_on.h` and `run/resume_on.h` with `concepts/` instead of grouping them with `run/sync_wait.h`.
