// A controller's Extractors is the tuple its callbacks are unpacked from.
#include <owl/routing/router.h>

struct App final {};

struct BadExtractors final {
    using Extractors = int;
    void on_message(owl::ws::Socket, owl::ws::Message) {}
};

void force() {
    (void)owl::Router<App>::make().ws<"/x", BadExtractors>();
}
