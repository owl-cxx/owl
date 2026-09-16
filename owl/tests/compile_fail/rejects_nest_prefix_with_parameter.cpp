// A nest prefix is literal; a parameter belongs in the nested routes,
// where a handler can declare and extract it.
#include <owl/routing/router.h>

struct AppState final {};

void force() {
    (void)owl::Router<AppState>::make().nest<"/{tenant}">(owl::Router<AppState>::make());
}
