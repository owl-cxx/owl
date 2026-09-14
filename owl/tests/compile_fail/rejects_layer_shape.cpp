// A layer is one of two shapes; anything else is named as such rather than
// failing somewhere inside the chain.
#include <owl/routing/router.h>

struct App final {};

void force() {
    (void)owl::Router<App>::make().layer([](int) { return 0; });
}
