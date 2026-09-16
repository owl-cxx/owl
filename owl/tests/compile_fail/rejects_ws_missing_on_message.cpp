#include <owl/routing/router.h>

struct AppState final {};

struct OnlyConnect final {
    void on_connect(owl::ws::Socket) {}
};

void force() {
    (void)owl::Router<AppState>::make().ws<"/x", OnlyConnect>();
}
