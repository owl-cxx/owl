// A nest prefix is a pattern too: it must start with '/'.
#include <owl/routing/router.h>

struct AppState final {};

void force() {
    (void)owl::Router<AppState>::make().nest<"api">(owl::Router<AppState>::make());
}
