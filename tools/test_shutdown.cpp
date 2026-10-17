#include "net/tcp_server.h"
#include "airplay/dacp_remote.h"
#if defined(_WIN32)
#include <windows.h>
#include <windns.h>
#endif
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
#if defined(_WIN32)
struct DnsCompletion {
    std::mutex mutex;
    std::condition_variable cv;
    bool done{false};
    DWORD status{ERROR_SUCCESS};
    static void WINAPI callback(DWORD status, PVOID ctx, PDNS_SERVICE_INSTANCE instance) {
        auto* self = static_cast<DnsCompletion*>(ctx);
        if (instance) DnsServiceFreeInstance(instance);
        std::lock_guard<std::mutex> lock(self->mutex);
        self->status = status;
        self->done = true;
        self->cv.notify_all();
    }
    void wait() {
        std::unique_lock<std::mutex> lock(mutex);
        require(cv.wait_for(lock, 5s, [&] { return done; }), "DNS callback timeout");
        require(status == ERROR_SUCCESS, "DNS callback succeeds");
    }
};
void test_dacp() {
    ap::net::TcpServer sender;
    std::atomic<int> requests{0};
    const std::vector<std::string> expected = {"playpause", "previtem", "nextitem",
        "setproperty?dacp.playingtime=12345", "setproperty?dacp.playingtime=999"};
    require(sender.start(0, [&](ap::net::ClientSocket client) {
        ap::airplay::RequestReader reader(client.cancelled);
        ap::airplay::Request request;
        require(reader.read(client.fd, request), "DACP request read");
        const int index = requests.load();
        require(index < static_cast<int>(expected.size()), "no duplicate media requests");
        require(request.uri == "/ctrl-int/1/" + expected[index], "media command and seek milliseconds");
        require(request.method == (index >= 3 ? "POST" : "GET"), "POST for seek, GET for transport controls");
        require(request.header("content-length") == "0", "empty request body is framed");
        require(request.header("active-remote") == "123456789", "DACP authenticated with sender token");
        ++requests;
        ap::net::send_all(client.fd, "HTTP/1.1 ", 9);
        std::this_thread::sleep_for(10ms);
        const std::string reply = std::string(index == 4 ? "403 Forbidden" : "204 No Content") +
            "\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
        ap::net::send_all(client.fd, reply.data(), static_cast<int>(reply.size()));
    }), "DACP sender start");
    std::wstring instance_name = L"iTunes_Ctrl_AA01020304050607._dacp._tcp.local";
    wchar_t host[256]; DWORD size = 256;
    require(GetComputerNameExW(ComputerNamePhysicalDnsHostname, host, &size), "DNS hostname");
    std::wstring host_name = std::wstring(host) + L".local";
    DNS_SERVICE_INSTANCE instance{};
    instance.pszInstanceName = instance_name.data();
    instance.pszHostName = host_name.data();
    instance.wPort = sender.port();
    DnsCompletion registered, deregistered;
    DNS_SERVICE_REGISTER_REQUEST dns{};
    dns.Version = DNS_QUERY_REQUEST_VERSION1;
    dns.pServiceInstance = &instance;
    dns.pRegisterCompletionCallback = &DnsCompletion::callback;
    dns.pQueryContext = &registered;
    DNS_SERVICE_CANCEL cancel{};
    require(DnsServiceRegister(&dns, &cancel) == DNS_REQUEST_PENDING, "register fake DACP service");
    registered.wait();
    {
        ap::airplay::DacpRemote remote("127.0.0.1");
        remote.update("AA01020304050607", "123456789", false);
        using ap::airplay::MediaCommand;
        require(!remote.send(MediaCommand::PlayPause), "mirror sessions do not send audio commands");
        std::this_thread::sleep_for(1s);
        remote.update("AA01020304050607", "123456789", true, "test-session");
        require(remote.send(MediaCommand::PlayPause, 0, "test-session"), "play/pause command");
        require(remote.available("test-session"), "audio control availability");
        require(remote.send(MediaCommand::Previous, 0, "test-session"), "previous command");
        require(remote.send(MediaCommand::Next, 0, "test-session"), "next command");
        require(remote.send(MediaCommand::Seek, 12345, "test-session"), "seek is sent in milliseconds");
        require(!remote.send(MediaCommand::Seek, 999, "test-session"), "HTTP rejection reports failure");
        require(!remote.send(MediaCommand::Next, 0, "stale-session"), "stale session command rejected");
        require(requests == 5, "one request per action; none for stale session");
        remote.update("AA01020304050607", "bad\r\nInjected: true", true, "test-session");
        require(!remote.send(MediaCommand::Next), "invalid token never sent");
        require(requests == 5, "no request for invalid credentials");
    }
    dns.pQueryContext = &deregistered;
    require(DnsServiceDeRegister(&dns, nullptr) == DNS_REQUEST_PENDING, "deregister fake DACP service");
    deregistered.wait();
    sender.stop();
    std::cout << "PASS: native DACP discovery, media commands/auth, seek units, fragmented response, HTTP rejection, session/mirror gating\n";
}
#endif
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
int main(int argc, char** argv) {
    require(ap::net::global_init(), "Winsock");
    ap::set_log_enabled(true);
    // Bound failures even when a regression blocks a worker join forever.
    std::thread([] { std::this_thread::sleep_for(20s); std::_Exit(2); }).detach();
    test_control_shutdown();
    test_blocked_send();
    if (argc > 1 && std::string(argv[1]) == "--mdns") test_mdns();
#if defined(_WIN32)
    if (argc > 1 && std::string(argv[1]) == "--dacp") test_dacp();
#endif
    ap::net::global_shutdown();
    return 0;
}
