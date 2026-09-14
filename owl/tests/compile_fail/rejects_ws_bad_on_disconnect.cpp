// on_disconnect takes the Socket: one controller serves every connection
// and must be told which one ended.
#include <owl/routing/router.h>

struct App final {};

struct BadDisconnect final {
    void on_message(owl::ws::Socket, owl::ws::Message) {}
    void on_disconnect() {}
};

void force() {
    (void)owl::Router<App>::make().ws<"/x", BadDisconnect>();
}
