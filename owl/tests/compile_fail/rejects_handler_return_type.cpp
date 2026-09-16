// A handler returns Response or coro::task<Response>; anything else has no
// way to be sent.
#include <owl/routing/router.h>

struct AppState final {};

int not_a_handler() { return 0; }

void force() {
    (void)owl::Router<AppState>::make().route<"/x">(owl::get(not_a_handler));
}
