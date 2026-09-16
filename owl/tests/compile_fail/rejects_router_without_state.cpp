// There is no build(): the state is part of the server's type, so a server
// is finished only with build_with(state).
#include <owl/server.h>

namespace {
    struct AppState final {
        int n = 0;
    };

    owl::Response ping() {
        return owl::Response::ok("pong");
    }
}

int main() {
    owl::Server<AppState> server = owl::Server<AppState>::builder()
                         .router(owl::Router<AppState>::make().route<"/ping">(owl::get(ping)))
                         .config({})
                         .build();
    (void)server;
}
