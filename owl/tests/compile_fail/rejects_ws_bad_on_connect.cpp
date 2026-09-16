// An on_connect that exists but does not match would be a silently dead
// callback, which is worse than a missing one.
#include <owl/routing/router.h>

struct AppState final {};

struct BadConnect final {
    void on_message(owl::ws::Socket, owl::ws::Message) {}
    void on_connect(int) {}
};

void force() {
    (void)owl::Router<AppState>::make().ws<"/x", BadConnect>();
}
