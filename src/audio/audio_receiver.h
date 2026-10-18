#pragma once

#include "audio/aac_decoder.h"
#include "audio/audio_output.h"
#include "net/socket.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>
#include <deque>
#include <condition_variable>
#include <mutex>
#include <optional>

// Forward-declare OpenSSL type to keep this header self-contained.
struct evp_cipher_ctx_st;
typedef struct evp_cipher_ctx_st EVP_CIPHER_CTX;

namespace ap::video { class VideoRenderer; }
namespace ap::airplay { class NtpClient; }

namespace ap::audio {

// UDP audio receiver for the AirPlay RAOP stream (type 96).
//
// Ported from UxPlay's raop_buffer.c (GPL-3.0). Packets arrive on the
// UDP data port negotiated during SETUP. Each packet is:
//
//   [12 bytes RTP header][AES-128-CBC encrypted payload]
//
// The AES key/IV come from SETUP (the same `aes_key` post-hashed against
// the ECDH secret, and `aes_iv` as-is). For every packet, the AES context
// is re-initialised with the SAME iv (UxPlay calls this "aes_cbc_reset"
// after each decrypt) so packets stay independent — trailing bytes that
// don't fit into a full 16-byte block are copied unchanged.
//
// The decrypted payload is a codec frame (ALAC / AAC-ELD / AAC-LC
// depending on the `ct` negotiated in SETUP). A network thread decodes
// to bounded PCM; a playout thread schedules audio-only playback using
// RTP/NTP anchors. Mirroring retains immediate audio submission.
class AudioReceiver {
public:
    struct PcmFrame {
        uint32_t rtp;
        std::vector<int16_t> samples;
        int64_t local_target_ns = 0; // retained PCM while the new RTP clock is pending
    };
    struct PauseState {
        std::deque<PcmFrame> pcm;
        std::unique_ptr<SdlAudioOutput> output;
        int sample_rate = 0;
        int64_t paused_at_ns = 0;
        uint32_t source_lead_frames = 0;
    };
    struct Config {
        socket_t                    data_sock = INVALID_SOCK; // ownership transferred
        socket_t                    control_sock = INVALID_SOCK; // borrowed; owner closes after stop
        ap::airplay::NtpClient*      ntp = nullptr; // borrowed; outlives receiver
        bool                        synchronize = false; // audio-only playback
        bool                        preserve_paused_audio = true;
        std::vector<unsigned char>  aes_key;                   // 16 B
        std::vector<unsigned char>  aes_iv;                    // 16 B
        int                         ct          = 0;           // compression type
        int                         sample_rate = 44100;
        // Non-owning. When set, the receiver acts as a play/pause
        // watchdog: it pushes rate 1 on any RTP packet and rate 0 after
        // ~500 ms of silence. Apple Music and many iOS apps signal
        // pause solely by stopping the RTP flow — no RTSP verb or
        // text/parameters rate: update is sent — so this is the only
        // reliable way to drive the UI pause state.
        ap::video::VideoRenderer*   renderer    = nullptr;
    };

    AudioReceiver();
    ~AudioReceiver();

    AudioReceiver(const AudioReceiver&)            = delete;
    AudioReceiver& operator=(const AudioReceiver&) = delete;

    bool start(Config cfg, PauseState state = {});
    void stop();
    PauseState suspend();
    void discard_pause();

    // Thread-safe: hand to SdlAudioOutput when it exists.
    void set_volume_db(float db);
    void set_synchronized(bool enabled);
    void flush(std::optional<uint32_t> boundary = {});
    void resume();

private:
    void thread_fn();
    void playout_fn();
    void release_resources();

    Config                      cfg_;
    std::atomic<bool>           running_{false};
    std::thread                 thread_;
    std::thread                 playout_thread_;
    std::mutex pcm_mutex_;
    std::condition_variable pcm_cv_;
    std::deque<PcmFrame> pcm_queue_;
    std::size_t queued_samples_ = 0;
    uint32_t sync_rtp_ = 0;
    uint64_t sync_ntp_ = 0;
    bool have_anchor_ = false;
    bool clear_output_ = false;
    std::optional<uint32_t> flush_boundary_;
    std::deque<PcmFrame> paused_pcm_;
    int64_t paused_at_ns_ = 0;
    bool imported_pause_ = false;
    uint32_t source_lead_frames_ = 0;
    bool have_first_packet_ = false;
    uint32_t first_packet_rtp_ = 0;
    int64_t first_packet_ns_ = 0;
    std::atomic<bool> synchronize_{false};
    EVP_CIPHER_CTX*                 aes_ctx_{nullptr};
    std::unique_ptr<AacDecoder>     decoder_;
    std::unique_ptr<SdlAudioOutput> output_;
};

// Human-readable label for a RAOP "ct" (compression type) value.
const char* ct_name(int ct);

} // namespace ap::audio
