# Contributing to owl

owl is early. APIs move between commits and there are no tagged releases yet,
so the most useful contributions are the ones that survive churn: tests, bug
reports, documentation, and portability fixes. Those are always welcome, no
discussion needed.

Changes that reshape a public API are welcome too, but open an issue or ask in
[Telegram](https://t.me/owl_cxx) or [Discord](https://discord.gg/RxdpFxb65j)
first. The layering rules below are load-bearing, and a large patch that cuts
across them is painful to revise after the fact.

## Where to start

Issues tagged [good first issue](https://github.com/MrAdkhambek/owl/labels/good%20first%20issue)
are self-contained and have a clear definition of done. If nothing there
appeals, adding a test for an untested path is always worth doing.

## Build

C++23, CMake **4.3**, and a compiler with working `<expected>` and
floating-point `from_chars`. On Linux that means **GCC 14**: Ubuntu's clang 18
reports `__cpp_concepts` below what libstdc++'s `<expected>` demands, and
libc++ 18 has no floating-point `from_chars`. On macOS, Apple clang works.

`owl::owl` needs libh2o-evloop, OpenSSL, and zlib. Optional drivers add
sqlite3 (on by default), libpq, and hiredis. **libh2o-evloop must be built
from h2o source with `H2O_USE_LIBUV=0`** — distribution packages ship h2o 2.2
and owl targets 2.3, so this is the one dependency you cannot install from a
package manager.

[`examples/rest/Dockerfile`](examples/rest/Dockerfile) is the known-good,
executable recipe for a Linux environment: the apt package list, the CMake 4.3
tarball, the h2o build, and the hand-written `libwslay.pc` that Ubuntu's
`libwslay-dev` omits. When in doubt, read that file — it is kept working.

On macOS, `brew install openssl@3 sqlite libpq hiredis` covers everything
except h2o, which still needs the source build above.

Then:

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build --output-on-failure
```

The optional libraries are off unless asked for:

```sh
cmake -S . -B build -G Ninja \
    -DOWL_ENABLE_PROMETHEUS=ON \
    -DOWL_ENABLE_POSTGRESQL=ON \
    -DOWL_ENABLE_REDIS=ON
```

googletest is fetched at configure time, so the first configure needs network.

## Tests

Every header gets tests, and a bug fix starts with a test that fails.

One test at a time:

```sh
ctest --test-dir build -R test_router --output-on-failure
```

Some tests need a live service and skip themselves without one:

| Variable | Used by |
|---|---|
| `OWL_TEST_PSQL_DSN` | `sql/tests/psql/test_psql.cpp` |
| `OWL_TEST_REDIS` | `redis/tests/live/test_live.cpp` |

```sh
OWL_TEST_PSQL_DSN=postgres://localhost/owl_test ctest --test-dir build -R sql_ --output-on-failure
OWL_TEST_REDIS=127.0.0.1:6379 ctest --test-dir build -R redis_ --output-on-failure
```

**`compile_fail/` tests are supposed to fail to compile.** Those translation
units are `EXCLUDE_FROM_ALL` and ctest builds them with `WILL_FAIL TRUE` —
they pin misuse that the type system must reject, such as a handler naming
`State<T>` for the wrong `T`, or an `fstr` built from a runtime buffer. If one
of them starts compiling, that is the bug. Do not make them compile.

## Layout

Each library lives in `include/<name>/`, so the prefix is part of the include.
Public headers are `#pragma once`.

`owl/include/owl/` is layered bottom-up:

```
core/  ->  util/  ->  http/  ->  extract/  ->  routing/
```

with `coro/loop_scheduler.h` and `server.h` on top. **Nothing lower includes
anything higher.** `owl.h` is the umbrella and includes every public header;
`owl/tests/test_owl.cpp` guards that, so a new public header must be added
there too.

Internal-only headers live in a `detail/` subdirectory of their own directory.
Only that directory's own public headers, its own tests, and the library's own
detail wiring may include them. Consumer code never includes a `detail/`
header, and a `detail/` header never widens its layer's public API.

**A new header must be listed in its library's `CMakeLists.txt` under
`target_sources` `FILE_SET HEADERS`.** That file set is the only header list —
do not pass sources to `add_library`. `detail/` headers are listed like any
other; they are simply not part of the documented API.

## Style

Four-space indent, and indent inside namespaces.

- Prefer a C++23 facility as soon as it applies — `std::expected`, ranges,
  `if consteval`, `std::to_underlying`, `std::unreachable`. Do not write a
  C++17/20 workaround for something C++23 provides.
- `const` everything that can be: locals, parameters, members, and methods,
  with `constexpr` or `consteval` where they fit.
- Mark `class` and `struct` `final` unless it is designed as a base.
- Includes: system headers first, one blank line, then project headers.
- Public headers carry comments, and they explain **why** — the invariant, the
  alternative rejected, the trap avoided. Do not narrate what the code already
  says. Test sources stay uncommented except where the point is non-obvious.

```cpp
#pragma once

#include <chrono>

#include "coro/concepts/scheduler.h"
```

### Errors are data

A fallible operation returns `std::expected` and fails with
`std::unexpected`. Check the status and return the error; do not route it
through an `exception_ptr` or convert it back and forth. `try`/`catch` is not
banned — it is the right tool at a boundary that genuinely throws — but an
error must not cross a library's public API as an exception.

## Commits and pull requests

Commit messages are `module: imperative lowercase summary`:

```
redis: drop unused includes from args.h and client.h
owl: benches listen on 0.0.0.0 so a docker drop can be hit remotely
```

No trailers or signatures. Keep a commit to one change; a series of small
commits reviews far better than one large one.

Before opening a pull request:

- [ ] Tests added or updated, and a bug fix has a test that failed before it
- [ ] Any new header is in `FILE_SET HEADERS` and in `owl.h` if it is public
- [ ] Full `ctest` passes locally
- [ ] README updated if the public API changed
