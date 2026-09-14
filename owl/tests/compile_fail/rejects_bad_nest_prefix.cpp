// A nest prefix is a pattern too: it must start with '/'.
#include <owl/routing/router.h>

struct App final {};

void force() {
    (void)owl::Router<App>::make().nest<"api">(owl::Router<App>::make());
}
