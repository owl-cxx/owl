// A controller's Extractors are checked against the pattern like a
// handler's parameters -- and before on_message, so the pattern is what
// gets blamed.
#include <tuple>

#include <owl/routing/router.h>

struct App final {};

struct NeedsId final {
    using Extractors = std::tuple<owl::Path<"id", int>>;
    void on_message(owl::ws::Socket, owl::ws::Message, owl::Path<"id", int>) {}
};

void force() {
    (void)owl::Router<App>::make().ws<"/rooms", NeedsId>();
}
