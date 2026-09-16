# Task 3 report: Split `awaitable.h`; move `when_all` / `when_any` into `algo/`

**Status:** DONE
**Worktree:** `/var/folders/ql/ct1l73_51kzg9s5clmgjhz_r0000gn/T/opencode/owl-coro-restructure`
**Branch:** `coro-restructure-sdd`
**Commit:** `0fe4001` Split awaitable.h; move when_all and when_any into algo/

## What landed

The line-68 cut matched the current file (contract 1–67, blank 68, engine 69–187). No invented boundary.

| From | To |
|---|---|
| `awaitable.h` lines 1–67 | `concepts/awaitable.h` |
| `awaitable.h` lines 69–187 | `algo/detail/leaf.h` |
| `when_all.h` | `algo/when_all.h` |
| `when_any.h` | `algo/when_any.h` |

`coro/include/coro/awaitable.h` deleted.

Symbols unchanged:

- Contract: `coro::Void`, `coro::detail::get_awaiter`, `coro::detail::awaiter_t`, `coro::await_result_t<T>`, `coro::await_value_t<T>`, `coro::Awaitable<T>`
- Engine: `coro::detail::Slot<T>`, `coro::detail::Leaf<State>`, `coro::detail::run_child<State,T,Aw>`
- Combinators: `coro::WhenAll`, `coro::WhenAllRange`, `coro::when_all`, `coro::WhenAny`, `coro::WhenAnyRange`, `coro::when_any`

`AllState` / `AnyState` / `no_winner` stayed in `when_all.h` / `when_any.h`. `get_awaiter` / `awaiter_t` shipped with the contract.

Include rewrites (quotes, `coro/` prefix):

- `algo/when_all.h`, `algo/when_any.h` — both halves: `coro/algo/detail/leaf.h` then `coro/concepts/awaitable.h`
- `as_tuple.h` — contract only: `coro/concepts/awaitable.h` (file itself stays at root until Task 4)

Umbrella (`coro.h`): added `concepts/awaitable.h` with the other concepts, `algo/when_all.h` and `algo/when_any.h` after `wait_all.h`. `leaf.h` is detail — FILE_SET yes, umbrella no.

FILE_SET: replaced `include/coro/awaitable.h`, `when_all.h`, `when_any.h` with `concepts/awaitable.h`, `algo/when_all.h`, `algo/when_any.h`, `algo/detail/leaf.h` in place (brief's replacement block). `as_tuple.h` stayed.

Owl/h2o comments removed from the split file as named in the brief (header, old `:36`, old `:81`). No remaining `owl`/`h2o` under `coro/include/`.

## Verification

Step 6 symbol presence:

```
struct Void            1
get_awaiter            3
using awaiter_t        1
using await_result_t   1
using await_value_t    1
concept Awaitable      1
struct Slot            1
class Leaf             1
run_child              2
```

Every symbol ≥1; `struct Void`, `struct Slot`, `class Leaf`, `concept Awaitable` exactly once.

Step 7:

```
export PATH="/Applications/CLion.app/Contents/bin/ninja/mac/aarch64:$PATH"
cmake --preset Debug && cmake --build --preset Debug && ctest --preset Debug
```

Rebuilt `test_umbrella` and `test_owl`. 26/26 passed. `test_umbrella` now transitively compiles `when_all.h`, `when_any.h`, and `concepts/awaitable.h`.

## Self-review

- No combinator/engine logic edits. Comments named in the brief + include lines only.
- Include rule held: `"coro/…"` inside `coro/include/coro/**`.
- `leaf.h` in FILE_SET, not in `coro.h`.
- Commit is `coro/` only; `cmake-build-debug/` untracked.
- Git recorded `awaitable.h` → `algo/detail/leaf.h` as a 64% rename plus a new `concepts/awaitable.h`; content is the split, not a move of the whole file.

## Concerns

None that affect later tasks. `coro/README.md` still shows `#include <coro/when_all.h>` / `when_any.h` (Task 12, same class as Task 1). FILE_SET keeps `concepts/awaitable.h` and the new `algo/` combinators in the old root slot rather than regrouped with the other `concepts/` / `algo/` entries — brief replaced paths in place. `concepts/awaitable.h` still carries unused `<cstddef>` / `<exception>` / `<optional>` from the 1–67 extract; not stripped.
