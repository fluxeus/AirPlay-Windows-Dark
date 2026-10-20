#pragma once

#include <cstdint>
#include <atomic>
#include <string>

#if defined(_WIN32)
    #include <winsock2.h>
    #include <ws2tcpip.h>
    using socket_t = SOCKET;
    constexpr socket_t INVALID_SOCK = INVALID_SOCKET;
#else
    #include <arpa/inet.h>
    #include <netinet/in.h>
    #include <sys/socket.h>
    #include <unistd.h>
    using socket_t = int;
    constexpr socket_t INVALID_SOCK = -1;
#endif

namespace ap::net {

// Initialise Winsock on Windows, no-op elsewhere. Call once at startup.
bool global_init();
void global_shutdown();

void close_socket(socket_t s);
// Detect unreachable TCP peers without treating a paused stream as a disconnect.
bool enable_tcp_keepalive(socket_t s, int idle_seconds = 5,
                          int interval_seconds = 1, int probes = 5);
int  last_error();
std::string last_error_string();

// Read up to len bytes, observing cancellation while idle. Does not close s.
int recv_interruptible(socket_t s, void* buf, int len, const std::atomic<bool>* cancelled);

// Read/write helpers that handle partial I/O.
// Returns number of bytes transferred, 0 on clean close, <0 on error.
int recv_all(socket_t s, void* buf, int len);
int send_all(socket_t s, const void* buf, int len);

// Returns the first non-loopback IPv4 address as a string, or "0.0.0.0".
std::string primary_ipv4();

// Returns the first non-loopback, non-link-local IPv6 address of the
// same interface as primary_ipv4(), or an empty string if none is
// configured. iOS 14+ sometimes prefers v6 on the _airplay._tcp
// discovery and we need to know we're reachable that way.
std::string primary_ipv6();

// Returns a MAC-like 6-byte device id as uppercase "AA:BB:CC:DD:EE:FF".
// AirPlay uses this as the `deviceid` field in the mDNS TXT record and as
// part of the service name. We derive it from the first usable NIC MAC.
std::string primary_mac();

} // namespace ap::net
