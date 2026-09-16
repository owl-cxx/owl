# Task 1 report: Dissolve `util/`

**Status:** DONE
**Worktree:** `/var/folders/ql/ct1l73_51kzg9s5clmgjhz_r0000gn/T/opencode/owl-coro-restructure`
**Branch:** `coro-restructure-sdd`
**Commit:** `6588221` Dissolve coro/util into algo, result and run

## What landed

`coro/include/coro/util/` is gone. The six headers were `git mv`'d:

| From | To |
|---|---|
| `util/fmap.h` | `algo/fmap.h` |
| `util/combine.h` | `algo/combine.h` |
| `util/wait_all.h` | `algo/wait_all.h` |
| `util/result.h` | `result/result.h` |
| `util/as_result.h` | `result/as_result.h` |
| `util/sync_wait.h` | `run/sync_wait.h` |

Symbol names are unchanged: `coro::fmap`, `coro::combine`, `coro::wait_all`, `coro::result`, `coro::as_result`, `coro::sync_wait`.

Include-path rewrites (quotes inside `coro/include/coro/**`, angles elsewhere):

- `coro/include/coro/coro.h` — 6 include lines
- `coro/include/coro/algo/combine.h` — `fmap.h`, `wait_all.h`
- `coro/include/coro/result/as_result.h` — `result.h`
- `owl/include/owl/http/response.h:14` — `fmap.h`
- 9 files under `coro/tests/` (brief listed 8; `test_async_generator.cpp` and `test_sleep_for.cpp` also included `sync_wait.h`, plus the seven others)

`coro/CMakeLists.txt` FILE_SET regrouped to tree order: root, `scheduler/` (stands in for later `concepts/`), `algo/`, `result/`, `run/`, `executors/`, `io/`, `sync/`. File set identical; only order and paths changed.

`coro/README.md` was **not** updated (Task 12). BSD `grep` does not honor GNU `--include`, so the Step 2 rewrite also touched the README; that was reverted before commit.

## Verification

Step 3: `rg -n 'coro/util' coro owl main.cpp -g '*.h' -g '*.cpp' -g 'CMakeLists.txt'` — no output. `util/` directory gone.

Step 5: six `OK` lines — each moved file differs from `HEAD` original only in `#include` lines.

Step 6:

```
cmake --preset Debug && cmake --build --preset Debug && ctest --preset Debug
```

26/26 passed. Same names as `/tmp/coro-baseline.txt`.

## Self-review

- Pure relocation. No logic edits. No new `.cpp`. No symbol renames.
- Include rule held: `"coro/…"` inside `coro/include/coro/**`, `<coro/…>` in tests and owl.
- FILE_SET still lists every public header; `scheduler/` kept until Task 2.
- CMakeLists header comment still mentions `util/` — Task 12 owns that.
- Commit contains only `coro` and `owl`; `cmake-build-debug/` untracked.

## Concerns

None that affect later tasks. Brief said “8 files under `coro/tests/`”; 9 files actually referenced the old paths and were rewritten.
