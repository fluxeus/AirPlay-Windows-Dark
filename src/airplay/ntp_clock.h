#pragma once

#include <array>
#include <cstdint>
#include <optional>

namespace ap::airplay {

// NTP timestamps may use an epoch unrelated to Windows. Map their differences
// to steady_clock, never system_clock, so changing the PC clock cannot move audio.
class NtpClock {
public:
    static int64_t delta_ns(uint64_t value, uint64_t reference) {
        const uint32_t seconds = static_cast<uint32_t>(value >> 32)
                               - static_cast<uint32_t>(reference >> 32);
        const int64_t signed_seconds = seconds <= 0x7fffffffU
            ? seconds : static_cast<int64_t>(seconds) - 0x100000000LL;
        const int64_t fractions = static_cast<int64_t>(static_cast<uint32_t>(value))
                               - static_cast<uint32_t>(reference);
        return signed_seconds * 1'000'000'000LL + fractions * 1'000'000'000LL / 0x100000000LL;
    }

    bool observe(int64_t sent, int64_t received, uint64_t remote_received,
                 uint64_t remote_sent) {
        const auto processing = delta_ns(remote_sent, remote_received);
        const auto elapsed = received - sent;
        const auto delay = elapsed - processing;
        if (!remote_received || !remote_sent || processing < 0 || elapsed < 0 ||
            elapsed > 1'000'000'000LL || delay < -1'000'000LL || delay > 500'000'000LL)
            return false;
        const auto rtt = delay > 0 ? delay : 0;
        samples_[next_++ % samples_.size()] = {true, remote_sent, received - rtt / 2, received, rtt};
        return true;
    }

    std::optional<int64_t> local_time(uint64_t remote, int64_t now) const {
        const Sample* best = nullptr;
        for (const auto& sample : samples_) {
            if (!sample.valid || now < sample.observed || now - sample.observed > 30'000'000'000LL)
                continue;
            if (!best || sample.rtt < best->rtt) best = &sample;
        }
        if (!best) return {};
        const auto delta = delta_ns(remote, best->remote);
        if (delta < -30'000'000'000LL || delta > 30'000'000'000LL) return {};
        return best->local + delta;
    }

private:
    struct Sample { bool valid = false; uint64_t remote = 0; int64_t local = 0, observed = 0, rtt = 0; };
    std::array<Sample, 8> samples_{};
    unsigned next_ = 0;
};

} // namespace ap::airplay
