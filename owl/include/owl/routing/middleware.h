#pragma once

// Next returns the handler's Response so middleware can add headers on the
// way out or skip next() and kick. A framed Response<F> could not be that
// type: one chain wraps mixed routes.

#include <cstddef>
#include <functional>
#include <vector>

#include <coro/task.h>

#include "owl/core/state.h"
#include "owl/http/request.h"
#include "owl/http/response.h"

namespace owl {
    template <typename S>
    class Next;

    template <typename S>
    using Middleware = std::function<coro::task<Response>(const Request&, const Context<S>&, Next<S>)>;

    template <typename S>
    using MiddlewareChain = std::vector<Middleware<S>>;

    template <typename S>
    using Handler = std::function<coro::task<Response>(const Request&, const Context<S>&)>;

    namespace detail {
        // The chains a request runs through, outermost first: a view into
        // whoever collected them, which outlives any Next walking it.
        template <typename S>
        struct ChainSpan {
            const MiddlewareChain<S>* const * data;
            std::size_t count;
        };
    }

    template <typename S>
    class Next final {
    public:
        Next(const detail::ChainSpan<S> chains, const Handler<S>* handler, const Context<S>* ctx) noexcept
            : chains_(chains),
              handler_(handler),
              ctx_(ctx) {
        }

        [[nodiscard]] coro::task<Response> operator()(const Request& req) const {
            std::size_t chain = chain_index_;
            std::size_t item = item_index_;
            while (chain < chains_.count && item >= chains_.data[chain]->size()) {
                ++chain;
                item = 0;
            }
            if (chain >= chains_.count) return (*handler_)(req, *ctx_);
            // The continuation is this, one item further along.
            Next rest = *this;
            rest.chain_index_ = chain;
            rest.item_index_ = item + 1;
            return (*chains_.data[chain])[item](req, *ctx_, rest);
        }

    private:
        detail::ChainSpan<S> chains_;
        std::size_t chain_index_ = 0;
        std::size_t item_index_ = 0;
        const Handler<S>* handler_;
        const Context<S>* ctx_;
    };
}
