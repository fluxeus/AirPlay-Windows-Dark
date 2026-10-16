#include "net/tcp_server.h"
#include "airplay/rtsp_parser.h"
#include "mdns/mdns_service.h"
#include "log.h"
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <thread>
#include <vector>

using namespace std::chrono_literals;
namespace {
void require(bool ok, const char* message) {
    if (!ok) { std::cerr << "FAIL: " << message << '\n'; std::exit(1); }
}
template<class Predicate> void wait_for(Predicate predicate) {
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (!predicate()) {
        require(std::chrono::steady_clock::now() < deadline, "worker deadline");
        std::this_thread::sleep_for(1ms);
    }
}
socket_t connect_to(uint16_t port) {
    socket_t fd = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    require(fd != INVALID_SOCK, "socket");
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(port);
    require(::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0, "connect");
#if defined(_WIN32)
    DWORD timeout = 2000;
#else
    timeval timeout{2, 0};
#endif
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
    return fd;
}
void check_closed(socket_t fd) {
    char byte;
    const int received = ::recv(fd, &byte, 1, 0);
#if defined(_WIN32)
    const bool reset = received < 0 && (ap::net::last_error() == WSAECONNRESET || ap::net::last_error() == WSAECONNABORTED);
#else
    const bool reset = received < 0 && ap::net::last_error() == ECONNRESET;
#endif
    require(received == 0 || reset, "peer receives EOF/reset, never a timeout, on disconnect");
    ap::net::close_socket(fd);
}
void test_control_shutdown() {
    ap::net::TcpServer server;
    for (int round = 0; round < 5; ++round) {
        std::atomic<int> started{0}, cleaned{0};
        require(server.start(0, [&](ap::net::ClientSocket client) {
            ++started;
            ap::airplay::RequestReader reader(client.cancelled);
            ap::airplay::Request request;
            while (reader.read(client.fd, request)) {
                const char reply[] = "OK\n";
                ap::net::send_all(client.fd, reply, 3);
            }
            // Stand in for media teardown: UI reset must wait for completion.
            std::this_thread::sleep_for(30ms);
            ++cleaned;
        }), "server start/restart");
        std::vector<socket_t> clients;
        for (int i = 0; i < 4; ++i) clients.push_back(connect_to(server.port()));
        wait_for([&] { return started == 4; });
        // An idle client survives several cancellation polling intervals.
        std::this_thread::sleep_for(250ms);
        const std::string complete = "OPTIONS / RTSP/1.0\r\nContent-Length: 0\r\n\r\n";
        require(ap::net::send_all(clients[0], complete.data(), static_cast<int>(complete.size())) > 0, "idle connection remains usable");
        char reply[3];
        require(ap::net::recv_all(clients[0], reply, 3) == 3, "idle request reply");
        // Cancel in the middle of a header and of a request body.
        const std::string partial_header = "SETUP / RTSP/1.0\r\nContent-Len";
        const std::string partial_body = "POST / RTSP/1.0\r\nContent-Length: 50\r\n\r\nabc";
        ap::net::send_all(clients[1], partial_header.data(), static_cast<int>(partial_header.size()));
        ap::net::send_all(clients[2], partial_body.data(), static_cast<int>(partial_body.size()));
        bool callback = false;
        server.close_all_clients([&] {
            require(cleaned == 4, "cleanup completes before disconnected callback");
            callback = true;
        });
        require(callback, "drained callback");
        for (auto fd : clients) check_closed(fd);
        auto reconnected = connect_to(server.port());
        wait_for([&] { return started == 5; });
        server.stop();
        require(cleaned == 5, "stop interrupts and joins active handlers");
        check_closed(reconnected);
    }
    std::cout << "PASS: disconnect, EOF/reset, cleanup ordering, reconnect, active stop, restart (5 rounds)\n";
}
void test_blocked_send() {
    ap::net::TcpServer server;
    std::atomic<bool> sending{false}, completed{false};
    require(server.start(0, [&](ap::net::ClientSocket client) {
        int small_buffer = 4096;
        setsockopt(client.fd, SOL_SOCKET, SO_SNDBUF, reinterpret_cast<const char*>(&small_buffer), sizeof(small_buffer));
        std::vector<char> bytes(1024 * 1024);
        sending = true;
        while (ap::net::send_all(client.fd, bytes.data(), static_cast<int>(bytes.size())) > 0) {}
        completed = true;
    }), "send server start");
    auto client = connect_to(server.port());
    wait_for([&] { return sending.load(); });
    std::this_thread::sleep_for(100ms);
    server.stop();
    require(completed, "stop interrupts blocked send");
    ap::net::close_socket(client);
    std::cout << "PASS: stop interrupts blocked send\n";
}
void test_mdns() {
    ap::airplay::DeviceContext context;
    context.name = "AirPlay shutdown regression test";
    context.deviceid = "02:00:00:00:00:03";
    context.features = "0x5A7FFFF7,0x1E";
    context.model = "AppleTV3,2";
    context.srcvers = "220.68";
    ap::mdns::MdnsService service;
    require(service.start(context, 65000), "mDNS registration queued");
    std::this_thread::sleep_for(1s);
    service.stop();
    service.stop();
    std::cout << "PASS: mDNS register/deregister and repeated stop (check DNS callback statuses above)\n";
}
}
int main(int argc, char**) {
    require(ap::net::global_init(), "Winsock");
    ap::set_log_enabled(true);
    // Bound failures even when a regression blocks a worker join forever.
    std::thread([] { std::this_thread::sleep_for(20s); std::_Exit(2); }).detach();
    test_control_shutdown();
    test_blocked_send();
    if (argc > 1) test_mdns();
    ap::net::global_shutdown();
    return 0;
}
