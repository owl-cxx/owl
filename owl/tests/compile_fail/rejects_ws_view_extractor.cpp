#include <owl/routing/router.h>

struct AppState final {};

coro::task<void> bad(owl::ws::Socket, owl::PathView<"id">) { co_return; }

void force() {
    (void)owl::Router<AppState>::make().ws<"/x/{id}">(bad);
}
