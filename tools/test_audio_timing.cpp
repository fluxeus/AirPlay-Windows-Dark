#define SDL_MAIN_HANDLED
#include "airplay/ntp_clock.h"
#include "airplay/ntp_client.h"
#include "audio/audio_receiver.h"
#include "audio/rtp_flush.h"
#include "video/video_renderer.h"
#include "log.h"
#include <SDL.h>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <thread>
#include <vector>
extern "C" {
#include <libavcodec/avcodec.h>
#include <openssl/evp.h>
}
using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;
#if defined(_WIN32)
using SocketLength = int;
#else
using SocketLength = socklen_t;
#endif
std::atomic<int> played{0};
std::atomic<int64_t> first_played{0};
int64_t now_ns() { return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count(); }
namespace ap {
void log_write(LogLevel, const std::string& text) {
    if (text.find("Audio playout:") == 0) {
        if (played.fetch_add(1) == 0) first_played = now_ns();
    }
    if (text.find("error") != std::string::npos) std::fprintf(stderr, "%s\n", text.c_str());
}
void set_log_enabled(bool) {}
bool is_log_enabled() { return true; }
}
// No renderer is attached in this integration test; only satisfy link symbols.
namespace ap::video {
void VideoRenderer::push_audio_pcm(const int16_t*, int, int, int) {}
void VideoRenderer::push_playback_rate(float) {}
bool VideoRenderer::in_flush_grace() const { return false; }
}
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
uint64_t remote_now() {
    const uint64_t ns = static_cast<uint64_t>(now_ns()) + 400'000'000'000'000ULL;
    return ((ns / 1'000'000'000ULL) << 32) | ((ns % 1'000'000'000ULL) << 32) / 1'000'000'000ULL;
}
void put32(unsigned char* p, uint32_t v) { for (int i = 3; i >= 0; --i) { p[i] = static_cast<unsigned char>(v); v >>= 8; } }
void put64(unsigned char* p, uint64_t v) { put32(p, static_cast<uint32_t>(v >> 32)); put32(p + 4, static_cast<uint32_t>(v)); }
struct Udp {
    socket_t fd = INVALID_SOCK;
    sockaddr_in address{};
    Udp() {
        fd = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        require(fd != INVALID_SOCK, "UDP socket");
        address.sin_family = AF_INET; address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        require(::bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0, "UDP bind");
        SocketLength size = sizeof(address);
        require(getsockname(fd, reinterpret_cast<sockaddr*>(&address), &size) == 0, "UDP port");
    }
    ~Udp() { if (fd != INVALID_SOCK) ap::net::close_socket(fd); }
};
void send_to(Udp& source, const Udp& dest, const unsigned char* p, int size) {
    require(sendto(source.fd, reinterpret_cast<const char*>(p), size, 0,
        reinterpret_cast<const sockaddr*>(&dest.address), sizeof(dest.address)) == size, "UDP send");
}
std::vector<unsigned char> fixture() {
    const auto* codec = avcodec_find_encoder(AV_CODEC_ID_ALAC);
    require(codec != nullptr, "ALAC encoder");
    auto* encoder = avcodec_alloc_context3(codec);
    encoder->sample_rate = 44100; encoder->time_base = {1, 44100};
    encoder->sample_fmt = AV_SAMPLE_FMT_S16P;
    av_channel_layout_default(&encoder->ch_layout, 2);
    require(avcodec_open2(encoder, codec, nullptr) >= 0, "ALAC encoder open");
    auto* frame = av_frame_alloc();
    frame->format = encoder->sample_fmt; frame->sample_rate = 44100; frame->nb_samples = 352;
    av_channel_layout_copy(&frame->ch_layout, &encoder->ch_layout);
    require(av_frame_get_buffer(frame, 0) >= 0, "ALAC sample allocation");
    for (int c = 0; c < 2; ++c)
        for (int i = 0; i < 352; ++i)
            reinterpret_cast<int16_t*>(frame->data[c])[i] = static_cast<int16_t>((i * 1709 + c * 307) % 30000);
    require(avcodec_send_frame(encoder, frame) >= 0, "ALAC encode");
    avcodec_send_frame(encoder, nullptr);
    auto* packet = av_packet_alloc();
    require(avcodec_receive_packet(encoder, packet) >= 0, "ALAC encoded packet");
    std::vector<unsigned char> result(packet->data, packet->data + packet->size);
    av_packet_free(&packet); av_frame_free(&frame); avcodec_free_context(&encoder);
    require(result.size() > 100, "Substantial audio fixture");
    return result;
}
void clock_tests() {
    using ap::audio::flush_timestamp;
    require(flush_timestamp("seq=65535;rtptime=0") == 0, "FLUSH accepts RTP zero");
    require(flush_timestamp(" rtptime=4294967295 ;seq=1") == 0xffffffffU, "FLUSH maximum RTP timestamp");
    require(!flush_timestamp("rtptime=-1") && !flush_timestamp("rtptime=4294967296") &&
            !flush_timestamp("rtptime=42junk"), "Reject invalid FLUSH boundaries");
    uint32_t position = 0xfffffffeU;
    std::vector<int16_t> pcm{1,1,2,2,3,3,4,4};
    require(ap::audio::trim_before(position, pcm, 0) == 4 && position == 0 && pcm.front() == 3,
            "Partial PCM FLUSH across RTP rollover preserves the next sample");
    require(ap::audio::trim_before(position, pcm, 0xffffffffU) == 0 && pcm.size() == 4,
            "An expired FLUSH preserves future PCM");
    using ap::airplay::NtpClock;
    NtpClock clock;
    const auto r = (400000ULL << 32);
    require(!clock.local_time(r, 0), "No fabricated clock before synchronization");
    require(clock.observe(1'000'000'000, 1'020'000'000, r, r), "Clock observation");
    require(clock.local_time(r + (3ULL << 32), 1'020'000'000) == 4'010'000'000LL, "Three-second advance with arbitrary sender epoch");
    require(!clock.observe(0, 2'000'000'000, r, r), "Reject excessive RTT");
    require(!clock.local_time(r, 32'000'000'000LL), "Expire stale samples");
    require(NtpClock::delta_ns(1ULL << 32, 0xffffffff00000000ULL) == 2'000'000'000LL, "NTP seconds rollover");
    require(NtpClock::delta_ns(10ULL << 32, (10ULL << 32) | 0x80000000ULL) == -500'000'000LL, "Fractional negative delta");
}
int main() {
    try {
        require(ap::net::global_init(), "Winsock");
        SDL_setenv("SDL_AUDIODRIVER", "dummy", 1);
        clock_tests();
        Udp timing, responder, sender;
        std::atomic<bool> running{true};
        std::thread remote([&] {
            while (running) {
                fd_set set; FD_ZERO(&set); FD_SET(responder.fd, &set);
                timeval timeout{0, 100000};
                if (select(static_cast<int>(responder.fd + 1), &set, nullptr, nullptr, &timeout) <= 0) continue;
                unsigned char request[32], response[32]{};
                sockaddr_in peer{}; SocketLength length = sizeof(peer);
                if (recvfrom(responder.fd, reinterpret_cast<char*>(request), 32, 0, reinterpret_cast<sockaddr*>(&peer), &length) != 32) continue;
                response[0] = 0x80; response[1] = 0xd3;
                std::memcpy(response + 8, request + 24, 8);
                const auto stamp = remote_now(); put64(response + 16, stamp); put64(response + 24, stamp);
                sendto(responder.fd, reinterpret_cast<char*>(response), 32, 0, reinterpret_cast<sockaddr*>(&peer), length);
            }
        });
        struct Join { std::thread& t; std::atomic<bool>& run; ~Join() { run = false; t.join(); } } join{remote, running};
        ap::airplay::NtpClient ntp;
        require(ntp.start(timing.fd, "127.0.0.1", ntohs(responder.address.sin_port)), "NTP start");
        for (int i = 0; i < 200 && !ntp.local_time_ns(remote_now()); ++i) std::this_thread::sleep_for(10ms);
        const auto mapped = ntp.local_time_ns(remote_now());
        require(mapped && std::abs(*mapped - now_ns()) < 50'000'000, "Native UDP NTP maps sender epoch to steady clock");
        Udp data, control;
        ap::audio::AudioReceiver receiver;
        ap::audio::AudioReceiver::Config config;
        config.data_sock = data.fd; config.control_sock = control.fd; config.ntp = &ntp;
        config.aes_key.resize(16); config.aes_iv.resize(16); config.ct = 2; config.synchronize = true;
        require(receiver.start(config), "Audio receiver start");
        // Receiver owns data socket after start; retain its address for sends.
        data.fd = INVALID_SOCK;
        const auto payload = fixture();
        auto send_audio = [&](uint16_t seq, uint32_t rtp, int lead_seconds) {
            unsigned char anchor[20]{}; anchor[0] = 0x80; anchor[1] = 0xd4;
            put32(anchor + 4, rtp - lead_seconds * 44100); put64(anchor + 8, remote_now()); put32(anchor + 16, rtp);
            send_to(sender, control, anchor, 20);
            std::this_thread::sleep_for(10ms);
            std::vector<unsigned char> packet(payload.size() + 12);
            packet[0] = 0x80; packet[1] = 0x60; packet[2] = static_cast<unsigned char>(seq >> 8); packet[3] = static_cast<unsigned char>(seq); put32(packet.data() + 4, rtp);
            auto* aes = EVP_CIPHER_CTX_new();
            EVP_EncryptInit_ex(aes, EVP_aes_128_cbc(), nullptr, config.aes_key.data(), config.aes_iv.data());
            EVP_CIPHER_CTX_set_padding(aes, 0);
            const int blocks = static_cast<int>(payload.size() / 16 * 16); int out = 0;
            require(EVP_EncryptUpdate(aes, packet.data() + 12, &out, payload.data(), blocks) == 1, "AES encrypt");
            std::memcpy(packet.data() + 12 + out, payload.data() + blocks, payload.size() - blocks);
            EVP_CIPHER_CTX_free(aes);
            send_to(sender, data, packet.data(), static_cast<int>(packet.size()));
        };
        // First sync anchor crosses the 32-bit RTP timestamp rollover.
        const auto start = now_ns(); send_audio(1, 1000, 3);
        std::this_thread::sleep_for(2500ms);
        require(played == 0, "Audio sent 3 seconds early must not play on arrival");
        for (int i = 0; i < 150 && played == 0; ++i) std::this_thread::sleep_for(10ms);
        require(played == 1, "Scheduled audio eventually plays");
        const auto elapsed_ms = (first_played - start) / 1'000'000;
        require(elapsed_ms >= 2800 && elapsed_ms <= 3200, "Playback follows three-second RTP/NTP deadline");
        std::printf("PASS: audio arriving 3s early submitted at %lld ms\n", static_cast<long long>(elapsed_ms));
        send_audio(2, 22345678, 1); std::this_thread::sleep_for(100ms); receiver.flush(22345778);
        std::this_thread::sleep_for(1200ms);
        require(played == 2, "RTP FLUSH retains future PCM instead of losing the entire block");
        send_audio(3, 22355678, 1); std::this_thread::sleep_for(100ms); receiver.flush();
        std::this_thread::sleep_for(1200ms);
        require(played == 2, "FLUSH without a boundary cancels all scheduled PCM");
        receiver.set_synchronized(false); send_audio(4, 32345678, 3);
        for (int i = 0; i < 50 && played == 2; ++i) std::this_thread::sleep_for(10ms);
        require(played == 3, "Mirroring retains immediate audio submission");
        receiver.set_synchronized(true); send_audio(5, 42345678, 4);
        const auto stopped = Clock::now(); receiver.stop();
        require(Clock::now() - stopped < 500ms, "Stop interrupts a future scheduled frame");
        ntp.stop();
        SDL_Quit(); ap::net::global_shutdown();
        std::puts("PASS: NTP mapping, rollover, stale clock, timestamp playout, FLUSH, mirror mode and bounded shutdown");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
}
