#include "upnp.hpp"

#ifdef USE_UPNP
#include <miniupnpc/miniupnpc.h>
#include <miniupnpc/upnpcommands.h>
#include <miniupnpc/upnperrors.h>
#endif

#include <print>
#include <cstring>
#include <string>
#include <limits>
#include <stdexcept>

// ── UPnP port forwarding ──
#ifdef USE_UPNP
bool     g_upnp_active = false;
uint16_t g_upnp_port   = 0;
UPNPUrls g_upnp_urls{};
IGDdatas g_upnp_data{};
char     g_upnp_lan_addr[64]{};

bool upnp_add_mapping(uint16_t port) {
    if (g_upnp_active) return false; 

    struct UPNPDev* devlist = upnpDiscover(2000, nullptr, nullptr, 0, 0, 2, nullptr);
    if (!devlist) return false;
    
    int igd = UPNP_GetValidIGD(devlist, &g_upnp_urls, &g_upnp_data, g_upnp_lan_addr, sizeof(g_upnp_lan_addr), nullptr, 0);
    freeUPNPDevlist(devlist);
    
    if (igd != 1 && igd != 2) { FreeUPNPUrls(&g_upnp_urls); return false; }
    
    std::string port_str = std::to_string(port);
    
    int r = UPNP_AddPortMapping(g_upnp_urls.controlURL, g_upnp_data.first.servicetype,
                                port_str.c_str(), port_str.c_str(), g_upnp_lan_addr, "ns-backend", "UDP", nullptr, "0");
    if (r != 0) { FreeUPNPUrls(&g_upnp_urls); return false; }
    
    g_upnp_active = true;
    g_upnp_port = port;
    char external_ip[40];
    if (UPNP_GetExternalIPAddress(g_upnp_urls.controlURL, g_upnp_data.first.servicetype, external_ip) == 0) {
        std::println("UPnP: UDP port {} successfully forwarded!", port);
        std::println("UPnP: Tell your clients to connect to -> {}:{}", external_ip, port);
    }
    return true;
}

void upnp_remove_mapping(uint16_t) {
    if (!g_upnp_active) return;
    std::string port_str = std::to_string(g_upnp_port);
    UPNP_DeletePortMapping(g_upnp_urls.controlURL, g_upnp_data.first.servicetype, port_str.c_str(), "UDP", nullptr);
    std::println("UPnP: port mapping removed cleanly");
    FreeUPNPUrls(&g_upnp_urls); g_upnp_active = false; g_upnp_port = 0;
}
#else
bool upnp_add_mapping(uint16_t) { return false; }
void upnp_remove_mapping(uint16_t) {}
#endif
