#pragma once

#include "net/socket.h"

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace ap::net {

struct ClientSocket {
    socket_t fd = INVALID_SOCK;
    std::shared_ptr<std::atomic<bool>> cancelled = std::make_shared<std::atomic<bool>>(false);
    // Audio-only senders can retain their output route after a control FIN.
    // Request an explicit transport reset for receiver-initiated disconnect.
    std::shared_ptr<std::atomic<bool>> reset_on_disconnect = std::make_shared<std::atomic<bool>>(false);
    std::string peer;   // "ip:port"
};

// Minimal blocking TCP server. Each accepted client runs on its own thread
// calling `handler(client)`. The socket is borrowed; the server closes it after the handler returns.
// Stop() joins all client threads and closes the listener.
class TcpServer {
public:
    using Handler = std::function<void(ClientSocket)>;

    TcpServer() = default;
    ~TcpServer();

    bool start(uint16_t port, Handler handler);
    void stop();

    // Cancel active handlers and shut down their transports. Handlers must
    // use recv_interruptible() (or otherwise observe client.cancelled).
    // Wait for all session teardown before invoking on_drained. New clients
    // are rejected until that callback completes. Call outside the render
    // thread; stop()/close_all_clients() are serialized by the caller.
    void close_all_clients(const std::function<void()>& on_drained = {});

    uint16_t port() const { return port_; }

private:
    void accept_loop();

    socket_t listener_{INVALID_SOCK};
    uint16_t port_{0};
    std::thread accept_thread_;
    std::atomic<bool> running_{false};
    std::vector<std::thread> workers_;
    Handler handler_;

    // Workers close and erase under the same lock to prevent descriptor
    // reuse from racing a concurrent disconnect request.
    std::mutex          clients_mtx_;
    std::condition_variable clients_cv_;
    bool disconnecting_{false};
    std::map<socket_t, ClientSocket> active_clients_;
};

} // namespace ap::net
