# Contributor infrastructure

Date: 2026-09-16
Status: approved, pending implementation

## Problem

owl is two weeks old: 112 commits, one author, six libraries with 1190 lines of
README between them. The documentation is not the gap. Four things stop an
outsider from contributing, in the order they hit:

1. **No LICENSE.** The default is all rights reserved. Contribution is legally
   undefined and most people will not open a PR.
2. **No reproducible build.** owl needs libh2o-evloop from h2o master (Ubuntu
   ships 2.2; owl targets 2.3), CMake 4.3 (Ubuntu ships 3.28), GCC 14, and a
   hand-written `libwslay.pc`. Every one of those is a place to give up.
3. **No visible conventions.** The house rules live in `AGENTS.md`, which is
   gitignored. A contributor cannot see the layering rule, the `detail/`
   visibility rule, or the instruction not to make the compile_fail tests
   compile — but a PR violating them gets rejected.
4. **No visible backlog.** `tasks/todo.md` is excluded. Nobody knows what needs
   doing or where the project is going.

This design fixes all four. Outreach — where to announce, how to position owl
against Drogon or oat++ — is deliberately out of scope.

## Decisions

| Decision | Choice | Reason |
|---|---|---|
| License | MIT | Lowest friction, universally recognized, matches Drogon and most C++ web frameworks. No patent grant, which does not matter until corporate contributors appear — and they will not while the README says "do not ship this". |
| Build environment | Prebuilt image on GHCR | The h2o-from-master build dominates CI time *and* is the hardest part of contributing locally. One artifact solves both. |
| Primary platform | Linux | owl is built to be deployed on Linux. macOS is where development happens, not where the framework has to prove itself. |
| End-to-end coverage | Monkey test, phase 1 | Proves owl serves real traffic, not just that unit tests link. Phase 2 (chaos) stays off the PR path. |
| Design docs | Tracked in `docs/` | `/docs/` removed from `.git/info/exclude`. Making the project's direction public serves the contribution goal directly. |

## 1. License

- `LICENSE` at the repository root: MIT, `Copyright (c) 2026 Adkhambek`. The name
  is taken from the git author; substitute a legal name if preferred.
- README gains a `## License` section. It currently has none.

Do this first. All 112 commits are held by one author, so this is a one-file
decision today. After the first outside PR, relicensing needs every
contributor's consent.

## 2. Dev image — `ghcr.io/mradkhambek/owl-dev`

New `docker/dev.Dockerfile`, derived from the build stage of
`examples/rest/Dockerfile` with the application build removed:

- Base `ubuntu:24.04`; `gcc-14`/`g++-14`. Ubuntu's clang 18 cannot build owl —
  it reports `__cpp_concepts` below what libstdc++'s `<expected>` demands, and
  libc++ 18 has no floating-point `from_chars`. GCC 14 is the only working
  Linux compiler, which is why there is no compiler matrix below.
- CMake 4.3 from the Kitware release tarball.
- `libssl-dev zlib1g-dev libuv1-dev libpq-dev libhiredis-dev libwslay-dev`
  **plus `libsqlite3-dev`**. The examples set `OWL_ENABLE_SQLITE=OFF`, so the
  existing Dockerfile omits it; owl's own default is ON, so CI's default job
  needs it.
- The hand-written `/usr/share/pkgconfig/libwslay.pc`, because Ubuntu's
  `libwslay-dev` ships a CMake config rather than a `.pc` file and owl looks it
  up under Homebrew's name.
- libh2o-evloop built from source and installed.

**h2o is pinned to a commit SHA, not `master`.** Tracking master means an
upstream commit can break contributor PRs with no change from this repository.
The weekly rebuild reports divergence; bumping the pin stays a deliberate act.

`.github/workflows/dev-image.yml` builds and pushes on changes to
`docker/dev.Dockerfile`, on a weekly schedule, and on `workflow_dispatch`.
Tags: `latest` and the pinned h2o SHA.

The weekly run does two distinct things. It rebuilds the pinned SHA, which
catches drift in the Ubuntu base image and apt packages underneath a fixed h2o.
It then resolves the current head of h2o master and, if that differs from the
pin, opens or updates a single tracking issue saying so. Divergence becomes
something reported, never something that breaks a pull request.

## 3. CI — `.github/workflows/ci.yml`

Every job runs `container: ghcr.io/mradkhambek/owl-dev`, so no pull request
ever builds h2o.

| Job | Configuration | Tests |
|---|---|---|
| `default` | defaults (`OWL_ENABLE_SQLITE=ON`) | hermetic ctest |
| `full` | prometheus, postgres, redis ON; `services:` postgres + redis; `OWL_TEST_PSQL_DSN` and `OWL_TEST_REDIS` set | full ctest, live tests included |
| `asan` | `full` configuration with sanitizers | hermetic only |
| `e2e` | `examples/rest` via docker compose, on the host runner | `monkey.py --seed 1`, phase 1 |

Notes:

- `e2e` is the one job that does **not** run in the dev image. It needs the
  Docker daemon to bring up compose, and a container job does not have one, so
  it runs directly on `ubuntu-latest`. It stays fast without the dev image
  because `examples/rest/Dockerfile` installs dependencies and builds h2o
  *before* `COPY . /src`: with buildx and the GitHub Actions cache those layers
  are restored rather than rebuilt, and only invalidate when the Dockerfile
  itself changes. The example keeps building owl from source, so it goes on
  serving as the from-scratch build documentation.
- `compile_fail` tests build from ctest via `EXCLUDE_FROM_ALL` targets and work
  in-container unchanged. They must keep failing to compile.
- The `e2e` job runs `monkey.py` **without** `--compose`. Phase 1 holds the
  server to one rule — it may refuse anything, it may not fail — and is seeded
  and reproducible. Phase 2 restarts postgres and redis under load; that
  belongs on a nightly or manual dispatch, not on a required check.
- Known flaky tests (the `when_any` flake, the `loop_scheduler` UAF) are
  excluded by name with `ctest -E`, each with a comment linking a tracking
  issue. A red badge must mean a real break; a permanently-red CI deters
  contribution more than no CI does.
- Triggers: `pull_request`, and `push` to `main`. A `concurrency` group cancels
  superseded runs.
- googletest arrives by `FetchContent` at configure time, so jobs need network.
  They have it.

macOS is a follow-up, not part of this work: there is no prebuilt image for it,
h2o would still need a source build on every run, and Apple clang is already
exercised daily by development.

## 4. CONTRIBUTING.md

Largely a public edition of `AGENTS.md`, which contributors cannot see. Carry
over:

- Build and test commands, and the rule that a new header must be listed in its
  library's `FILE_SET HEADERS`.
- The layering rule: `core/` -> `util/` -> `http/` -> `extract/` -> `routing/`,
  with `coro/loop_scheduler.h` and `server.h` on top. Nothing lower includes
  anything higher.
- `detail/` visibility: only a directory's own public headers, its own tests,
  and the library's own detail wiring may include its `detail/` headers.
- Style: 4-space indent, indent inside namespaces, `const` everything that can
  be, `final` unless designed as a base, system includes before project
  includes, and public-header comments that explain *why*.
- **Do not make the `compile_fail` tests compile.**

Add, because they are house rules not yet written down anywhere public:

- Errors are data: return `std::expected` and `std::unexpected`; do not
  propagate exceptions across a library boundary.
- Commit style, taken from the log: `module: imperative lowercase summary`, no
  trailers or signatures.
- What contributing means under API churn. The README says "Experimental. APIs
  move. Do not ship this" — that stays, it is honest. But it also tells a
  contributor their work may have no user, so CONTRIBUTING states plainly that
  tests, documentation, portability fixes and bug reports are always welcome,
  and that large API proposals should start in an issue or in chat.

Omit the "Workflow Orchestration" and "Task Management" sections of
`AGENTS.md`. Those are agent instructions, not contributor rules.

## 5. Backlog

The unchecked work in `tasks/todo.md` is expert performance work on internals —
per-bucket atomics with prefix sums at dump, a `thread_local` RED cache keyed by
`(method, status, route)`, generation counters. Valuable, and unsuitable as a
first contribution.

The genuine starter issues are already written down, in the "out of scope, by
design" notes of completed work:

- **HEAD -> GET fallback.** Noted as deliberately absent from the CORS work.
- **Preflight to an unregistered path returns a bare 404**, because no chain
  runs on 404.
- **`views::chunk` / `stride` / `enumerate` fallbacks**, missing from Apple
  libc++ 21 and worked around ad hoc in `base64.h`.

Each is self-contained, testable, and has an obvious definition of done.

Deliverables: a public `ROADMAP.md` distilled from `tasks/todo.md` so the
direction is visible, and drafted issue bodies written to a file.

**Issues are not created by this work.** Creating them posts under the
maintainer's GitHub account; that stays a deliberate, manual step.

## 6. Templates

- `.github/ISSUE_TEMPLATE/bug.yml` and `feature.yml`.
- `.github/ISSUE_TEMPLATE/config.yml` with `contact_links` pointing at
  <https://t.me/owl_cxx> and <https://discord.gg/RxdpFxb65j>, so questions land
  in chat rather than the issue tracker.
- `.github/pull_request_template.md`: a short checklist — tests added, `ctest`
  green, README updated if the API changed.
- A README `## Contributing` section linking CONTRIBUTING.md and the two chat
  channels.

## Risks

| Risk | Mitigation |
|---|---|
| The GHCR image is a new thing to maintain | Pin h2o by SHA; weekly rebuild surfaces drift without breaking PRs. |
| First-time contributors cannot pull a private GHCR image | Publish the package as public when the workflow first runs. |
| The `e2e` job flakes on container startup timing | Wait on a health endpoint before starting the monkey run; phase 2 chaos stays off the PR path. |
| Quarantined flakes get forgotten | Each `ctest -E` exclusion carries a comment and a tracking issue. |

## Out of scope

Outreach and positioning; macOS CI; releases, tags and versioning; a code of
conduct; publishing the drafted issues.
