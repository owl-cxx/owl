#pragma once

// Per-connection state, shared by Socket, the engine and the driver -- and
// everything the outside may do to a connection: send, close, park in
// recv(). Only the engine drives wslay.
//
// Heap, not the request pool: the upgrade destroys the request. Complete
// here, with no wslay type, so everything below can be inline and every
// translation unit that holds a WebSocket handler links -- not only the
// ones that include the engine.

#include <atomic>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>

#include <h2o.h>

#include <coro/task.h>

#include "owl/coro/loop_scheduler.h"
#include "owl/ws/message.h"

namespace owl::ws::detail {
    struct Session;

    // What one connection may hold. Copied into the Session when upgrade()
    // builds it, so changing default_limits never races a live connection.
    struct Limits final {
        // One inbound message. wslay fails the connection with 1009 past it.
        std::size_t max_message_bytes = 16 * 1024 * 1024;
        // Delivered but not yet taken by the handler. Reading pauses here
        // and resumes when the handler next parks in recv(), so a peer that
        // outruns its handler is held back by TCP, not by the heap.
        std::size_t max_pending_bytes = 16 * 1024 * 1024;
        // Queued for the peer. Past it the connection is dropped: send()
        // does not suspend yet, so a peer that stops reading would grow this
        // without bound.
        std::size_t max_unsent_bytes = 64 * 1024 * 1024;
        // Inbound silence before a ping. A ping still unanswered one interval
        // later drops the peer, as does a close handshake unanswered for two:
        // a half-open TCP peer never errors, so this is the only way out.
        std::uint64_t idle_ping_ms = 30'000;
    };

    // Process-wide defaults. Not yet a Server option; the tests lower them
    // before their workers start.
    inline Limits default_limits{};

    // h2o_timer_t carries no user data, so a callback walks back from the
    // timer -- which needs a standard-layout struct, and Session (a deque,
    // a task) is not one.
    struct SessionTimer final {
        h2o_timer_t timer{};
        Session* session = nullptr;
    };

    [[nodiscard]] inline Session* session_of(h2o_timer_t* const entry) noexcept {
        return H2O_STRUCT_FROM_MEMBER(SessionTimer, timer, entry)->session;
    }

    // What only the engine can do, on the owner thread. Pointers rather
    // than declarations, so a translation unit that never includes the
    // engine still links.
    struct SessionOps final {
        void (*enqueue)(Session*, std::string, Opcode) noexcept;
        void (*close)(Session*) noexcept;
    };

    // Where a connection is in its life. It only ever advances. Open is the
    // one phase a Socket may send in, on any thread; Closing means nothing
    // more will be delivered; Dead that the socket failed too, so nothing
    // more can be written.
    enum class Phase : std::uint8_t { Opening, Open, Closing, Dead };

    // What every Socket copy shares, and what outlives the Session. A Socket
    // kept in a shared map after its connection ended stays safe to call --
    // send and close find no session and do nothing -- and its id() cannot
    // be handed to a new connection while the old copy is alive, which a
    // session address could.
    //
    // session is touched only on the owner thread. owner, hop and ops are
    // set at upgrade, before the handler starts, so before any Socket can
    // reach another thread. phase is the one field any thread may read.
    struct Handle final {
        std::atomic<Phase> phase{Phase::Opening};   // advanced by the owner, read anywhere
        Session* session = nullptr;                 // null once the session is destroyed
        std::thread::id owner{};                    // the worker thread running the session
        h2o_multithread_receiver_t* hop = nullptr;  // that worker's hop
        const SessionOps* ops = nullptr;            // null until upgrade()
    };

    [[nodiscard]] inline Phase phase(const Handle* handle) noexcept {
        return handle->phase.load(std::memory_order_relaxed);
    }

    [[nodiscard]] inline bool on_owner(const Handle* handle) noexcept {
        return std::this_thread::get_id() == handle->owner;
    }

    struct Session final {
        Session() {
            handle->session = this;
        }

        ~Session() {
            handle->session = nullptr;
        }

        Session(const Session&) = delete;
        Session& operator=(const Session&) = delete;

        // The connection is this coroutine; see engine.h for how it ends.
        coro::task<void> handler;
        // Messages the handler has not taken yet. A queue rather than a slot:
        // a handler that awaits inside on_message sees later frames wait here
        // instead of interleaving.
        std::deque<Message> pending;
        std::coroutine_handle<> waiter;       // set while parked in recv()
        const std::shared_ptr<Handle> handle = std::make_shared<Handle>();

        h2o_loop_t* loop = nullptr;           // saved before the request dies
        h2o_socket_t* sock = nullptr;         // set in on_complete
        void* wslay = nullptr;                // wslay_event_context_ptr, opaque here
        std::string out;                      // frames staged for the next write; h2o's until that write completes
        Limits limits = default_limits;       // copied from default_limits at upgrade
        std::size_t pending_bytes = 0;        // the payload bytes sitting in pending
        std::size_t unsent_bytes = 0;         // queued for the peer, not yet handed to h2o
        std::uint64_t last_seen = 0;          // h2o_now() of the last inbound byte
        std::uint64_t ping_sent = 0;          // h2o_now() of the unanswered ping; 0 if none
        SessionTimer kicker{{}, this};        // runs the engine, or reaps, on the loop's next pass
        SessionTimer idle{{}, this};          // pings a quiet peer, drops a dead one

        bool finished = false;                // the handler has returned
        bool reading_paused = false;          // stopped at max_pending_bytes; park() restarts it
    };

    [[nodiscard]] inline Phase phase(const Session* session) noexcept {
        return phase(session->handle.get());
    }

    // Owner thread only, and never backwards.
    inline void advance(Session* session, const Phase to) noexcept {
        if (phase(session) < to) session->handle->phase.store(to, std::memory_order_relaxed);
    }

    [[nodiscard]] inline bool is_open(const Session* session) noexcept {
        return phase(session) == Phase::Open;
    }

    [[nodiscard]] inline bool closing(const Session* session) noexcept {
        return phase(session) >= Phase::Closing;
    }

    [[nodiscard]] inline bool dead(const Session* session) noexcept {
        return phase(session) == Phase::Dead;
    }

    [[nodiscard]] inline std::optional<Message> take_message(Session* session) noexcept {
        if (session->pending.empty()) return std::nullopt;
        Message message = std::move(session->pending.front());
        session->pending.pop_front();
        session->pending_bytes -= message.size();
        return message;
    }

    // Links a timer unless it already is; h2o aborts on a double link.
    inline void link_once(SessionTimer& timer, const std::uint64_t delay) noexcept {
        if (!h2o_timer_is_linked(&timer.timer)) h2o_timer_link(timer.session->loop, delay, &timer.timer);
    }

    // Runs the engine on the loop's next pass; see on_kick in engine.h.
    // Everything that may resume the handler goes through here, so it
    // starts from the loop rather than inside whoever asked.
    inline void kick(Session* session) noexcept {
        link_once(session->kicker, 0);
    }

    // Parking means pending is empty, so a paused read may resume. The
    // pause was the handler's, not the peer's, so the silence is forgiven.
    inline void park(Session* session, const std::coroutine_handle<> waiter) noexcept {
        session->waiter = waiter;
        if (session->reading_paused) {
            session->last_seen = h2o_now(session->loop);
            kick(session);
        }
    }

    // Owner thread only: the ops are the engine's, and the Session is its.
    inline void deliver_here(const Handle* handle, std::string data, const Opcode opcode, const bool close) noexcept {
        Session* const session = handle->session;
        if (session == nullptr) return;
        if (close) {
            handle->ops->close(session);
        } else {
            handle->ops->enqueue(session, std::move(data), opcode);
        }
    }

    // A send or close from a thread that does not own the connection,
    // carried to the one that does on its hop. The Handle rides along, so
    // it survives the trip even when the connection does not.
    struct Post final : owl::detail::LoopJob {
        std::shared_ptr<Handle> handle;
        std::string data;
        Opcode opcode = Opcode::Text;
        bool close = false;
    };

    inline void on_post(owl::detail::LoopJob* const job) {
        const std::unique_ptr<Post> post{static_cast<Post*>(job)};
        deliver_here(post->handle.get(), std::move(post->data), post->opcode, post->close);
    }

    // The phase is only a cheap early out; the owner checks the session
    // again on delivery, which is the check that counts.
    inline void post_to_owner(std::shared_ptr<Handle> handle, std::string data, const Opcode opcode, const bool close) noexcept {
        if (phase(handle.get()) != Phase::Open) return;
        auto* const post = new Post{{.super = {}, .run = &on_post}, std::move(handle), std::move(data), opcode, close};
        h2o_multithread_send_message(post->handle->hop, &post->super);
    }

    // Any thread. Before upgrade() no thread is the owner and the phase is
    // still Opening, so the call is dropped.
    inline void deliver(const std::shared_ptr<Handle>& handle, std::string data, const Opcode opcode, const bool close) noexcept {
        if (on_owner(handle.get())) {
            deliver_here(handle.get(), std::move(data), opcode, close);
        } else {
            post_to_owner(handle, std::move(data), opcode, close);
        }
    }

    inline void enqueue(const std::shared_ptr<Handle>& handle, std::string data, const Opcode opcode) noexcept {
        deliver(handle, std::move(data), opcode, false);
    }

    inline void begin_close(const std::shared_ptr<Handle>& handle) noexcept {
        deliver(handle, {}, Opcode::Text, true);
    }
}
