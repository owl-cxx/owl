// A pattern that does not parse is refused where the route is mounted.
#include <owl/routing/router.h>

struct App final {};

owl::Response ping() { return owl::Response::ok("pong"); }

void force() {
    (void)owl::Router<App>::make().route<"/users/{id">(owl::get(ping));
}
