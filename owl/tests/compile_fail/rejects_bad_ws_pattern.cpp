// The WebSocket form parses its pattern the same way the HTTP form does.
#include <owl/routing/router.h>

struct AppState final {};

coro::task<void> chat(owl::ws::Socket) { co_return; }

void force() {
    (void)owl::Router<AppState>::make().ws<"/rooms/{id">(chat);
}
