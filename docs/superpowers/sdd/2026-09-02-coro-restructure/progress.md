# SDD ledger — plan: docs/superpowers/plans/2026-09-02-coro-restructure.md

Spec: docs/superpowers/specs/2026-09-02-coro-restructure-design.md
Workspace: /Users/professor/CLionProjects/owl (branch coro-restructure, primary checkout)

## Rulings (preflight)

- Ruling: work in place on existing `coro-restructure` branch — user already created it and said start implementation; GIT_DIR==GIT_COMMON so this is not a linked worktree; a second worktree would fork the same branch. Cost if wrong: feature work lands on the primary checkout (user already has it there).
- Ruling: harness Task tool has no model parameter — omit it; subagents inherit session model. Cost if wrong: spend the expensive model on mechanical tasks.
- Ruling: pre-existing dirty files (AGENTS.md, root CMakeLists.txt, READMEs, main.cpp, owl/CMakeLists.txt, owl/routing/router.h, untracked cmake/) stay unstaged. Implementers must NOT `git add -A`; stage only files the task names. Cost if wrong: unrelated WIP rides the restructure commits.
- Ruling: Task 12 `cmake --install` depends on the uncommitted root `install(TARGETS ...)` already in the working tree; leave it in place, do not commit it as part of this plan (spec: packaging out of scope). Cost if wrong: install check uses unpublished CMake, or fails if someone reverts the dirty file.

## Preflight scan

| Pair | Shared | Produces vs consumes | Finding |
|---|---|---|---|
| T1 → T2 | `run/`, `coro.h`, `coro/CMakeLists.txt` | T1 creates `run/`; T2 adds schedule_on/resume_on | sequential, OK |
| T1 → T3 | `algo/`, `coro.h`, CMake | T1 creates `algo/`; T3 adds when_all/when_any/leaf | sequential, OK |
| T1 → T4 | `result/as_result.h`, `test_as_result.cpp` | T1 moves as_result; T4 absorbs AsResult | sequential, OK |
| T2 → T6 | `coro.h`, CMake | T2 regroups comments; T6 drops timed_wait_queue from umbrella | sequential, OK |
| T3 → T4 | `as_tuple.h` include line | T3 rewrites include to concepts/awaitable.h; T4 moves the file | sequential, OK |
| T3–T6 → T7 | `coro.h` | T3–T6 add includes piecemeal; T7 rewrites the whole umbrella in tree order | sequential, OK — T7 is the authority |
| T7 → T8 | `_coro_public` | T7 produces; T8 consumes | sequential, OK |
| T8 → T9 | `if (BUILD_TESTING)` in CMake | T8 inserts `coro_headers` before `coro_add_test`; T9 replaces `coro_add_test` | sequential, OK if T9 keeps the T8 block |
| T9 → T10/T11 | tests tree, CMake | T9 creates empty `concepts/` `io/`; T10/T11 fill them | sequential, OK |
| T1 vs dirty tree | `git add -A coro owl` | would stage dirty `coro/README.md` and unrelated owl files | ruled above: no `git add -A` |
| T12 vs dirty README | `coro/README.md` already dirty | T12 owns the rewrite | sequential, OK |

### Per-task self-consistency

| Task | Check | Finding |
|---|---|---|
| T3 | lines 1–67 vs 69–187 of awaitable.h (188 lines, cut at blank 68) | matches: 67 is end of `Awaitable`, 69 opens `namespace detail` for Slot/Leaf |
| T3 | `{ sed 1,67p; echo '}'; }` | extra `}` closes `namespace coro`; not a double-close |
| T3 vs rubric | verbatim split of awaitable.h | plan-mandated redistribution, not duplication |
| T4 | TDD: tests first, fail because AsResult lives in as_tuple.h | matches spec; tests must not include as_tuple.h |
| T4 | both as_result overloads in one header | partial ordering asserted by tests |
| T5 | replace `<coro/task.h>` with concepts include; leave unused std includes | matches include rule + "don't strip std includes in a structural commit" |
| T6 | namespace move + leave umbrella | more than include-only; plan specifies it; not a T1–6 "stop if logic changes" violation |
| T8 | generated TUs from `_coro_public` | 21 files, no detail, no coro.h |
| T9 | empty `concepts/` and `io/` until T10/T11 | intended |
| T10/T11 | real behavioral tests with timing | not empty asserts; T11 isolated so droppable |
| T12 | README sed + install check | packaging CMake is dirty/uncommitted (ruled) |

Scan is clean enough to start. No pair contradicts Global Constraints. Dispatch Task 0 then Task 1.

## Task 0: complete (no commit — evidence only)

- cmake --preset Debug && cmake --build --preset Debug: clean
- ctest --preset Debug: 26/26 passing
- coro tests: test_task, test_async_generator, test_fmap, test_combine, test_as_result, test_wait_all, test_scheduling, test_async_mutex, test_sleep_for, test_umbrella
- orphans awaitable.h when_all.h when_any.h as_tuple.h: all OK
- names: /tmp/coro-baseline.txt
- HEAD at baseline: 38cc06b

## Task 1
