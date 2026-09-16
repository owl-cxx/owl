#include <owl/owl.h>

namespace {
    struct AppState final {
        int n = 0;
    };
}

static_assert(sizeof(owl::Server<AppState>) >= 1);
