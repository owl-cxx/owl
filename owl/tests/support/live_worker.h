#pragma once

// Server::start() never returns and has no stop yet, so the live tests
// build workers from Server's own parts and pump each on a thread they
// can stop. Each worker listens on its own loopback port, so a test
// chooses the worker a client lands on.

#include <atomic>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <thread>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <h2o.h>

#include <owl/detail.h>
#include <owl/routing/router.h>

namespace owl_test {
    [[nodiscard]] inline int listen_loopback() {
        const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) throw std::runtime_error("listen_loopback: socket");
        constexpr int reuse = 1;
        ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;
        if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 || ::listen(fd, 128) != 0) {
            ::close(fd);
            throw std::runtime_error("listen_loopback: bind");
        }
        return fd;
    }

    template <typename S>
    struct LiveWorker final {
        owl::MiddlewareChain<S> layers;
        owl::detail::GlobalConf globalconf;
        std::vector<std::unique_ptr<owl::detail::Worker>> workers;
        std::atomic<bool> stop{false};
        std::vector<std::thread> pumps;
        std::vector<std::uint16_t> ports;
        std::uint16_t port = 0;

        explicit LiveWorker(const owl::Router<S>& router, const std::size_t count = 1) {
            std::signal(SIGPIPE, SIG_IGN);
            auto* const host = h2o_config_register_host(&globalconf.conf, h2o_iovec_init(H2O_STRLIT("default")), 65535);
            auto* const path = h2o_config_register_path(host, "/", 0);
            (void)owl::detail::make_dispatcher<S>(path, &router, &layers, std::make_shared<S>(), {});
            for (std::size_t i = 0; i < count; ++i) {
                auto& worker = *workers.emplace_back(std::make_unique<owl::detail::Worker>(&globalconf.conf));
                const int fd = listen_loopback();
                ports.push_back(owl::detail::port_of(fd));
                worker.listener = h2o_evloop_socket_create(worker.ctx.loop, fd, H2O_SOCKET_FLAG_DONT_READ);
                worker.listener->data = &worker;
                h2o_socket_read_start(worker.listener, &owl::detail::on_accept);
            }
            port = ports.front();
            for (const auto& worker : workers) {
                pumps.emplace_back([this, loop = worker->ctx.loop] {
                    while (!stop.load()) h2o_evloop_run(loop, 5);
                });
            }
        }

        ~LiveWorker() {
            stop.store(true);
            for (auto& pump : pumps) pump.join();
            workers.clear();
        }

        LiveWorker(const LiveWorker&) = delete;
        LiveWorker& operator=(const LiveWorker&) = delete;
    };
}
