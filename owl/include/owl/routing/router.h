#pragma once

#include <array>
#include <cstddef>
#include <format>
#include <memory>
#include <ranges>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>

#include <coro/task.h>

#include <fstr/fstr.h>

#include "owl/core/method.h"
#include "owl/core/state.h"
#include "owl/extract/from_context.h"
#include "owl/http/request.h"
#include "owl/http/response.h"
#include "owl/routing/detail/endpoint.h"
#include "owl/routing/detail/layer.h"
#include "owl/routing/detail/pattern.h"
#include "owl/routing/detail/traits.h"
#include "owl/routing/detail/trie.h"
#include "owl/ws/controller.h"
#include "owl/ws/detail/handshake.h"
#include "owl/ws/detail/routes.h"
#include "owl/ws/detail/views.h"
#include "middleware.h"

namespace owl {
    // ========================================================================
    // MethodRouter: the set of methods served at one path.
    //
    //     get(list_users).post(create_user)
    //
    // Handler signatures are carried in the type rather than erased on the
    // spot, because the path parameters a handler asks for can only be checked
    // once route<Pattern>() sees the pattern and the handlers together.
    // ========================================================================

    template <typename... Endpoints>
    class MethodRouter {
    public:
        constexpr explicit MethodRouter(Endpoints... endpoints) : endpoints_(endpoints...) {
        }

        template <typename R, typename... Args>
        [[nodiscard]] constexpr auto get(R (*handler)(Args...)) const {
            return with<Method::Get>(handler);
        }

        template <typename R, typename... Args>
        [[nodiscard]] constexpr auto post(R (*handler)(Args...)) const {
            return with<Method::Post>(handler);
        }

        template <typename R, typename... Args>
        [[nodiscard]] constexpr auto put(R (*handler)(Args...)) const {
            return with<Method::Put>(handler);
        }

        template <typename R, typename... Args>
        [[nodiscard]] constexpr auto del(R (*handler)(Args...)) const {
            return with<Method::Delete>(handler);
        }

        template <typename R, typename... Args>
        [[nodiscard]] constexpr auto patch(R (*handler)(Args...)) const {
            return with<Method::Patch>(handler);
        }

        template <typename R, typename... Args>
        [[nodiscard]] constexpr auto head(R (*handler)(Args...)) const {
            return with<Method::Head>(handler);
        }

        template <typename R, typename... Args>
        [[nodiscard]] constexpr auto options(R (*handler)(Args...)) const {
            return with<Method::Options>(handler);
        }

        [[nodiscard]] constexpr const std::tuple<Endpoints...>& endpoints() const noexcept {
            return endpoints_;
        }

    private:
        template <Method M, typename R, typename... Args>
        [[nodiscard]] constexpr auto with(R (*handler)(Args...)) const {
            using Added = detail::Endpoint<M, R, Args...>;
            return std::apply(
                [handler](const Endpoints&... existing) {
                    return MethodRouter<Endpoints..., Added>(existing..., Added{handler});
                },
                endpoints_);
        }

        std::tuple<Endpoints...> endpoints_;
    };

    // The first method at a path; the rest chain on as members.
    template <typename R, typename... Args>
    [[nodiscard]] constexpr auto get(R (*handler)(Args...)) {
        return MethodRouter<>{}.get(handler);
    }

    template <typename R, typename... Args>
    [[nodiscard]] constexpr auto post(R (*handler)(Args...)) {
        return MethodRouter<>{}.post(handler);
    }

    template <typename R, typename... Args>
    [[nodiscard]] constexpr auto put(R (*handler)(Args...)) {
        return MethodRouter<>{}.put(handler);
    }

    template <typename R, typename... Args>
    [[nodiscard]] constexpr auto del(R (*handler)(Args...)) {
        return MethodRouter<>{}.del(handler);
    }

    template <typename R, typename... Args>
    [[nodiscard]] constexpr auto patch(R (*handler)(Args...)) {
        return MethodRouter<>{}.patch(handler);
    }

    // Adapts whatever shape the layer was written in to the stored Middleware
    // signature. A layer that wants state takes const Context<S>&, which the
    // chain already carries; there is no second injection path. The adapter
    // only reshapes the call: the layer's own task is handed back as it is,
    // so no second frame sits between the chain and the layer.
    template <typename S, typename F>
    Middleware<S> wrap_layer(F fn) {
        if constexpr (detail::ContextLayer<F, S>) {
            return [fn = std::move(fn)](const Request& req, const Context<S>& ctx, Next<S> next) -> coro::task<Response> {
                return fn(req, ctx, std::move(next));
            };
        } else if constexpr (detail::PlainLayer<F, S>) {
            return [fn = std::move(fn)](const Request& req, const Context<S>&, Next<S> next) -> coro::task<Response> {
                return fn(req, std::move(next));
            };
        } else {
            static_assert(false,
                          "layer expects task<Response>(const Request&, const Context<S>&, Next) "
                          "or task<Response>(const Request&, Next)");
        }

        std::unreachable();
    }

    namespace detail {
        // The two handler outcomes that are not already a task: a kick from
        // extraction, and a synchronous Response. Each is one frame, paid
        // only on the path that needs it.
        [[nodiscard]] inline coro::task<Response> kicked(KickToken kick) {
            co_return to_response(std::move(kick));
        }

        [[nodiscard]] inline coro::task<Response> ready(Response response) {
            co_return std::move(response);
        }
    }

    template <typename S>
    class Router {
    public:
        [[nodiscard]] static Router make() {
            return Router{};
        }

        Router(const Router&) = delete;
        Router& operator=(const Router&) = delete;
        Router(Router&&) noexcept = default;
        Router& operator=(Router&&) noexcept = default;
        ~Router() = default;

        template <fstr::fstr Pattern, typename... Endpoints>
        [[nodiscard]] Router route(const MethodRouter<Endpoints...>& methods) && {
            std::apply([this](const Endpoints&... endpoint) {
                (mount<Pattern>(endpoint), ...);
            }, methods.endpoints());
            return std::move(*this);
        }

        // A coroutine on a parallel slot so GET and Upgrade can share a path.
        template <fstr::fstr Pattern, typename... Args>
        [[nodiscard]] Router ws(coro::task<void> (*handler)(ws::Socket, Args...)) && {
            ws_checks<Pattern, Args...>();
            mount_ws<Pattern, Args...>(handler);
            return std::move(*this);
        }

        // Controller form. Adopts an instance the caller built -- and, if
        // it keeps the shared_ptr, can still reach. That is the difference
        // from the overload below: there the controller is only reachable
        // from inside a connection.
        //
        // One instance serves every connection on the route. Extractors,
        // if the controller declares any, are the part that is *not*
        // shared: they are pulled per request below.
        template <fstr::fstr Pattern, typename C>
        [[nodiscard]] Router ws(std::shared_ptr<C> instance) && {
            static_assert(ws::detail::is_extractor_tuple_v<ws::detail::extractors_of_t<C>>,
                          "a controller's Extractors must be a std::tuple<...> of extractors");

            if (instance == nullptr) {
                // Every connection on this route would dereference it.
                throw std::invalid_argument("owl::Router: ws controller instance is null");
            }

            register_controller<Pattern, C>(std::move(instance), std::type_identity<ws::detail::extractors_of_t<C>>{});
            return std::move(*this);
        }

        // Constructor arguments, forwarded once. A lone shared_ptr<C> goes
        // to the overload above on its own: a pack is never the more
        // specialized match.
        template <fstr::fstr Pattern, typename C, typename... CtorArgs>
        [[nodiscard]] Router ws(CtorArgs... args) && {
            static_assert(std::is_constructible_v<C, CtorArgs...>, "the controller cannot be constructed from these arguments");

            // Constructed here, once -- not built per request. Every
            // connection on this route shares this one instance, which is
            // what makes members shared state rather than per-connection
            // state, and what requires the controller to be safe for
            // concurrent use when threads > 1.
            return std::move(*this).template ws<Pattern, C>(std::make_shared<C>(std::move(args)...));
        }

        template <typename F>
        [[nodiscard]] Router layer(F middleware) && {
            root_.middleware.emplace_back(wrap_layer<S>(std::move(middleware)));
            return std::move(*this);
        }

        template <fstr::fstr Prefix>
        [[nodiscard]] Router nest(Router&& nested) && {
            static_assert(detail::parse_pattern(Prefix.view()).ok, "invalid nest prefix");
            static_assert(detail::parse_pattern(Prefix.view()).params == 0, "a nest prefix must be literal; parameters belong in the nested routes");
            // The root prefix is spelled empty so a nested path never
            // starts with two slashes.
            std::string path{Prefix.view() == "/" ? std::string_view{} : Prefix.view()};
            adopt(std::move(nested.root_), path);
            return std::move(*this);
        }

        [[nodiscard]] const Handler<S>*
        match(
            const Method method,
            const std::string_view path,
            Request& req,
            detail::MatchedChains<S>* out = nullptr
        ) const {
            std::array<std::string_view, max_path_segments> segments{};
            const std::size_t count = detail::split_path(path, segments.data(), segments.size());
            if (count == detail::no_match) return nullptr;

            // A caller with no use for the chains still gets a walk that
            // records them; the scratch is default-initialised for the
            // reason MatchedChains gives.
            detail::MatchedChains<S> scratch;
            detail::MatchedChains<S>& chains = out != nullptr ? *out : scratch;
            chains.count = 0;
            chains.push(&root_.middleware);

            req.clear_params();
            detail::Walk<S> walk{
                .segments = segments.data(),
                .count = count,
                .method = method,
                .websocket = method == Method::Get && ws::detail::wants_websocket(req),
                .req = req,
                .chains = chains,
            };
            return walk.descend(root_, 0);
        }

        [[nodiscard]] MethodSet
        allowed_methods(const std::string_view path) const noexcept {
            std::array<std::string_view, max_path_segments> segments{};
            const std::size_t count = detail::split_path(path, segments.data(), segments.size());

            MethodSet allowed;
            if (count == detail::no_match) return allowed;
            detail::collect_methods(root_, segments.data(), count, 0, allowed);
            return allowed;
        }

        [[nodiscard]] std::size_t size() const noexcept {
            return size_;
        }

    private:
        Router() = default;

        // What every registration checks of a route before mounting it: the
        // pattern parses, and the parameters are the pattern's and this
        // router's. Parsing comes first because declares<Pattern> reads the
        // parse without checking it -- a bad pattern would otherwise be
        // reported as a parameter the pattern does not declare.
        template <fstr::fstr Pattern, typename... Args>
        static constexpr void check_args() {
            static_assert(detail::parse_pattern(Pattern.view()).ok, "invalid route pattern");
            static_assert(
                ((detail::path_param_of<std::remove_cvref_t<Args>>::value.empty()
                    || detail::declares<Pattern>(detail::path_param_of<std::remove_cvref_t<Args>>::value)) && ...),
                "handler asks for a path parameter this route pattern does not declare");
            static_assert(
                ((!detail::state_of<std::remove_cvref_t<Args>>::is_state
                    || std::is_same_v<typename detail::state_of<std::remove_cvref_t<Args>>::type, S>) && ...),
                "handler asks for a State<T> that is not this router's state type");
        }

        // What both .ws forms check beyond check_args: none of the
        // parameters is a view into the request the connection outlives.
        template <fstr::fstr Pattern, typename... Args>
        static constexpr void ws_checks() {
            check_args<Pattern, Args...>();
            static_assert((!detail::is_view_extractor_v<std::remove_cvref_t<Args>> && ...),
                          "a WebSocket handler outlives its request: take Path/Query/Header with an owning T, "
                          "Json<T>, State<T>, or a const& to a driver -- not a view");
        }

        // What every controller must satisfy, whichever way it arrives.
        //
        // Args... is the pack from C::Extractors, empty when it declares
        // none. Every method is allowed either spelling -- with the pack
        // or without it -- so on_disconnect need not carry an auth header
        // it has no use for. With an empty pack the two are the same
        // check, which is why nothing changes for a controller that takes
        // no extractors.
        template <typename C, typename... Args>
        static constexpr void ws_controller_checks() {
            static_assert(ws::detail::HasOnMessage<C, Args...>
                          || ws::detail::HasOnMessage<C>,
                          "a WebSocket controller needs "
                          "on_message(ws::Socket, ws::Message) returning void or task<void>, "
                          "optionally followed by the controller's declared Extractors");
            // A method that exists but does not match is a silently dead
            // callback, which is worse than either a match or an absence.
            static_assert(!ws::detail::NamesOnConnect<C>
                          || ws::detail::HasOnConnect<C, Args...>
                          || ws::detail::HasOnConnect<C>,
                          "on_connect must be on_connect(ws::Socket), optionally followed by "
                          "the controller's declared Extractors, returning void or task<void>");
            static_assert(!ws::detail::NamesOnDisconnect<C>
                          || ws::detail::HasOnDisconnect<C, Args...>
                          || ws::detail::HasOnDisconnect<C>,
                          "on_disconnect must be on_disconnect(ws::Socket), optionally followed "
                          "by the controller's declared Extractors, returning void or "
                          "task<void> -- it takes the Socket because one controller serves "
                          "every connection and must be told which one ended");
        }

        // Registers a controller route with C::Extractors unpacked into a
        // parameter pack. The tag is there so Args... can be *deduced* from
        // a type alias, which an explicit template argument list cannot do
        // on its own.
        //
        // From here on this is the coroutine form's registration with a
        // WsController in place of a function pointer -- the same
        // extraction, the same checks, the same pre-upgrade rejection.
        template <fstr::fstr Pattern, typename C, typename... Args>
        void register_controller(std::shared_ptr<C> instance, std::type_identity<std::tuple<Args...>>) {
            // Order matters for the diagnostic, not the outcome: a pack
            // that names an undeclared path parameter also fails to match
            // on_message, and "your on_message is wrong" would be the wrong
            // thing to be told about a mistake in the pattern.
            ws_checks<Pattern, Args...>();
            ws_controller_checks<C, Args...>();
            mount_ws<Pattern, Args...>(detail::WsController<C, Args...>{std::move(instance)});
        }

        // Where both .ws forms end: extract per request, reject before the
        // upgrade, and bind what was extracted to the route's start.
        template <fstr::fstr Pattern, typename... Args, typename Start>
        void mount_ws(Start start) {
            insert_ws(Pattern.view(), [start = std::move(start)](const Request& req, const Context<S>& ctx) -> coro::task<Response> {
                auto args = extract_all<Args...>(ctx, req);
                if (!args) [[unlikely]] co_return to_response(std::move(args).error());
                co_return Response::websocket(std::make_unique<detail::WsRoute<Start, Args...>>(start, *std::move(args)));
            });
        }

        template <fstr::fstr Pattern, Method M, typename R, typename... Args>
        void mount(const detail::Endpoint<M, R, Args...>& endpoint) {
            static_assert(detail::is_handler_return_v<R>, "a handler must return Response or coro::task<Response>");
            check_args<Pattern, Args...>();

            // Not a coroutine: a handler that is one hands its own task
            // back, so no adapter frame sits between the chain and it. The
            // by-value arguments move into that task's frame at the call,
            // before args is gone; a const& one binds a Context member.
            auto* const handler = endpoint.handler;
            insert(M, Pattern.view(), [handler](const Request& req, const Context<S>& ctx) -> coro::task<Response> {
                auto args = extract_all<Args...>(ctx, req);
                if (!args) [[unlikely]] return detail::kicked(std::move(args).error());
                if constexpr (std::is_same_v<R, Response>) {
                    return detail::ready(std::apply(handler, *std::move(args)));
                } else {
                    return std::apply(handler, *std::move(args));
                }
            });
        }

        // Registers every route of a nested trie under a prefix. Each
        // node's handlers, Upgrade slot and layers go through the same
        // insert, insert_ws and claim as a direct registration, so the
        // depth cap, the parameter-name rule and the collision rules are
        // enforced in one place, and every node's pattern is its full
        // path. `path` is the prefix so far; a nested "/" adds nothing to
        // it, so /api + / is /api and / + /ping is /ping.
        void adopt(detail::RouteNode<S>&& from, std::string& path) {
            const std::string_view pattern = path.empty() ? "/" : std::string_view{path};
            for (auto& [method, handler] : from.handlers) insert(method, pattern, std::move(handler));
            if (from.websocket) insert_ws(pattern, std::move(from.websocket));
            if (!from.middleware.empty()) {
                MiddlewareChain<S>& chain = claim(pattern).middleware;
                for (auto& layer : from.middleware) chain.push_back(std::move(layer));
            }

            const std::size_t mark = path.size();
            for (auto& child : from.literals) {
                path.append("/").append(child->label);
                adopt(std::move(*child), path);
                path.resize(mark);
            }
            if (from.param) {
                path.append("/{").append(from.param->param_name).append("}");
                adopt(std::move(*from.param), path);
                path.resize(mark);
            }
        }

        // The node a pattern names, created along the way. Refuses a
        // parameter slot already claimed under another name: two routes
        // naming it differently would make param() depend on which won.
        detail::RouteNode<S>& claim(const std::string_view pattern) {
            const detail::PatternInfo info = detail::parse_pattern(pattern);
            // Asserted at compile time for a direct registration; a nested
            // path is built at run time and can run past the depth cap.
            if (!info.ok) throw std::invalid_argument(std::format("cannot register {}: too many segments", pattern));

            detail::RouteNode<S>* node = &root_;
            for (std::size_t i = 0; i < info.count; ++i) {
                const detail::Segment& segment = info.segments[i];
                if (!segment.is_param) {
                    node = &node->literal_child(segment.text);
                    continue;
                }
                if (!node->param) {
                    node->param = std::make_unique<detail::RouteNode<S>>();
                    node->param->param_name = std::string(segment.text);
                } else if (node->param->param_name != segment.text) {
                    throw std::invalid_argument(std::format("cannot register {}: {{{}}} is already {{{}}} at that position",
                                                            pattern, segment.text, node->param->param_name));
                }
                node = node->param.get();
            }
            return *node;
        }

        void insert(const Method method, const std::string_view pattern, Handler<S> handler) {
            detail::RouteNode<S>& node = claim(pattern);
            if (node.registered(method)) {
                throw std::invalid_argument(std::format("cannot register {}: {} is already registered", pattern, method));
            }
            node.handlers.emplace_back(method, std::move(handler));
            node.pattern = std::string(pattern);
            ++size_;
            refresh_options_fallback(node);
        }

        // The Allow line is fixed here, once per registration, rather than
        // walked out of the node per request.
        static void refresh_options_fallback(detail::RouteNode<S>& node) {
            MethodSet allowed;
            for (const auto& method : node.handlers | std::views::keys) allowed.add(method);
            allowed.add(Method::Options);
            node.options_fallback = [allow = allowed.to_allow_header()](const Request&, const Context<S>&) -> coro::task<Response> {
                return detail::ready(Response::no_content().header("allow", allow));
            };
        }

        void insert_ws(const std::string_view pattern, Handler<S> handler) {
            detail::RouteNode<S>& node = claim(pattern);
            if (node.websocket) {
                throw std::invalid_argument(std::format("cannot register {}: a websocket is already registered", pattern));
            }
            node.websocket = std::move(handler);
            node.pattern = std::string(pattern);
            ++size_;
        }

        detail::RouteNode<S> root_;
        std::size_t size_ = 0;
    };
}
