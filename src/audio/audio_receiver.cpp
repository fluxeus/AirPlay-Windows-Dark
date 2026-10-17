#include "audio/audio_receiver.h"
#include "airplay/ntp_client.h"
#include "audio/rtp_flush.h"
#include "log.h"
#include "video/video_renderer.h"

#include <chrono>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <sstream>

#include <openssl/evp.h>

#if defined(_WIN32)
    #include <winsock2.h>
#else
    #include <sys/socket.h>
    #include <sys/time.h>
    #include <errno.h>
#endif

namespace ap::audio {
namespace {

constexpr int kRecvTimeoutMs = 200;

void set_recv_timeout(socket_t s, int ms) {
#if defined(_WIN32)
    DWORD timeout = static_cast<DWORD>(ms);
    ::setsockopt(s, SOL_SOCKET, SO_RCVTIMEO,
                 reinterpret_cast<const char*>(&timeout), sizeof(timeout));
#else
    timeval tv;
    tv.tv_sec  = ms / 1000;
    tv.tv_usec = (ms % 1000) * 1000;
    ::setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif
}

std::string hex_dump(const unsigned char* b, std::size_t n, std::size_t max = 32) {
    std::ostringstream os;
    for (std::size_t i = 0; i < n && i < max; ++i) {
        char tmp[4];
        std::snprintf(tmp, sizeof(tmp), "%02x ", b[i]);
        os << tmp;
    }
    if (n > max) os << "...(" << n << "B)";
    return os.str();
}

} // namespace

// RAOP compression-type values, from UxPlay/global.h and observed sessions.
const char* ct_name(int ct) {
    switch (ct) {
        case 0: return "unspecified";
        case 1: return "PCM";
        case 2: return "ALAC";
        case 3: return "AAC-LC";
        case 4: return "AAC-ELD";      // common on AirPlay 2 (Apple Music)
        case 8: return "AAC-ELD 44.1k";
        default: return "unknown";
    }
}

AudioReceiver::AudioReceiver()  = default;
AudioReceiver::~AudioReceiver() { stop(); }

bool AudioReceiver::start(Config cfg) {
    if (cfg.data_sock == INVALID_SOCK || cfg.sample_rate <= 0) return false;
    if (cfg.aes_key.size() != 16 || cfg.aes_iv.size() != 16) {
        LOG_ERROR << "AudioReceiver: aes_key/aes_iv must be 16 B "
                  << "(got " << cfg.aes_key.size() << '/' << cfg.aes_iv.size() << ')';
        return false;
    }

    cfg_ = std::move(cfg);

    aes_ctx_ = EVP_CIPHER_CTX_new();
    if (!aes_ctx_ ||
        EVP_DecryptInit_ex(aes_ctx_, EVP_aes_128_cbc(), nullptr,
                           cfg_.aes_key.data(), cfg_.aes_iv.data()) != 1) {
        LOG_ERROR << "AudioReceiver: EVP_DecryptInit_ex(aes-128-cbc) failed";
        if (aes_ctx_) { EVP_CIPHER_CTX_free(aes_ctx_); aes_ctx_ = nullptr; }
        return false;
    }
    EVP_CIPHER_CTX_set_padding(aes_ctx_, 0);

    // AAC-ELD decoder — ct==4 or ct==8 per observed iOS streams. We try
    // to init unconditionally; if the codec is something else (PCM, ALAC)
    // the packets will fail to decode and we'll log but not crash.
    decoder_ = std::make_unique<AacDecoder>();
    AacDecoder::Config dc;
    dc.ct          = cfg_.ct;
    dc.sample_rate = cfg_.sample_rate;
    dc.channels    = 2;
    dc.spf         = 480;   // observed frame cadence in logs; 512 also seen
    if (!decoder_->init(dc)) {
        LOG_WARN << "AudioReceiver: AAC decoder init failed — running in "
                    "decrypt-only mode";
        decoder_.reset();
    }

    // SDL-backed audio sink. Goes live only if the OS gave us a device;
    // a headless Linux VM will fall back to silent decode-only mode.
    if (decoder_) {
        output_ = std::make_unique<SdlAudioOutput>();
        if (!output_->start(cfg_.sample_rate, 2)) {
            LOG_WARN << "AudioReceiver: SDL audio output unavailable — "
                        "PCM will be decoded but not played";
            output_.reset();
        }
    }

    set_recv_timeout(cfg_.data_sock, kRecvTimeoutMs);

    running_ = true;
    synchronize_ = cfg_.synchronize && cfg_.ntp;
    playout_thread_ = std::thread(&AudioReceiver::playout_fn, this);
    thread_  = std::thread(&AudioReceiver::thread_fn, this);
    LOG_INFO << "AudioReceiver listening (ct=" << cfg_.ct
             << ' ' << ct_name(cfg_.ct)
             << ", sample_rate=" << cfg_.sample_rate << ')';
    return true;
}

void AudioReceiver::set_volume_db(float db) {
    if (output_) output_->set_volume_db(db);
}

void AudioReceiver::flush(std::optional<uint32_t> boundary) {
    std::lock_guard<std::mutex> lock(pcm_mutex_);
    const auto before = queued_samples_;
    flush_boundary_ = boundary;
    if (!boundary) {
        pcm_queue_.clear();
        queued_samples_ = 0;
    } else {
        for (auto it = pcm_queue_.begin(); it != pcm_queue_.end();) {
            queued_samples_ -= trim_before(it->rtp, it->samples, *boundary);
            if (it->samples.empty()) it = pcm_queue_.erase(it);
            else ++it;
        }
    }
    clear_output_ = true;
    LOG_INFO << "Audio FLUSH: boundary=" << (boundary ? std::to_string(*boundary) : "all")
             << " removed_frames=" << (before - queued_samples_) / 2
             << " retained_frames=" << queued_samples_ / 2;
    pcm_cv_.notify_all();
}

void AudioReceiver::set_synchronized(bool enabled) {
    if (synchronize_.exchange(enabled && cfg_.ntp) != (enabled && cfg_.ntp)) flush();
}

void AudioReceiver::stop() {
    if (!running_.exchange(false)) return;
    pcm_cv_.notify_all();
    if (thread_.joinable()) thread_.join();
    if (playout_thread_.joinable()) playout_thread_.join();
    if (cfg_.data_sock != INVALID_SOCK) ap::net::close_socket(cfg_.data_sock);
    cfg_.data_sock = INVALID_SOCK;

    if (output_)  { output_->stop();  output_.reset();  }
    if (decoder_) {                    decoder_.reset(); }

    if (aes_ctx_) {
        EVP_CIPHER_CTX_free(aes_ctx_);
        aes_ctx_ = nullptr;
    }
}

void AudioReceiver::thread_fn() {
    unsigned char buf[4096];
    unsigned char cleartext[4096];
    uint64_t pkts            = 0;
    uint64_t dedup_dropped   = 0;
    uint64_t total_bytes     = 0;
    uint64_t hist_0_16 = 0, hist_17_64 = 0, hist_65_256 = 0,
             hist_257_512 = 0, hist_513_plus = 0;
    int big_pkts_logged = 0;

    // RAOP retransmits lost packets; iOS typically sends each seq 2-3 times
    // for resilience. We keep a bitset of recently-seen seqs so the decoder
    // only sees each audio frame once. seq is 16-bit — we use a 65 536-bit
    // bitset via a boolean vector (8 KB).
    std::vector<bool> seen_seq(65536, false);

    // Silence watchdog: any valid RTP packet pushes the renderer to
    // "playing"; going idle for more than kSilencePauseMs pushes it to
    // "paused". The rate=1 push is UNCONDITIONAL on each packet — other
    // paths (FLUSH, POST /rate, text/parameters rate:) may have set
    // playing=false without telling us, and push_playback_rate is a
    // cheap atomic exchange that no-ops when the state already matches.
    // Without the unconditional push, a tentative FLUSH that arrives
    // during active playback would leave the overlay stuck until the
    // user manually pauses+resumes.
    constexpr int kSilencePauseMs = 500;
    auto last_packet = std::chrono::steady_clock::now();
    // Arm the silence watchdog only after we've seen at least one real
    // audio packet. Otherwise, a fresh mirror session that starts with
    // iOS's keepalive-only dribble (sub-100B packets) would flip the
    // renderer to rate=0 within 500ms and paint a pause badge over the
    // first video frame, even though the iPhone is actively playing.
    bool audio_ever_seen = false;
    uint64_t sync_packets = 0;

    while (running_.load()) {
        fd_set readable;
        FD_ZERO(&readable);
        FD_SET(cfg_.data_sock, &readable);
        socket_t highest = cfg_.data_sock;
        if (cfg_.control_sock != INVALID_SOCK) {
            FD_SET(cfg_.control_sock, &readable);
            if (cfg_.control_sock > highest) highest = cfg_.control_sock;
        }
        timeval timeout{0, kRecvTimeoutMs * 1000};
        const int ready = ::select(static_cast<int>(highest + 1), &readable, nullptr, nullptr, &timeout);
        if (!running_) break;
        if (ready > 0 && cfg_.control_sock != INVALID_SOCK &&
            FD_ISSET(cfg_.control_sock, &readable)) {
            unsigned char control[2048];
            const int size = ::recvfrom(cfg_.control_sock, reinterpret_cast<char*>(control), sizeof(control), 0, nullptr, nullptr);
            if (size >= 20 && (control[1] & 0x7f) == 0x54) {
                const auto be32 = [&](int offset) {
                    return (static_cast<uint32_t>(control[offset]) << 24)
                         | (static_cast<uint32_t>(control[offset + 1]) << 16)
                         | (static_cast<uint32_t>(control[offset + 2]) << 8)
                         | control[offset + 3];
                };
                const uint64_t ntp = (static_cast<uint64_t>(be32(8)) << 32) | be32(12);
                ++sync_packets;
                {
                    std::lock_guard<std::mutex> lock(pcm_mutex_);
                    sync_rtp_ = be32(4);
                    sync_ntp_ = ntp;
                    have_anchor_ = ntp != 0;
                }
                pcm_cv_.notify_all();
                if (sync_packets <= 3 || sync_packets % 10 == 0)
                    LOG_INFO << "Audio timing anchor: rtp=" << be32(4)
                             << " ntp_raw=" << ntp << " next_rtp=" << be32(16);
            }
        }
        if (ready <= 0 || !FD_ISSET(cfg_.data_sock, &readable)) {
            if (cfg_.renderer && audio_ever_seen &&
                std::chrono::steady_clock::now() - last_packet >= std::chrono::milliseconds(kSilencePauseMs))
                cfg_.renderer->push_playback_rate(0.0f);
            continue;
        }
        int n = ::recvfrom(cfg_.data_sock,
                           reinterpret_cast<char*>(buf), sizeof(buf), 0,
                           nullptr, nullptr);
        if (n < 0) {
            if (cfg_.renderer && audio_ever_seen) {
                const auto idle = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - last_packet).count();
                if (idle >= kSilencePauseMs) {
                    cfg_.renderer->push_playback_rate(0.0f);
                }
            }
            continue;
        }
        if (n < 12) continue;   // RTP header minimum

        // Only count substantial packets (real audio) as proof of
        // active playback. During pause iOS keeps dribbling small
        // keepalive / silence-frame packets (observed around 32 B
        // payload) at the normal packet rate — they used to keep
        // resetting the silence watchdog and flipping rate back to 1.
        // Real AAC-ELD / ALAC audio frames are 400+ bytes once any
        // musical content is present, so a 100-byte threshold separates
        // the two cleanly.
        constexpr int kAudioPacketMinBytes = 100;
        if (n >= kAudioPacketMinBytes) {
            last_packet = std::chrono::steady_clock::now();
            audio_ever_seen = true;
            // Suppress rate=1 during the post-FLUSH grace window: iOS
            // still drains its buffer with real audio for a few hundred
            // ms after a real pause FLUSH.
            if (cfg_.renderer && !cfg_.renderer->in_flush_grace()) {
                cfg_.renderer->push_playback_rate(1.0f);
            }
        }

        // Parse RTP header (RFC 3550, 12-byte fixed part).
        const uint8_t  pt  =  buf[1] & 0x7f;
        const uint16_t seq = (static_cast<uint16_t>(buf[2]) << 8) | buf[3];
        const uint32_t ts  = (static_cast<uint32_t>(buf[4]) << 24)
                           | (static_cast<uint32_t>(buf[5]) << 16)
                           | (static_cast<uint32_t>(buf[6]) <<  8)
                           |  static_cast<uint32_t>(buf[7]);

        const int payload_len   = n - 12;
        const int encrypted_len = (payload_len / 16) * 16;
        const int tail_len      = payload_len - encrypted_len;

        // Reset IV before decrypting each packet — UxPlay does the same
        // via aes_cbc_reset(). The aes_iv from SETUP is therefore used
        // as the IV of every packet (packets are independent).
        EVP_DecryptInit_ex(aes_ctx_, nullptr, nullptr, nullptr,
                           cfg_.aes_iv.data());
        int outlen = 0;
        if (encrypted_len > 0) {
            EVP_DecryptUpdate(aes_ctx_, cleartext, &outlen,
                              buf + 12, encrypted_len);
        }
        if (tail_len > 0) {
            std::memcpy(cleartext + outlen, buf + 12 + encrypted_len,
                        static_cast<std::size_t>(tail_len));
            outlen += tail_len;
        }

        ++pkts;
        total_bytes += static_cast<uint64_t>(payload_len);

        if (payload_len <=  16)      ++hist_0_16;
        else if (payload_len <=  64) ++hist_17_64;
        else if (payload_len <= 256) ++hist_65_256;
        else if (payload_len <= 512) ++hist_257_512;
        else                         ++hist_513_plus;

        // Dedup only the "big enough to be real audio" packets — the dwarf
        // sync packets (4 B) are not audio frames, they come with their own
        // overlapping seq values we don't want to blacklist.
        const bool real_audio = (payload_len >= 100);
        bool is_duplicate     = false;
        if (real_audio) {
            if (seen_seq[seq]) {
                is_duplicate = true;
                ++dedup_dropped;
            } else {
                seen_seq[seq] = true;
                // Clear a sliding 8k-wide window behind the new seq so the
                // bitset doesn't stay full forever once we wrap past 65k.
                const uint16_t erase_from = static_cast<uint16_t>(seq - 8192);
                // erase a small contiguous region every 1024 packets to cap
                // the work per packet.
                if ((pkts & 0x3ff) == 0) {
                    for (int k = 0; k < 1024; ++k) {
                        seen_seq[static_cast<uint16_t>(erase_from + k)] = false;
                    }
                }
            }
        }

        // Verbose log for the first couple of everything, AND for the first
        // 5 non-duplicate "big enough to be real audio" packets. That's
        // where the codec signature lives.
        const bool first_any = (pkts <= 2);
        const bool first_big = (real_audio && !is_duplicate && big_pkts_logged < 5);
        if (first_any || first_big) {
            LOG_INFO << "audio pkt#" << pkts << ": pt=" << static_cast<int>(pt)
                     << " seq=" << seq << " ts=" << ts
                     << " paylen=" << payload_len
                     << " (enc=" << encrypted_len << " tail=" << tail_len
                     << (is_duplicate ? " DUP" : "") << ')';
            LOG_INFO << "  clear: " << hex_dump(cleartext,
                                                static_cast<std::size_t>(outlen));
            if (first_big) ++big_pkts_logged;
        }

        // Feed the real-audio (non-duplicate) frames to the AAC decoder,
        // then immediately drain any PCM it produced into the SDL sink.
        if (real_audio && !is_duplicate && decoder_) {
            int got = decoder_->decode(cleartext, outlen);
            if (got > 0 && decoder_->frames_decoded() <= 3) {
                LOG_INFO << "  decoded " << got << " samples (" << got / 2
                         << " per channel) — total PCM frames out: "
                         << decoder_->frames_decoded();
            }

            if (got > 0) {
                // Drain every available PCM sample from the decoder's
                // queue into SDL. We do it in one pull — buffer is large
                // enough for the ~1920 stereo samples of a single AAC-ELD
                // frame at 480 spf.
                int16_t pcm[8192];
                int     have = 0;
                uint32_t pcm_rtp = ts;
                while ((have = decoder_->pull_pcm_s16(
                            pcm, static_cast<int>(sizeof(pcm) / sizeof(pcm[0])))) > 0) {
                    if (cfg_.renderer) {
                        cfg_.renderer->push_audio_pcm(
                            pcm, have, cfg_.sample_rate, 2);
                    }
                    {
                        std::lock_guard<std::mutex> lock(pcm_mutex_);
                        // Bound decoded PCM to five seconds, including when
                        // sender timing is unavailable or malformed.
                        const auto limit = static_cast<std::size_t>(cfg_.sample_rate) * 2 * 5;
                        while (!pcm_queue_.empty() && queued_samples_ + have > limit) {
                            queued_samples_ -= pcm_queue_.front().samples.size();
                            pcm_queue_.pop_front();
                        }
                        PcmFrame frame{pcm_rtp, std::vector<int16_t>(pcm, pcm + have)};
                        if (flush_boundary_) trim_before(frame.rtp, frame.samples, *flush_boundary_);
                        if (!frame.samples.empty()) {
                            // Boundary reached; normal duplicate/late handling
                            // takes over instead of keeping a fence for hours.
                            flush_boundary_.reset();
                            queued_samples_ += frame.samples.size();
                            pcm_queue_.push_back(std::move(frame));
                        }
                    }
                    pcm_rtp += static_cast<uint32_t>(have / 2);
                    pcm_cv_.notify_one();
                }
            }
        }

        if ((pkts % 500) == 0) {
            const uint64_t queue_ms = output_
                ? static_cast<uint64_t>(output_->queued_bytes()) * 1000 / (cfg_.sample_rate * 2 * sizeof(int16_t))
                : 0;
            LOG_INFO << "audio: " << pkts << " pkts, "
                     << (total_bytes / 1024) << " KB received, rtp=" << ts
                     << " sink_queue=" << queue_ms << " ms, sync_packets=" << sync_packets;
        }
    }

    LOG_INFO << "AudioReceiver stopped (" << pkts << " pkts, "
             << total_bytes << " payload bytes, "
             << dedup_dropped << " duplicate audio frames dropped, "
             << (decoder_ ? decoder_->frames_decoded() : 0)
             << " PCM frames decoded)";
    LOG_INFO << "  payload size histogram: "
             << "[0..16]="      << hist_0_16
             << "  [17..64]="   << hist_17_64
             << "  [65..256]="  << hist_65_256
             << "  [257..512]=" << hist_257_512
             << "  [513+]="     << hist_513_plus;
}

void AudioReceiver::playout_fn() {
    using Clock = std::chrono::steady_clock;
    const auto now_ns = [] {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(
            Clock::now().time_since_epoch()).count();
    };
    // Clock acquisition is a startup grace period, not a new delay to insert
    // every time a FLUSH or pause empties the PCM queue.
    const auto clock_deadline = Clock::now() + std::chrono::seconds(2);
    bool fallback_logged = false;
    bool scheduled_logged = false;
    uint64_t played = 0, late_dropped = 0;
    std::unique_lock<std::mutex> lock(pcm_mutex_);
    while (running_) {
        if (clear_output_) {
            if (output_) output_->clear();
            clear_output_ = false;
        }
        if (pcm_queue_.empty()) {
            pcm_cv_.wait_for(lock, std::chrono::milliseconds(20));
            continue;
        }

        std::optional<int64_t> target;
        if (synchronize_ && cfg_.ntp && have_anchor_) {
            target = cfg_.ntp->local_time_ns(sync_ntp_);
            if (target) {
                const uint32_t ticks = pcm_queue_.front().rtp - sync_rtp_;
                const int64_t delta = ticks <= 0x7fffffffU
                    ? ticks : static_cast<int64_t>(ticks) - 0x100000000LL;
                *target += delta * 1'000'000'000LL / cfg_.sample_rate;
            }
        }
        if (synchronize_ && !target && cfg_.ntp && cfg_.ntp->running() && Clock::now() < clock_deadline) {
            pcm_cv_.wait_for(lock, std::chrono::milliseconds(10));
            continue;
        }
        if (synchronize_ && !target && !fallback_logged) {
            LOG_WARN << "Audio sync unavailable; playing without timestamp scheduling (no repeated startup wait)";
            fallback_logged = true;
        }
        const int64_t pending = output_ ? output_->pending_ns() : 0;
        // Keep future PCM under our control: SDL can only flush its entire queue.
        if (synchronize_ && pending > 60'000'000LL) {
            pcm_cv_.wait_for(lock, std::chrono::milliseconds(5));
            continue;
        }
        const int64_t advance = target ? *target - now_ns() - pending : 0;
        if (target && !scheduled_logged) {
            LOG_INFO << "Audio synchronized playout: initial_wait_ms=" << advance / 1'000'000;
            scheduled_logged = true;
        }
        // Ignore broken anchors rather than buffering forever.
        if (target && advance > 5'000'000'000LL) {
            target.reset();
        } else if (target && advance > 1'000'000LL) {
            pcm_cv_.wait_for(lock, std::chrono::nanoseconds(std::min<int64_t>(advance, 10'000'000LL)));
            continue;
        } else if (target && advance < -250'000'000LL) {
            queued_samples_ -= pcm_queue_.front().samples.size();
            pcm_queue_.pop_front();
            ++late_dropped;
            if (late_dropped == 1 || late_dropped % 500 == 0)
                LOG_WARN << "Audio late frames dropped=" << late_dropped
                         << " behind_ms=" << -advance / 1'000'000;
            continue;
        }

        auto frame = std::move(pcm_queue_.front());
        pcm_queue_.pop_front();
        queued_samples_ -= frame.samples.size();
        // Keep flush and enqueue serialized: after FLUSH is observed no
        // already-dequeued PCM can be reinserted into SDL by this thread.
        if (output_) output_->push(frame.samples.data(), static_cast<int>(frame.samples.size()));
        if (++played <= 3 || played % 500 == 0)
            LOG_INFO << "Audio playout: synchronized=" << (target ? 1 : 0)
                     << " remaining=" << queued_samples_ / 2 << " samples, sink_ms="
                     << pending / 1'000'000 << " late_dropped=" << late_dropped;
    }
    pcm_queue_.clear();
    queued_samples_ = 0;
}

} // namespace ap::audio
