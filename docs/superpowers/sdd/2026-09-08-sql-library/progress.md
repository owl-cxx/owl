# SDD ledger — plan: docs/superpowers/plans/2026-09-08-sql-library.md

Pre-history (before this ledger existed):
- Task 1: complete (commit 8ab56bf, "sql: build scaffolding and sql::error; owl decoupled from the old library") — predates ledger; no SDD review was run on it. Accepted as-is: user committed it themselves.
- Task 2: in progress in working tree at ledger creation — convert.h and test_convert.cpp exist and match plan text verbatim; registration state unverified at ledger creation.

## Preflight scan

Interface handoffs (produces -> consumes):

| # | Pair | Handoff | Check |
|---|------|---------|-------|
| 1 | T1->T2 | error/error_kind -> convert.h | on-disk convert.h includes "sql/error.h" -- ok |
| 2 | T2->T5,T9,T10 | bindable/to_text/from_text -> fake driver, sqlite, psql | driver execute<Q> signature constrains Args on bindable -- ok |
| 3 | T3->T7,T9,T10 | scan_placeholders/stmt_key -> query, drivers | detail/ excluded from umbrella by guard (sql/CMakeLists.txt:94), still FILE_SET via tree guard -- ok |
| 4 | T4->T9,T10,T11 | reactor_ref/scheduler_ref -> drivers, Context | coro APIs verified to exist: io_reactor, postable (concepts/scheduler.h:26), interest + wait_status (concepts/io_reactor.h:18,24) -- ok |
| 5 | T5->T6,T7,T8 | driver concept + fake driver -> pool, query, transaction | pool(config, io) matches open(config, io) -- ok |
| 6 | T6->T7,T8,T11 | pool/lease/source -> query, transaction, owl wiring | source concept = checkout(); lease/pool friendship private give_back -- ok |
| 7 | T7,T8->CMake | 6 compile-fail tests | sql_add_compile_fail_test exists from T1 (sql/CMakeLists.txt:161) -- ok |
| 8 | T9,T10->T11 | drivers -> Context/Builder/extractors | T11 gated on OWL_ENABLE_POSTGRESQL/OWL_ENABLE_SQLITE -- ok |
| 9 | T12->all | docs + sanitizer sweep | sweep runs full suite Debug/Release/ASan/TSan |

Self-consistency: T2 "Expected: PASS, 9 tests" = Bindable(2) + ToText(2) + FromText(5) = 9 -- ok.
T2 on-disk convert.h verified verbatim against plan Step 3; test_convert.cpp and registration verified by implementer before commit.

Rulings:
- Ruling: sql.h include order is alphabetical (convert.h before error.h, as on disk now). T2's "add after error.h" phrasing yields to T5's explicit alphabetical list ("concepts.h, convert.h, error.h, io.h"); the umbrella guard checks set equality, not order. Cost if wrong: cosmetic include order only.
- Ruling: Task 2 was found complete-but-uncommitted on disk at session start; the implementer verifies against the brief and commits rather than rewriting from scratch. Cost if wrong: a drift between disk and plan slips through -- mitigated by the task reviewer diffing the commit against the brief.

## Progress

Task 2: complete (commits 8ab56bf..61e5e31, review clean)
- Reviewer ⚠️ trailer-verification resolved by controller: `git log -1 61e5e31` shows both trailers verbatim.
- Task 2: minor (deferred): `from_text<std::string_view>` lifetime contract undocumented (convert.h:168; plan-mandated code, one-sentence comment would help). Final review to triage.

Task 3: complete (commits 61e5e31..60e0b85, review clean)
- Ruling: FILE_SET entries added together with the headers, not before the RED run — the tree/manifest guard FATAL_ERRORs at configure on FILE_SET entries for nonexistent files, making the brief's literal step order unachievable; RED was observed via the header-not-found failure of the test target. Cost if wrong: none — end state identical to the brief.
- Reviewer ⚠️ trailer-verification resolved by controller: trailers verbatim on 60e0b85.
- Task 3: minor (deferred): `$05` leading-zero acceptance undocumented/untested (placeholders.h:42; matches Postgres; one-line comment or test would pin it).
- Task 3: minor (deferred): `$64` valid boundary untested (only `$65` rejection side is).
- Task 3: minor (deferred): hole-error return carries `.count = max` alongside `.error` (placeholders.h:55-56; brief-mandated; callers must check error first).

Task 4: complete (commits 60e0b85..fc1e173, review clean)
- Ruling (same as Task 3, now standing): FILE_SET entry + umbrella include go in the same edit as the new header; the brief's literal step order is unachievable (tree guard FATAL_ERRORs at configure). Cost if wrong: none.
- Reviewer ⚠️ trailer-verification resolved by controller: trailers verbatim on fc1e173.
- Task 4: minor (deferred): null `scheduler_ref::post()` inline-resume branch (io.h:143) untested; `NullScheduleIsReadyAndPostResumesInline` name promises more than it delivers. Brief-authored gap; final review to triage.

Task 5: complete (commits fc1e173..76eb402, 1 parked, review otherwise clean)
- Task 5: parked — plan-mandated Important: `driver` concept looser than the documented interface (abandon return type unpinned, execute probe args-less, ok() const-ness unenforced; concepts.h:63-69) — Ruling: keep as spec'd. The spec (binding authority) writes exactly these probes at docs/superpowers/specs/2026-09-08-sql-library-design.md:341-342; the plan transcribes it. Downstream call sites in pool/query/transaction invoke `execute<Q>(args...)` and rely on void-abandon semantics, so a mis-shaped driver fails to compile in Tasks 6-8's tests — the concept is a fast-fail filter, not the last line of defense. Cost if wrong: an external driver author gets a worse error message at a use site instead of the concept; no runtime risk.
- Task 5: minor (deferred): test_concepts.cpp relies on transitive `<sql/convert.h>` via fake_driver.h for `sql::bindable`.
- Task 5: minor (deferred): fake's `suspend_open`, `opened`, and `abandon()` behavior not exercised by Task 5's tests (expected consumers: Tasks 6-8 tests).

Task 6: complete (commits 76eb402..bbb9373, review clean)
- Reviewer ⚠️ trailer-verification resolved by controller: trailers verbatim on bbb9373.
- Task 6: minor (deferred): abandoning a parked checkout coroutine (dropping it without resume) dangles its waiter node in the pool queue; header contract comment covers only the outstanding-lease case (pool.h:19-22). Inherent to the coro model; one-line note would make the boundary explicit.
- Task 6: minor (deferred): `park::await_suspend` missing `noexcept` its sibling awaiters carry (pool.h:197; brief-verbatim).
- Task 6: minor (deferred): header comment "returns nothing" overpromises for `operator*` on an empty lease (UB), exact only for `operator->` (pool.h:44-45).

Task 7: complete (commits bbb9373..d143a08, review clean)
- Reviewer ⚠️ trailer-verification resolved by controller: trailers verbatim on d143a08.
- Task 7: minor (deferred): query.h names `source`/`bindable` relying on pool.h's transitive includes (brief-prescribed include list; break would surface if pool.h's includes change).
- Task 7: minor (deferred): pre-existing CMake dev warning for deprecated `SQLite::SQLite3` target (sql/CMakeLists.txt:58) — outside any task diff; final review to triage.
- Controller note: the parked Task 5 ruling's enforcement point (execute<Q>(args...) invoked with forwarded args through the lease, query.h:41) was verified by the reviewer — ruling holds.

Task 8: complete (commits d143a08..e477f5b, review clean)
- Reviewer ⚠️ trailer-verification resolved by controller: trailers verbatim on e477f5b.
- Ruling (standing, Tasks 3-8): FILE_SET entry + umbrella include go in the same edit as the new header; RED may surface as the configure-time manifest guard rather than a compiler error — same root cause.
- Task 8: minor (deferred): COMMIT failure returns the connection to the pool without abandon (transaction.h:125-127; brief-verbatim; defensible since the pool's next use catches a dead connection). Final review to weigh deliberately.

