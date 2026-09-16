# Roadmap

What owl is working towards, and what is deliberately not being worked on.
owl is experimental — see [Stability](README.md#stability). Nothing here is a
commitment to a date, and the ordering changes.

Per-library detail lives with the library: [`owl`](owl/README.md#roadmap) has
its own roadmap table for the web framework.

## In progress

**Prometheus request-path cost.** Recording a request costs about 390 ns on one
thread, and four threads are slower in aggregate than one. Three causes: two
allocating lookups plus name validation per request; libc++'s `atomic_load` on
`shared_ptr` taking an internal lock; and the histogram doing up to 14
read-modify-writes per observation. The work is per-bucket atomics with prefix
sums at dump time, a bounded `shared_ptr` per family, a generation counter, and
a `thread_local` RED cache keyed by `(method, status, route)`.

Alongside it, some API gaps: custom histogram buckets, labeled gauges,
`describe()` setting `HELP`, and labelling unmatched routes as `unmatched`
rather than dropping them.

## Next

| | |
|---|---|
| **Static files** | A route that sendfiles a directory. Range requests after that. |
| **TLS** | HTTPS as a `Server::Builder` switch; h2o already links OpenSSL. |
| **Graceful shutdown** | Stop listeners, drain in-flight handlers, then join workers. |
| **WebSocket backpressure** | `send` currently has none. Controller extractors also still need a declared `using Extractors` tuple rather than deduction from `on_message`. |

## Good first issues

Self-contained, with a clear definition of done. Each was noted as
deliberately out of scope when the surrounding work shipped, so the shape of
the gap is already understood.

- **`HEAD` → `GET` fallback.** A `HEAD` request to a route with only a `GET`
  handler is not served. Left out of the OPTIONS fallback work on purpose.
- **Preflight to an unregistered path is a bare 404.** No layer chain runs on
  a 404, so a CORS preflight to a path that does not exist gets no CORS
  headers. Whether that is worth changing is part of the issue.
- **Ranges fallbacks for Apple libc++ 21.** It has no `views::chunk`,
  `views::stride`, or `views::enumerate`. `util/base64.h` works around their
  absence by hand; the workaround is not shared, and the next place that wants
  one will repeat it.

## Infrastructure

- [x] MIT license, contributing guide
- [ ] CI on Linux: default options, all libraries with live postgres and
      redis, ASan, and an end-to-end monkey test against `examples/rest`
- [ ] A prebuilt dev image so contributors do not have to build libh2o-evloop
      by hand
- [ ] macOS CI. Deferred: there is no prebuilt image for it and h2o would need
      a source build on every run.
- [ ] Tagged releases. Not until the API stops moving.

## Not planned

- **A C++20 fallback.** owl leans on `std::expected`, `if consteval`, and
  structural NTTPs. Back-porting would cost more than it returns.
- **libuv.** owl targets libh2o-evloop specifically; the event loop is not an
  abstraction owl tries to hide.
