// A handler may only ask for a path parameter its pattern declares; the
// mismatch is a compile error at the route, not an empty value at runtime.
#include <owl/routing/router.h>

struct AppState final {};

owl::Response echo_id(owl::PathView<"id"> id) { return owl::Response::ok(std::string{id.value}); }

void force() {
    (void)owl::Router<AppState>::make().route<"/ping">(owl::get(echo_id));
}
