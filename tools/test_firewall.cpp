#include "platform/firewall.h"
#include <iostream>
#include <stdexcept>
#include <string>
using namespace ap::platform;
static void require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
int main(int argc,char** argv) {
    try {
        if (argc>1 && std::string(argv[1])=="--inspect") {
            const auto result=inspect_firewall();
            std::cout<<"Windows Firewall state="<<static_cast<int>(result.state)<<" "<<result.message<<'\n';
            return result.state==FirewallState::Checking ? 1 : 0;
        }
        require(evaluate_firewall(2,0,0,{})==FirewallState::NeedsRule,"missing rule not detected");
        require(evaluate_firewall(2,0,0,{{2,256,false,true}})==FirewallState::Allowed,"application allow not detected");
        require(evaluate_firewall(2,0,0,{{2,6,false,true}})==FirewallState::NeedsRule,"UDP coverage missing");
        require(evaluate_firewall(2,0,0,{{2,6,false,true},{2,17,false,true}})==FirewallState::Allowed,"split TCP/UDP rules rejected");
        require(evaluate_firewall(2,0,0,{{4,256,false,true}})==FirewallState::NeedsRule,"wrong profile accepted");
        require(evaluate_firewall(6,0,0,{{2,256,false,true}})==FirewallState::NeedsRule,"multiple active profiles ignored");
        require(evaluate_firewall(6,0,0,{{2,256,false,true},{4,256,false,true}})==FirewallState::Allowed,"multi-profile coverage rejected");
        require(evaluate_firewall(2,0,0,{{2,256,false,true},{2,6,true,true}})==FirewallState::NeedsRule,"block precedence ignored");
        require(evaluate_firewall(2,2,0,{{2,256,false,true}})==FirewallState::Managed,"block-all policy ignored");
        require(evaluate_firewall(2,0,0,{{2,256,false,false}})==FirewallState::NeedsRule,"restricted rule accepted as full coverage");
        require(evaluate_firewall(0,0,0,{})==FirewallState::Allowed,"disabled firewall reported blocked");
        require(evaluate_firewall(6,0,2,{{4,256,false,true}})==FirewallState::Allowed,"default allow ignored");
        std::cout<<"Firewall profile, protocol, scope and block-precedence checks passed\n";
        return 0;
    } catch (const std::exception& error) { std::cerr<<error.what()<<'\n';return 1; }
}
