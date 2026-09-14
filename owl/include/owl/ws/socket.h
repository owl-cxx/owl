#pragma once

// One WebSocket connection, as a handler sees it.
//
//     coro::task<void> room(owl::ws::Socket sock, owl::Path<"id", int> id) {
//         co_await sock.send("welcome");
//         while (const auto msg = co_await sock.recv()) {
//             co_await sock.send(msg->data());
//         }
//     }                                    // returning closes the connection
//
// The handler *is* the connection: it starts when the upgrade completes and the
// connection closes when it returns, so per-connection state is just locals and
// there is no lifecycle to implement. `recv()` returning nullopt is the peer
// having gone away.
//
// A Socket may be copied, kept and called from any thread. It shares the
// connection's Handle rather than pointing at the Session, so a copy that
// outlives its connection is inert rather than dangling, and send() or
// close() from another worker is posted to the connection's own worker
// instead of racing it. recv() is the one exception: it parks the handler,
// so it belongs to the handler, on its own worker.
//
// Exposes no wslay type. Engine-only operations go through SessionOps, and the
// accessors that only read Session are inline in session.h so every translation
// unit that holds a handler links.

#include <cassert>
#include <coroutine>
#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>

#include "owl/ws/detail/session.h"
#include "owl/ws/message.h"

namespace owl::ws {
    class Socket final {
    public:
        explicit Socket(std::shared_ptr<detail::Handle> handle) noexcept : handle_(std::move(handle)) {
        }

        // Copy only. A moved-from Socket would hold null, and a Socket is
        // always callable; declaring the copy leaves no implicit move, so a
        // move is a copy: one atomic increment.
        Socket(const Socket&) = default;
        Socket& operator=(const Socket&) = default;

        // Suspends until the next message, or resumes with nullopt once the
        // peer has gone. `while (const auto m = co_await sock.recv())` is the
        // whole read loop.
        class Recv final {
        public:
            explicit Recv(detail::Session* session) noexcept : session_(session) {
            }

            // A queued message, or an already-closed connection, needs no trip
            // through the event loop.
            [[nodiscard]] bool await_ready() const noexcept {
                return !session_->pending.empty() || !detail::is_open(session_);
            }

            void await_suspend(const std::coroutine_handle<> waiter) const noexcept {
                detail::park(session_, waiter);
            }

            [[nodiscard]] std::optional<Message> await_resume() const noexcept {
                return detail::take_message(session_);
            }

        private:
            detail::Session* session_;
        };

        [[nodiscard]] Recv recv() const noexcept {
            assert(detail::on_owner(handle_.get()) && "recv() belongs to the connection's own worker");
            return Recv{handle_->session};
        }

        // Any thread. On the connection's worker it is queued and flushed at
        // once; from another thread it is posted there. On a connection that
        // has ended it does nothing. Never suspends -- wslay buffers, so
        // there is nothing to wait for yet -- but awaitable anyway, so that
        // adding real backpressure later changes the return type and not a
        // single call site. One overload on purpose: a string_view companion
        // made passing a std::string ambiguous, and the payload has to be
        // owned here anyway.
        [[nodiscard]] std::suspend_never send(std::string data, const Opcode opcode = Opcode::Text) const noexcept {
            detail::enqueue(handle_, std::move(data), opcode);
            return {};
        }

        // Starts the closing handshake. The handler's next recv() reports
        // nullopt; returning from the handler does this anyway. Any thread,
        // like send().
        void close() const noexcept {
            detail::begin_close(handle_);
        }

        [[nodiscard]] bool open() const noexcept {
            return detail::phase(handle_.get()) == detail::Phase::Open;
        }

        // A stable identity for the connection. The Handle's address, so it is
        // not reused while any copy of this Socket is alive.
        //
        // A shared controller sees every connection through the same instance,
        // so it needs something to key per-connection state on; this is it.
        // Opaque on purpose: nothing but comparison and hashing is meaningful.
        [[nodiscard]] const void* id() const noexcept {
            return handle_.get();
        }

        [[nodiscard]] friend bool operator==(const Socket& a, const Socket& b) noexcept {
            return a.handle_ == b.handle_;
        }

        // There is deliberately no request() accessor. The h2o request is gone
        // the moment the upgrade completes, and a Request holds a pointer to
        // the driver plus path parameters that are string_views into h2o's
        // path buffer -- every one of them dangling by the time a handler could
        // read them. Ask for what you need as a handler parameter instead,
        // where it is extracted before the upgrade and copied into the frame.
        // Owning extractors only; see the static_assert in Router::ws.

    private:
        std::shared_ptr<detail::Handle> handle_;
    };
}

// So a controller can write unordered_map<Socket, ...> directly. By const&,
// so hashing costs no refcount.
template <>
struct std::hash<owl::ws::Socket> {
    std::size_t operator()(const owl::ws::Socket& sock) const noexcept {
        return std::hash<const void*>{}(sock.id());
    }
};
