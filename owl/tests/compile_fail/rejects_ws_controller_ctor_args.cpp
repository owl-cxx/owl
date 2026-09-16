// The constructor-argument form names the mismatch at the route rather
// than inside make_shared.
#include <owl/routing/router.h>

struct AppState final {};

struct Echo final {
    explicit Echo(int n) : n(n) {}
    void on_message(owl::ws::Socket, owl::ws::Message) {}
    int n;
};

void force() {
    (void)owl::Router<AppState>::make().ws<"/x", Echo>("not an int");
}
