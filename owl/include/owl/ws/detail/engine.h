#pragma once

/*
 * Copyright (c) 2014 DeNA Co., Ltd.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

// The only header that sees wslay. Public owl/ws/*.h stay wslay-free, and
// the h2o request is gone once the upgrade completes -- nothing the
// connection needs may live in that frame.
//
// Everything here is inline, not static: upgrade() is inline, and an
// inline function that names a static one is an ODR violation in every
// translation unit after the first.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include <h2o.h>
#include <h2o/http1.h>
#include <openssl/evp.h>
#include <wslay/wslay.h>

#include "coro/task.h"
#include "owl/http/detail/finish.h"
#include "owl/http/request.h"
#include "owl/ws/detail/handshake.h"
#include "owl/ws/detail/session.h"
#include "owl/ws/detail/upgrade.h"

namespace owl::ws::detail {
    [[nodiscard]] inline wslay_event_context_ptr ctx_of(const Session* session) noexcept {
        return static_cast<wslay_event_context_ptr>(session->wslay);
    }

    inline void proceed(Session* session) noexcept;
    inline void on_recv(h2o_socket_t* sock, const char* err);
    inline void on_write_complete(h2o_socket_t* sock, const char* err);

    inline ssize_t recv_callback(wslay_event_context_ptr ctx, uint8_t* buf, size_t len, int, void* user_data) {
        const auto* const session = static_cast<Session*>(user_data);
        if (session->sock->input->size == 0) {
            wslay_event_set_error(ctx, WSLAY_ERR_WOULDBLOCK);
            return -1;
        }
        if (session->sock->input->size < len) len = session->sock->input->size;
        std::memcpy(buf, session->sock->input->bytes, len);
        h2o_buffer_consume(&session->sock->input, len);
        return static_cast<ssize_t>(len);
    }

    // wslay frees its copy the moment this returns, so the bytes go into the
    // staged write. One write in flight at a time: while h2o holds it, wslay
    // keeps the rest queued.
    inline ssize_t send_callback(wslay_event_context_ptr ctx, const uint8_t* data, size_t len, int, void* user_data) {
        auto* const session = static_cast<Session*>(user_data);
        if (h2o_socket_is_writing(session->sock)) {
            wslay_event_set_error(ctx, WSLAY_ERR_WOULDBLOCK);
            return -1;
        }
        session->out.append(reinterpret_cast<const char*>(data), len);
        // Frame headers are counted too, so the clamp keeps it from wrapping.
        session->unsent_bytes -= std::min(session->unsent_bytes, len);
        return static_cast<ssize_t>(len);
    }

    inline void on_msg_recv(wslay_event_context_ptr, const struct wslay_event_on_msg_recv_arg* arg, void* user_data) {
        if (arg->opcode != WSLAY_TEXT_FRAME && arg->opcode != WSLAY_BINARY_FRAME) return;
        auto* const session = static_cast<Session*>(user_data);
        session->pending_bytes += arg->msg_length;
        session->pending.emplace_back(
            std::string(reinterpret_cast<const char*>(arg->msg), arg->msg_length),
            arg->opcode == WSLAY_BINARY_FRAME ? Opcode::Binary : Opcode::Text);
    }

    // The per-frame hooks stay null: whole messages are all owl wants.
    inline constexpr wslay_event_callbacks wslay_callbacks{
        .recv_callback = recv_callback,
        .send_callback = send_callback,
        .on_msg_recv_callback = on_msg_recv,
    };

    inline void wake(Session* session) noexcept {
        if (const auto waiter = std::exchange(session->waiter, {})) {
            waiter.resume();
        }
    }

    // Hands the staged write to h2o. It copies the iovec, not the bytes, so
    // out stays as it is until on_write_complete.
    inline void write_out(Session* session) noexcept {
        if (session->out.empty() || h2o_socket_is_writing(session->sock)) return;
        auto staged = h2o_iovec_init(session->out.data(), session->out.size());
        h2o_socket_write(session->sock, &staged, 1, on_write_complete);
    }

    // Nothing left to send: the last write finished and wslay holds no more
    // frames.
    [[nodiscard]] inline bool drained(const Session* session) noexcept {
        return !h2o_socket_is_writing(session->sock) && session->out.empty() && !wslay_event_want_write(ctx_of(session));
    }

    // A finished handler's session goes once the wire is quiet, or dead.
    [[nodiscard]] inline bool reapable(const Session* session) noexcept {
        return session->finished && (dead(session) || session->sock == nullptr || drained(session));
    }

    inline void destroy(Session* session) noexcept {
        h2o_timer_unlink(&session->kicker.timer);
        h2o_timer_unlink(&session->idle.timer);
        if (session->sock != nullptr) h2o_socket_close(session->sock);
        if (session->wslay != nullptr) wslay_event_context_free(ctx_of(session));
        delete session;
    }

    inline void maybe_reap(Session* session) noexcept {
        if (reapable(session)) kick(session);
    }

    // Everything that may resume this session's handler, or destroy the
    // session, is deferred to the loop's next pass on the session's own
    // timer, so it starts from the loop rather than inside whoever asked --
    // another connection's handler, an h2o callback, the handler's own last
    // line.
    inline void on_kick(h2o_timer_t* entry) {
        Session* const session = session_of(entry);
        if (reapable(session)) {
            destroy(session);
            return;
        }
        if (session->sock != nullptr && !dead(session)) {
            proceed(session);
        } else {
            wake(session);
        }
        maybe_reap(session);
    }

    // Drops the connection with no close handshake: the frame that could say
    // why would sit behind everything the peer is not reading. The handler
    // hears about it from its kick.
    inline void fail(Session* session) noexcept {
        advance(session, Phase::Dead);
        if (session->sock != nullptr) h2o_socket_read_stop(session->sock);
        kick(session);
    }

    // wslay refused: nothing more will be delivered, and a handler parked in
    // recv() hears from its kick. Unlike fail(), the socket stays up so a
    // close already in flight can finish.
    inline void abandon(Session* session) noexcept {
        advance(session, Phase::Closing);
        kick(session);
    }

    // The write half of proceed() and nothing else: no read, no wake. So it
    // is safe from inside any handler, including another connection's --
    // which is exactly where a broadcast calls it.
    inline void flush(Session* session) noexcept {
        if (session->sock == nullptr || dead(session) || h2o_socket_is_writing(session->sock)) return;
        auto* const ctx = ctx_of(session);
        if (wslay_event_want_write(ctx) && wslay_event_send(ctx) != 0) {
            abandon(session);
            return;
        }
        write_out(session);
    }

    // One timer per connection, relinked each interval rather than on every
    // read: noting last_seen is a store, relinking is list surgery.
    //
    // A tick that finds a ping unanswered -- no inbound byte since it was
    // sent -- drops the peer, and so does a closing connection that has
    // heard nothing for two intervals. Otherwise a tick after an interval of
    // silence sends a ping. Any inbound byte is an answer, a pong included.
    // While reading is paused the handler is behind, not the peer, so the
    // peer is not judged.
    inline void on_idle(h2o_timer_t* entry) {
        Session* const session = session_of(entry);
        if (dead(session)) return;
        const std::uint64_t interval = session->limits.idle_ping_ms;
        const std::uint64_t now = h2o_now(session->loop);
        const bool judged = !session->reading_paused;
        const bool unanswered = session->ping_sent != 0 && session->last_seen < session->ping_sent;
        const bool stale_close = closing(session) && now - session->last_seen >= 2 * interval;
        if (judged && (unanswered || stale_close)) {
            fail(session);
            return;
        }
        session->ping_sent = 0;
        if (judged && !closing(session) && now - session->last_seen >= interval) {
            const wslay_event_msg ping{WSLAY_PING, nullptr, 0};
            if (wslay_event_queue_msg(ctx_of(session), &ping) == 0) flush(session);
            session->ping_sent = now;
        }
        h2o_timer_link(session->loop, interval, &session->idle.timer);
    }

    [[nodiscard]] inline bool inbox_full(const Session* session) noexcept {
        return session->pending_bytes >= session->limits.max_pending_bytes;
    }

    inline void proceed(Session* session) noexcept {
        auto* const ctx = ctx_of(session);
        bool close = false;
        for (;;) {
            if (!h2o_socket_is_writing(session->sock) && wslay_event_want_write(ctx) && wslay_event_send(ctx) != 0) {
                close = true;
                break;
            }
            if (session->sock->input->size == 0 || !wslay_event_want_read(ctx) || inbox_full(session)) break;
            if (wslay_event_recv(ctx) != 0) {
                close = true;
                break;
            }
            // What was read may want answering, so around again.
        }

        bool read = false;
        if (!close) {
            write_out(session);
            const bool want_read = wslay_event_want_read(ctx);
            // The handler is behind: let TCP hold the rest.
            session->reading_paused = want_read && inbox_full(session);
            read = want_read && !session->reading_paused;
            // Neither side has anything left: the close handshake is done.
            close = !want_read && !h2o_socket_is_writing(session->sock) && !wslay_event_want_write(ctx);
        }
        if (close) advance(session, Phase::Closing);
        if (read) {
            h2o_socket_read_start(session->sock, on_recv);
        } else {
            h2o_socket_read_stop(session->sock);
        }
        if (!session->pending.empty() || closing(session)) wake(session);
    }

    inline void on_recv(h2o_socket_t* sock, const char* err) {
        auto* const session = static_cast<Session*>(sock->data);
        if (err != nullptr) {
            fail(session);
            return;
        }
        session->last_seen = h2o_now(session->loop);
        proceed(session);
    }

    inline void on_write_complete(h2o_socket_t* sock, const char* err) {
        auto* const session = static_cast<Session*>(sock->data);
        session->out.clear();
        if (err != nullptr) {
            fail(session);
            return;
        }
        proceed(session);
        maybe_reap(session);
    }

    // Owner thread only. A failed queue is out of memory or a close already
    // queued; either way nothing more will be delivered.
    inline void enqueue_here(Session* session, std::string data, const Opcode opcode) noexcept {
        if (closing(session)) return;
        if (session->unsent_bytes + data.size() > session->limits.max_unsent_bytes) {
            fail(session);
            return;
        }
        const uint8_t frame = opcode == Opcode::Binary ? WSLAY_BINARY_FRAME : WSLAY_TEXT_FRAME;
        const wslay_event_msg message{frame, reinterpret_cast<const uint8_t*>(data.data()), data.size()};
        if (wslay_event_queue_msg(ctx_of(session), &message) != 0) {
            abandon(session);
            return;
        }
        session->unsent_bytes += data.size();
        flush(session);
    }

    inline void close_here(Session* session, const std::uint16_t code) noexcept {
        if (closing(session)) return;
        advance(session, Phase::Closing);
        wslay_event_queue_close(ctx_of(session), code, nullptr, 0);
        flush(session);
        kick(session);
    }

    inline void finish(Session* session, const std::uint16_t code) noexcept {
        session->finished = true;
        // A dropped connection must not go on writing its backlog.
        if (!dead(session)) close_here(session, code);
        maybe_reap(session);
    }

    // The handler is the connection, so its failure is the connection's:
    // 1011 tells the peer this was not a normal close. Nothing is logged --
    // owl's library code has no logger -- so the close code is the signal.
    inline coro::task<void> supervise(Session* session, coro::task<void> inner) {
        std::uint16_t code = WSLAY_CODE_NORMAL_CLOSURE;
        try {
            co_await std::move(inner);
        } catch (...) {
            code = WSLAY_CODE_INTERNAL_SERVER_ERROR;
        }
        finish(session, code);
    }

    inline constexpr SessionOps engine_ops{
        .enqueue = &enqueue_here,
        .close = [](Session* session) noexcept { close_here(session, WSLAY_CODE_NORMAL_CLOSURE); },
    };

    inline void on_complete(void* data, h2o_socket_t* sock, size_t reqsize) {
        auto* const session = static_cast<Session*>(data);
        if (sock == nullptr) {
            destroy(session);
            return;
        }
        session->sock = sock;
        sock->data = session;
        h2o_buffer_consume(&sock->input, reqsize);
        advance(session, Phase::Open);
        session->last_seen = h2o_now(session->loop);
        h2o_timer_link(session->loop, session->limits.idle_ping_ms, &session->idle.timer);
        session->handler.start();
        proceed(session);
    }

    [[nodiscard]] inline bool valid_key(h2o_req_t* req, const std::string_view key) noexcept {
        if (key.size() != 24) return false;
        // `=` is not in the base64url alphabet. A 16-byte nonce is 24 chars
        // with padding; stripping it is what makes decode report 16 bytes
        // rather than fail. The function accepts `+` and `/` too.
        auto b64 = key;
        while (!b64.empty() && b64.back() == '=') b64.remove_suffix(1);
        const auto decoded = h2o_decode_base64url(&req->pool, b64.data(), b64.size());
        return decoded.base != nullptr && decoded.len == 16;
    }

    // RFC 6455 4.2.2: base64(SHA-1(key + GUID)). h2o's encoder writes the
    // trailing NUL, so the result reads as a C string.
    [[nodiscard]] inline std::array<char, 29> accept_key(const std::string_view client_key) noexcept {
        constexpr std::string_view guid = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
        unsigned char source[24 + 36];
        std::memcpy(source, client_key.data(), 24);
        std::memcpy(source + 24, guid.data(), 36);
        unsigned char digest[20];
        EVP_Digest(source, sizeof(source), digest, nullptr, EVP_sha1(), nullptr);
        std::array<char, 29> encoded;
        h2o_base64_encode(encoded.data(), digest, sizeof(digest), 0);
        return encoded;
    }

    // Returns the status the exchange ended with: 101 once h2o owns the
    // connection, otherwise the error run_handler sends. The handshake is
    // judged before anything is built, so a rejection costs no session and
    // no frame. Takes the Request run_handler already has -- building a
    // second one cost a pool allocation and a query parse per upgrade.
    // Still checks wants_websocket: Response::websocket is public, and a
    // plain GET route may return one.
    //
    // Not noexcept: building the session and the handler's frame may throw,
    // which run_handler answers with its 500. Nothing after
    // h2o_http1_upgrade can.
    [[nodiscard]] inline int upgrade(Request& request, const owl::detail::WsUpgradePtr route, h2o_multithread_receiver_t* const hop) {
        h2o_req_t* const req = request.raw();
        if (!wants_websocket(request)) return 400;
        if (request.header("Sec-WebSocket-Version") != "13") {
            // RFC 6455 4.4: name the version this server speaks.
            owl::detail::add_header(req, "sec-websocket-version", "13");
            return 426;
        }
        const auto key = request.header("Sec-WebSocket-Key");
        if (!key.has_value() || !valid_key(req, *key)) return 400;

        auto session = std::make_unique<Session>();
        // The task is lazy: the extractors move into its frame here, while
        // the route -- owned by this call -- is still alive.
        session->handler = supervise(session.get(), route->make(session.get()));
        wslay_event_context_ptr ctx = nullptr;
        if (wslay_event_context_server_init(&ctx, &wslay_callbacks, session.get()) != 0) return 500;
        wslay_event_config_set_max_recv_msg_length(ctx, session->limits.max_message_bytes);
        session->wslay = ctx;
        Handle& handle = *session->handle;
        handle.ops = &engine_ops;
        handle.hop = hop;
        handle.owner = std::this_thread::get_id();
        session->loop = req->conn->ctx->loop;
        h2o_timer_init(&session->kicker.timer, on_kick);
        h2o_timer_init(&session->idle.timer, on_idle);

        const auto accept = accept_key(*key);
        owl::detail::write_status(req, 101);
        h2o_add_header(&req->pool, &req->res.headers, H2O_TOKEN_UPGRADE, nullptr, H2O_STRLIT("websocket"));
        owl::detail::add_header(req, "sec-websocket-accept", accept.data());

        h2o_http1_upgrade(req, nullptr, 0, on_complete, session.release());
        return 101;
    }
}
