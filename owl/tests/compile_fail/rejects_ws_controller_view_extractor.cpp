// The controller form refuses a view extractor for the same reason the
// coroutine form does: the connection outlives the request it points into.
#include <tuple>

#include <owl/routing/router.h>

struct App final {};

struct HoldsView final {
    using Extractors = std::tuple<owl::PathView<"id">>;
    void on_message(owl::ws::Socket, owl::ws::Message, owl::PathView<"id">) {}
};

void force() {
    (void)owl::Router<App>::make().ws<"/rooms/{id}", HoldsView>();
}
