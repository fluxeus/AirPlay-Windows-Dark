#include "platform/firewall.h"
#include <chrono>
#include <cstdint>
#include <cwctype>
#include <functional>
#include <sstream>

#if defined(_WIN32)
#include <windows.h>
#include <netfw.h>
#include <shellapi.h>
#include <wrl/client.h>
#endif

namespace ap::platform {
FirewallState evaluate_firewall(unsigned required, unsigned blocked, unsigned defaults,
                               const std::vector<FirewallRule>& rules) {
    if (blocked & required) return FirewallState::Managed;
    unsigned tcp = defaults, udp = defaults;
    for (const auto& rule : rules) {
        if (rule.block && (rule.profiles & required)) return FirewallState::NeedsRule;
        if (!rule.full_coverage) continue;
        if (rule.protocol == 256 || rule.protocol == 6) tcp |= rule.profiles;
        if (rule.protocol == 256 || rule.protocol == 17) udp |= rule.profiles;
    }
    return ((tcp & udp & required) == required) ? FirewallState::Allowed : FirewallState::NeedsRule;
}

#if defined(_WIN32)
namespace {
using Microsoft::WRL::ComPtr;
struct Apartment {
    HRESULT result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    ~Apartment() { if (SUCCEEDED(result)) CoUninitialize(); }
};
struct Bstr {
    BSTR value = nullptr;
    Bstr() = default;
    explicit Bstr(const wchar_t* text) : value(SysAllocString(text)) {}
    ~Bstr() { SysFreeString(value); }
    Bstr(const Bstr&) = delete;
    Bstr& operator=(const Bstr&) = delete;
    std::wstring text() const { return value ? value : L""; }
};
std::wstring executable_path() {
    std::wstring path(32768,L'\0');
    const DWORD length = GetModuleFileNameW(nullptr,path.data(),static_cast<DWORD>(path.size()));
    if (!length || length >= path.size()) return {};
    path.resize(length); return path;
}
std::wstring normalized(std::wstring text) {
    if (text.find(L'%') != std::wstring::npos) {
        const DWORD size=ExpandEnvironmentStringsW(text.c_str(),nullptr,0);
        if (size) { std::wstring expanded(size,L'\0'); if (ExpandEnvironmentStringsW(text.c_str(),expanded.data(),size)) { expanded.resize(size-1); text=expanded; } }
    }
    for (auto& c : text) c = c == L'/' ? L'\\' : static_cast<wchar_t>(std::towlower(c));
    return text;
}
bool any(const std::wstring& text) { return text.empty() || text==L"*"; }
struct Entry {
    ComPtr<INetFwRule> native;
    std::wstring name;
    bool exact_app = false;
    FirewallRule model;
};
bool entries(INetFwRules* rules, const std::wstring& path, std::vector<Entry>& result) {
    ComPtr<IUnknown> unknown;
    ComPtr<IEnumVARIANT> enumeration;
    if (FAILED(rules->get__NewEnum(unknown.GetAddressOf())) || FAILED(unknown.As(&enumeration))) return false;
    VARIANT item; VariantInit(&item);
    ULONG count=0;
    HRESULT next=S_OK;
    while ((next=enumeration->Next(1,&item,&count)) == S_OK) {
        ComPtr<INetFwRule> rule;
        if (item.vt==VT_DISPATCH && item.pdispVal) item.pdispVal->QueryInterface(IID_PPV_ARGS(rule.GetAddressOf()));
        VariantClear(&item);
        if (!rule) continue;
        VARIANT_BOOL enabled=VARIANT_FALSE;
        NET_FW_RULE_DIRECTION direction{}; NET_FW_ACTION action{};
        long profiles=0,protocol=0;
        Bstr app,name,ports,local,remote,service,interfaces;
        if (FAILED(rule->get_Enabled(&enabled)) || !enabled || FAILED(rule->get_Direction(&direction)) || direction!=NET_FW_RULE_DIR_IN ||
            FAILED(rule->get_ApplicationName(&app.value)) || FAILED(rule->get_Action(&action)) ||
            FAILED(rule->get_Profiles(&profiles)) || FAILED(rule->get_Protocol(&protocol)) || FAILED(rule->get_Name(&name.value))) continue;
        const auto application=normalized(app.text());
        const bool exact=application==path;
        if (!exact && !application.empty()) continue;
        // Global allow rules are not proof that all traffic to this executable is allowed.
        if (!exact && action!=NET_FW_ACTION_BLOCK) continue;
        if (protocol!=NET_FW_IP_PROTOCOL_ANY && protocol!=NET_FW_IP_PROTOCOL_TCP && protocol!=NET_FW_IP_PROTOCOL_UDP) continue;
        const bool have_ports=protocol==NET_FW_IP_PROTOCOL_ANY || SUCCEEDED(rule->get_LocalPorts(&ports.value));
        const bool have_scope=SUCCEEDED(rule->get_LocalAddresses(&local.value)) && SUCCEEDED(rule->get_RemoteAddresses(&remote.value)) &&
            SUCCEEDED(rule->get_ServiceName(&service.value)) && SUCCEEDED(rule->get_InterfaceTypes(&interfaces.value));
        VARIANT restricted_interfaces; VariantInit(&restricted_interfaces);
        const HRESULT interfaces_result=rule->get_Interfaces(&restricted_interfaces);
        const bool all_interfaces=SUCCEEDED(interfaces_result) && (restricted_interfaces.vt==VT_EMPTY || restricted_interfaces.vt==VT_NULL);
        VariantClear(&restricted_interfaces);
        // AirPlay negotiates dynamic TCP and UDP ports. A narrow port rule is insufficient.
        const bool coverage=have_ports && any(ports.text()) && have_scope && any(local.text()) &&
            (any(remote.text()) || normalized(remote.text())==L"localsubnet") && service.text().empty() &&
            (interfaces.text().empty() || normalized(interfaces.text())==L"all") && all_interfaces;
        // An unrelated global port/service restriction should not be presented as an app block.
        if (!exact && !coverage) continue;
        result.push_back({rule,name.text(),exact,{static_cast<unsigned>(profiles),static_cast<int>(protocol),action==NET_FW_ACTION_BLOCK,coverage}});
    }
    return next==S_FALSE;
}
bool policy(ComPtr<INetFwPolicy2>& result, long& profiles) {
    return SUCCEEDED(CoCreateInstance(__uuidof(NetFwPolicy2),nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(result.GetAddressOf()))) &&
        SUCCEEDED(result->get_CurrentProfileTypes(&profiles)) && (profiles&7)!=0;
}
unsigned blocked_profiles(INetFwPolicy2* policy, long profiles, unsigned& required, unsigned& defaults) {
    unsigned blocked=0; required=0; defaults=0;
    for (int bit : {1,2,4}) {
        if (!(profiles&bit)) continue;
        const auto profile=static_cast<NET_FW_PROFILE_TYPE2>(bit);
        VARIANT_BOOL enabled=VARIANT_TRUE,block=VARIANT_FALSE;
        NET_FW_ACTION action=NET_FW_ACTION_BLOCK;
        policy->get_FirewallEnabled(profile,&enabled);
        if (!enabled) continue;
        required|=bit;
        if (FAILED(policy->get_BlockAllInboundTraffic(profile,&block)) || block) blocked|=bit;
        if (SUCCEEDED(policy->get_DefaultInboundAction(profile,&action)) && action==NET_FW_ACTION_ALLOW) defaults|=bit;
    }
    return blocked;
}
std::string elevation_request(const std::function<bool()>& stopping) {
    const auto path=executable_path();
    if (path.empty()) return "Could not locate this application.";
    SHELLEXECUTEINFOW launch{};
    launch.cbSize=sizeof(launch); launch.fMask=SEE_MASK_NOCLOSEPROCESS|SEE_MASK_NOASYNC|SEE_MASK_FLAG_NO_UI;
    launch.lpVerb=L"runas"; launch.lpFile=path.c_str(); launch.lpParameters=L"--configure-firewall"; launch.nShow=SW_HIDE;
    if (!ShellExecuteExW(&launch)) return GetLastError()==ERROR_CANCELLED ? "Permission request cancelled." : "Could not request administrator permission.";
    if (!launch.hProcess) return "Could not verify the firewall update.";
    while (WaitForSingleObject(launch.hProcess,250)==WAIT_TIMEOUT) {
        if (stopping()) { CloseHandle(launch.hProcess); return {}; }
    }
    DWORD code=1; GetExitCodeProcess(launch.hProcess,&code); CloseHandle(launch.hProcess);
    return code==0 ? std::string{} : "Windows did not apply the rule. Check your administrator's firewall policy.";
}
} // namespace

FirewallStatus inspect_firewall() {
    Apartment apartment;
    const auto path=normalized(executable_path());
    ComPtr<INetFwPolicy2> firewall; long profiles=0;
    if (FAILED(apartment.result) || path.empty() || !policy(firewall,profiles))
        return {FirewallState::Unknown,false,"Could not check Windows Firewall."};
    unsigned required=0,defaults=0;
    const unsigned blocked=blocked_profiles(firewall.Get(),profiles,required,defaults);
    NET_FW_MODIFY_STATE modify=NET_FW_MODIFY_STATE_OK;
    if (FAILED(firewall->get_LocalPolicyModifyState(&modify))) return {FirewallState::Unknown,false,"Could not read firewall policy."};
    ComPtr<INetFwRules> rules;
    std::vector<Entry> active;
    if (FAILED(firewall->get_Rules(rules.GetAddressOf())) || !entries(rules.Get(),path,active))
        return {FirewallState::Unknown,false,"Could not read Windows Firewall rules."};
    std::vector<FirewallRule> model;
    for (const auto& entry : active) model.push_back(entry.model);
    auto state=evaluate_firewall(required,blocked,defaults,model);
    if (state==FirewallState::NeedsRule && modify!=NET_FW_MODIFY_STATE_OK) state=FirewallState::Managed;
    return {state,false,state==FirewallState::Managed ? "Inbound access is controlled by your administrator or blocked for this network." : ""};
}

int configure_firewall() {
    Apartment apartment;
    const auto path=executable_path();
    ComPtr<INetFwPolicy2> firewall; long profiles=0;
    if (FAILED(apartment.result) || path.empty() || !policy(firewall,profiles)) return 1;
    NET_FW_MODIFY_STATE modify=NET_FW_MODIFY_STATE_OK;
    unsigned required=0,defaults=0;
    if (FAILED(firewall->get_LocalPolicyModifyState(&modify)) || modify!=NET_FW_MODIFY_STATE_OK ||
        blocked_profiles(firewall.Get(),profiles,required,defaults)) return 2;
    ComPtr<INetFwRules> rules;
    std::vector<Entry> existing;
    if (FAILED(firewall->get_Rules(rules.GetAddressOf())) || !entries(rules.Get(),normalized(path),existing)) return 3;
    // Stable per executable: moving the portable app must not alter a different copy's rule.
    uint64_t hash=14695981039346656037ull;
    for (wchar_t c : normalized(path)) { hash^=static_cast<uint64_t>(c); hash*=1099511628211ull; }
    std::wostringstream label; label<<L"AirPlay Windows - "<<std::hex<<hash;
    long allowed_profiles=profiles&7;
    for (const auto& entry : existing)
        if (entry.exact_app && !entry.model.block && entry.name==label.str())
            allowed_profiles|=static_cast<long>(entry.model.profiles&7);
    ComPtr<INetFwRule> rule;
    if (FAILED(CoCreateInstance(__uuidof(NetFwRule),nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(rule.GetAddressOf())))) return 5;
    Bstr name(label.str().c_str()),app(path.c_str()),group(L"AirPlay Windows"),scope(L"LocalSubnet");
    Bstr description(L"Allows AirPlay from devices on the local subnet for this executable and the selected network profiles.");
    if (FAILED(rule->put_Name(name.value)) || FAILED(rule->put_Description(description.value)) ||
        FAILED(rule->put_ApplicationName(app.value)) || FAILED(rule->put_Grouping(group.value)) ||
        FAILED(rule->put_Protocol(NET_FW_IP_PROTOCOL_ANY)) || FAILED(rule->put_Direction(NET_FW_RULE_DIR_IN)) ||
        FAILED(rule->put_Profiles(allowed_profiles)) || FAILED(rule->put_RemoteAddresses(scope.value)) ||
        FAILED(rule->put_Action(NET_FW_ACTION_ALLOW)) || FAILED(rule->put_EdgeTraversal(VARIANT_FALSE)) ||
        FAILED(rule->put_Enabled(VARIANT_TRUE)) || FAILED(rules->Add(rule.Get()))) return 6;
    // Only replace application blocks after the allow rule was written successfully.
    // Never change global policy or another executable's rules; preserve other profiles.
    for (auto& entry : existing) {
        if (!entry.exact_app || !entry.model.block || !(entry.model.profiles&profiles)) continue;
        const unsigned remaining=entry.model.profiles & 7u & ~static_cast<unsigned>(profiles);
        if (FAILED(remaining ? entry.native->put_Profiles(static_cast<long>(remaining)) : entry.native->put_Enabled(VARIANT_FALSE))) return 4;
    }
    return inspect_firewall().state==FirewallState::Allowed ? 0 : 7;
}
#else
FirewallStatus inspect_firewall() { return {FirewallState::Allowed,false,{}}; }
int configure_firewall() { return 1; }
namespace { std::string elevation_request(const std::function<bool()>&) { return "Only available on Windows."; } }
#endif

FirewallMonitor::FirewallMonitor() : worker_([this]{run();}) {}
FirewallMonitor::~FirewallMonitor() {
    { std::lock_guard<std::mutex> lock(mutex_); stopping_=true; }
    changed_.notify_one();
    worker_.join();
}
FirewallStatus FirewallMonitor::status() const { std::lock_guard<std::mutex> lock(mutex_); return status_; }
void FirewallMonitor::allow_local_network() {
    { std::lock_guard<std::mutex> lock(mutex_); if (status_.busy || status_.state!=FirewallState::NeedsRule) return; status_.busy=true; allow_=true; }
    changed_.notify_one();
}
void FirewallMonitor::run() {
    bool first=true;
    for (;;) {
        bool allow=false;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            if (!first) changed_.wait_for(lock,std::chrono::seconds(10),[&]{return stopping_||allow_;});
            first=false;
            if (stopping_) return;
            allow=allow_; allow_=false;
        }
        const std::string error=allow ? elevation_request([&]{std::lock_guard<std::mutex> lock(mutex_);return stopping_;}) : std::string{};
        auto result=inspect_firewall();
        if (!error.empty()) result.message=error;
        { std::lock_guard<std::mutex> lock(mutex_); result.busy=allow_; status_=std::move(result); }
    }
}
} // namespace ap::platform
