# Task 4 report: Reunite the `as_result` overload set

**Status:** DONE
**Worktree:** `/var/folders/ql/ct1l73_51kzg9s5clmgjhz_r0000gn/T/opencode/owl-coro-restructure`
**Branch:** `coro-restructure-sdd`
**Commit:** `10cda64` Reunite the as_result overload set in one header

TDD order held: failing tests first, compile-fail observed, then production move.

## What landed

| From | To |
|---|---|
| `as_tuple.h` `detail::AwaiterAdapter` / `awaiter_storage_t` | `result/detail/awaiter_adapter.h` |
| `as_tuple.h` `AsResult` + `as_result(Aw&&)` | `result/as_result.h` (alongside existing `as_result(task<T>)`) |
| `as_tuple.h` `AsTuple` + `as_tuple` | `result/as_tuple.h` |

`coro/include/coro/as_tuple.h` deleted.

Symbols unchanged:

- `coro::as_result(task<T>) -> task<result<T>>`
- `coro::AsResult<Aw>`, `coro::as_result(Aw&&) -> AsResult<Aw>`
- `coro::as_result_transform`, `coro::as_result_of`, `operator|`
- `coro::AsTuple<Aw>`, `coro::as_tuple(Aw&&) -> AsTuple<Aw>`
- `coro::detail::awaiter_storage_t<Aw>`, `coro::detail::AwaiterAdapter<Aw>`

`test_as_result.cpp` does **not** include `as_tuple.h`. The three new cases pin that one header now exposes both overloads.

Umbrella (`coro.h`): added `coro/result/as_tuple.h` next to `result/as_result.h`.

FILE_SET: dropped `include/coro/as_tuple.h`; added `include/coro/result/as_tuple.h` and `include/coro/result/detail/awaiter_adapter.h` with the other `result/` headers.

## TDD evidence

### RED

Appended `BothOverloadsLiveInOneHeader`, `AwaitableOverloadCarriesValueThrough`, `AwaitableOverloadCapturesThrow` plus `#include <coroutine>` and `#include <coro/concepts/awaitable.h>`. No `as_tuple.h`.

```
cmake --build --preset Debug --target test_as_result
```

Compile failed as expected — `AsResult` not in `as_result.h`, awaitable overload not found:

```
test_as_result.cpp:152:32: error: no matching function for call to 'as_result'
                      decltype(coro::as_result(std::declval<ready_awaiter>())),
as_result.h:14:24: note: candidate template ignored: could not match 'task<T>' against 'ready_awaiter'
test_as_result.cpp:153:53: error: expected expression
                      coro::AsResult<ready_awaiter>>,
test_as_result.cpp:165:27: error: no matching function for call to 'as_result'
        auto r = co_await coro::as_result(ready_awaiter{});
test_as_result.cpp:180:27: error: no matching function for call to 'as_result'
        auto r = co_await coro::as_result(throwing_awaiter{});
4 errors generated.
```

Failure is the missing overload / missing `AsResult`, not a typo.

### GREEN

Extracted `awaiter_adapter.h`, moved `AsResult` + awaitable `as_result` into `result/as_result.h`, reduced and moved `as_tuple.h`.

```
cmake --build --preset Debug --target test_as_result && ctest --preset Debug -R test_as_result
```

```
[2/2] Linking CXX executable coro/test_as_result
1/1 Test #5: test_as_result ...................   Passed    0.34 sec
100% tests passed out of 1
```

`test_as_result --gtest_list_tests` includes the three new cases:

```
AsResult.
  ...
  BothOverloadsLiveInOneHeader
  AwaitableOverloadCarriesValueThrough
  AwaitableOverloadCapturesThrow
```

Static asserts compiled, so both overloads selected correctly from `as_result.h` alone.

### Full suite (after umbrella / FILE_SET)

```
export PATH="/Applications/CLion.app/Contents/bin/ninja/mac/aarch64:$PATH"
cmake --preset Debug && cmake --build --preset Debug && ctest --preset Debug
```

26/26 passed. `test_umbrella` rebuilt (now transitively compiles `result/as_tuple.h`).

## Self-review

- Overload bodies copied, not rewritten. `AwaiterAdapter` is the shared half that already existed in `as_tuple.h`.
- Include rule held: `"coro/…"` inside `coro/include/coro/**`.
- `awaiter_adapter.h` in FILE_SET, not in `coro.h` (detail).
- Commit is `coro/` only; `cmake-build-debug/` untracked.
- Git recorded `as_tuple.h` as delete + new `result/as_tuple.h` rather than a rename, because the body shrank (adapter/`AsResult` removed). Content is the split, not a new combinator.

## Concerns

None that affect later tasks. `coro/README.md` and `AGENTS.md` still show `#include <coro/as_tuple.h>` / “not in the umbrella” (Task 12). `operator|` still only pipes `task<T>`, not a bare awaitable — pre-existing, not this task. Git did not detect the `as_tuple.h` move as a rename.
