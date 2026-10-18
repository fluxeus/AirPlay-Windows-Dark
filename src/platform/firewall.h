#pragma once
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace ap::platform {
enum class FirewallState { Checking, Allowed, NeedsRule, Managed, Unknown };
struct FirewallStatus {
    FirewallState state = FirewallState::Checking;
    bool busy = false;
    std::string message;
};

// A small policy model keeps coverage decisions testable without changing Windows.
struct FirewallRule {
    unsigned profiles = 0;
    int protocol = 256;
    bool block = false;
    bool full_coverage = false;
};
FirewallState evaluate_firewall(unsigned required_profiles, unsigned blocked_profiles,
                               unsigned default_allow_profiles, const std::vector<FirewallRule>& rules);
FirewallStatus inspect_firewall();
int configure_firewall(); // Elevated helper; invoked before receiver startup.

class FirewallMonitor {
public:
    FirewallMonitor();
    ~FirewallMonitor();
    FirewallStatus status() const;
    void allow_local_network();
private:
    void run();
    mutable std::mutex mutex_;
    std::condition_variable changed_;
    FirewallStatus status_;
    bool stopping_ = false, allow_ = false;
    std::thread worker_;
};
} // namespace ap::platform
