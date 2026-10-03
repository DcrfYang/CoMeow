// mgmp_upnp.h -- asks the home router (UPnP IGD) to forward the game port to this machine, so a host behind NAT can be reached
// by players on the internet without touching the router's settings. Best effort and silent: many routers have UPnP off, and a
// double-NAT (carrier-grade) connection cannot be helped at all -- the server's reachability probe (mgmp_signal: signal_reach)
// is what tells the host whether it worked.
//
// Everything runs on short-lived worker threads (discovery takes a couple of seconds); the game thread only starts, stops and polls.
// The mapping is leased for an hour and renewed while hosting (upnp_update), so a crash leaves nothing behind for long.
#pragma once

#include <cstdint>

namespace mgmp {

enum class UpnpState {
    Idle,       // nothing asked for
    Working,    // looking for the router / adding the mapping
    Mapped,     // the router forwards the port (see upnp_external_ip)
    Failed,     // no router answered, or it refused (see upnp_error)
};

void upnp_start(uint16_t port);        // TCP `port` on the router -> the same port here. No-op while already asked for.
void upnp_stop();                      // removes the mapping (in the background)
void upnp_update();                    // game thread, once per frame: renews the lease
UpnpState   upnp_state();
const char* upnp_external_ip();        // the router's WAN address when it said so, else ""
const char* upnp_error();              // a short English reason when Failed
// The router accepted the mapping but its own WAN address is private (10/8, 172.16/12, 192.168/16, 100.64/10, 169.254/16): it sits behind
// ANOTHER NAT (a second router or the carrier's), so the forward cannot be reached from the internet. State is Failed in that case.
bool        upnp_double_nat();

} // namespace mgmp
