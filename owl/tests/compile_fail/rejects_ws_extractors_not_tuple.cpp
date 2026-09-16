// A controller's Extractors is the tuple its callbacks are unpacked from.
#include <owl/routing/router.h>

struct AppState final {};

struct BadExtractors final {
    using Extractors = int;
    void on_message(owl::ws::Socket, owl::ws::Message) {}
};

void force() {
    (void)owl::Router<AppState>::make().ws<"/x", BadExtractors>();
}
