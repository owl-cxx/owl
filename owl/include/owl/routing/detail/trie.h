#pragma once

// The route trie
//
// Each node owns its literal children (sorted, binary searched) plus at
// most one parameter child. Matching walks segments and backtracks, trying
// literals before the parameter -- so /users/new beats /users/{id} without
// depending on registration order, which a linear scan cannot promise.
//
// Parameter names are owned by the nodes, so the views handed to Request
// stay valid for the router's lifetime.

#include <algorithm>
#include <array>
#include <cstddef>
#include <memory>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "owl/core/method.h"
#include "owl/http/request.h"
#include "owl/routing/detail/pattern.h"
#include "owl/routing/middleware.h"

namespace owl {
    // Chains attach at the root and at one node per descended segment, so a
    // matched path can never collect more of them than this. The cap being
    // provable is what keeps collection allocation-free with no silent drops.
    inline constexpr std::size_t max_middleware_chains = max_path_segments + 1;
}

namespace owl::detail {
    template <typename S>
    struct RouteNode {
        std::string label; // this node's literal segment
        std::string param_name; // set on a parameter child
        std::vector<std::unique_ptr<RouteNode>> literals; // sorted by label
        std::unique_ptr<RouteNode> param;
        std::vector<std::pair<Method, Handler<S>>> handlers;
        // Parallel to handlers so GET and an Upgrade handshake can share a
        // path. Empty means none -- WS is not an HTTP method and does not
        // live in handlers.
        Handler<S> websocket;
        // Answers OPTIONS on a node that registered methods but not that
        // one: 204 with Allow, which is what OPTIONS asks (RFC 9110 §9.3.7).
        // It exists so a CORS preflight reaches the layers instead of the
        // 405 that dispatch sends before any chain runs. Rebuilt by the
        // router on every registration at this node, so Allow is final by
        // the time a request arrives and matching pays nothing for it.
        Handler<S> options_fallback;
        // Registered template for this node: the full path, prefix included.
        std::string pattern;
        MiddlewareChain<S> middleware;

        // The Upgrade slot when the request asked for it and the node has
        // one; the method's handler otherwise; for OPTIONS, the fallback
        // when nothing was registered.
        [[nodiscard]] const Handler<S>* handler_for(const Method method, const bool upgrade = false) const noexcept {
            if (upgrade && websocket) return &websocket;
            for (const auto& [key, handler] : handlers) {
                if (key == method) return &handler;
            }
            if (method == Method::Options && options_fallback) return &options_fallback;
            return nullptr;
        }

        // What was registered, fallback excluded: the duplicate check
        // must not read the OPTIONS the node answers on its own as a
        // second registration.
        [[nodiscard]] bool registered(const Method method) const noexcept {
            return std::ranges::contains(handlers | std::views::keys, method);
        }

        // Where text sorts among the literal children: the child with that
        // label when there is one, else where it would go.
        [[nodiscard]] auto literal_pos(const std::string_view text) const noexcept {
            return std::ranges::lower_bound(literals, text, {}, [](const auto& node) {
                return std::string_view{node->label};
            });
        }

        [[nodiscard]] RouteNode* find_literal(const std::string_view text) const noexcept {
            const auto pos = literal_pos(text);
            return pos != literals.end() && (*pos)->label == text ? pos->get() : nullptr;
        }

        RouteNode& literal_child(const std::string_view text) {
            if (RouteNode* const found = find_literal(text)) return *found;
            auto child = std::make_unique<RouteNode>();
            child->label = std::string(text);
            return **literals.insert(literal_pos(text), std::move(child));
        }
    };

    // The middleware a request collects while matching: pointers to the
    // chains of every group root on the matched path, outermost first.
    // The chains themselves live in the trie, so the pointers outlive any
    // request handed them; the array lives in whoever asked for the match.
    // An empty chain is no chain: push and splice both drop one, so no
    // caller has to ask first.
    //
    // Slot 0 is left empty for the server-wide chain, which belongs to the
    // Server rather than the trie and is spliced in at dispatch. Reserving
    // it here is what lets the driver hand `Next` a view straight into
    // this array: it used to copy the whole thing into a second array in
    // its coroutine frame, costing 272 bytes of frame plus the memset that
    // zeroed them, on every request.
    template <typename S>
    struct MatchedChains {
        // Deliberately not zero-initialised: only [1, 1 + count) is ever
        // read, and count is set before any read. Value-initialising this
        // was ~272 bytes of memset per request for an array that is
        // usually empty.
        std::array<const MiddlewareChain<S>*, max_middleware_chains + 1> chains;
        std::size_t count = 0; // entries live at [1, 1 + count)

        void push(const MiddlewareChain<S>* chain) noexcept {
            if (chain->empty()) return;
            chains[1 + count++] = chain;
        }

        [[nodiscard]] ChainSpan<S> splice(const MiddlewareChain<S>* front) noexcept {
            if (front == nullptr || front->empty()) return {.data = chains.data() + 1, .count = count};
            chains[0] = front;
            return {.data = chains.data(), .count = count + 1};
        }
    };

    // One request's walk down the trie: the segments its path split into,
    // what it asked for, and the two things a match fills in as it goes --
    // the Request's parameters and the chains passed on the way. Both are
    // written in place and cut back on backtrack, so a branch that fails
    // leaves nothing behind for the next one tried.
    template <typename S>
    struct Walk {
        const std::string_view* segments;
        std::size_t count;
        Method method;
        bool websocket;
        Request& req;
        MatchedChains<S>& chains;

        [[nodiscard]] const Handler<S>* descend(const RouteNode<S>& node, const std::size_t index) {
            if (index == count) {
                const Handler<S>* const handler = node.handler_for(method, websocket);
                if (handler != nullptr) req.set_route_pattern(node.pattern);
                return handler;
            }
            if (const RouteNode<S>* child = node.find_literal(segments[index])) {
                if (const Handler<S>* handler = enter(*child, index)) return handler;
            }
            if (node.param) return enter(*node.param, index);
            return nullptr;
        }

        // Steps into child for segments[index], undoing what the step
        // recorded if nothing below it matches. A parameter child is the
        // one with a name; the capture cap is add_param's.
        [[nodiscard]] const Handler<S>* enter(const RouteNode<S>& child, const std::size_t index) {
            const std::size_t params = req.param_count();
            const std::size_t pushed = chains.count;
            chains.push(&child.middleware);
            if (!child.param_name.empty()) req.add_param(child.param_name, segments[index]);
            if (const Handler<S>* handler = descend(child, index + 1)) return handler;
            req.truncate_params(params);
            chains.count = pushed;
            return nullptr;
        }
    };

    // Collects every method reachable at this path. Unlike a Walk it does
    // not stop at the first hit and does not backtrack away from a
    // subtree: a request can route through either the literal child or the
    // parameter child, so Allow is the union of both.
    template <typename S>
    void collect_methods(const RouteNode<S>& node, const std::string_view* segments,
                         const std::size_t count, const std::size_t index,
                         MethodSet& out) noexcept {
        if (index == count) {
            for (const auto& method : node.handlers | std::views::keys) out.add(method);
            // Served by the fallback whenever anything else is.
            if (!node.handlers.empty()) out.add(Method::Options);
            return;
        }
        if (const RouteNode<S>* child = node.find_literal(segments[index])) {
            collect_methods(*child, segments, count, index + 1, out);
        }
        if (node.param) {
            collect_methods(*node.param, segments, count, index + 1, out);
        }
    }
}
