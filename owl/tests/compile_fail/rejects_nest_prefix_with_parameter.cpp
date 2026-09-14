// A nest prefix is literal; a parameter belongs in the nested routes,
// where a handler can declare and extract it.
#include <owl/routing/router.h>

struct App final {};

void force() {
    (void)owl::Router<App>::make().nest<"/{tenant}">(owl::Router<App>::make());
}
