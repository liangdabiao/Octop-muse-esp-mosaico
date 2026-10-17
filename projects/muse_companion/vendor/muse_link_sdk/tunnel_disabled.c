/* This companion uses the Noise control channel only, never an L3 tunnel. */
#include "tunnel_netif.h"
#include <string.h>
bool tunnel_netif_start(const ip4_addr_t *ip,const ip4_addr_t *mask,const ip4_addr_t *gw)
{ (void)ip;(void)mask;(void)gw;return false; }
void tunnel_netif_set_link(bool up) { (void)up; }
uint8_t tunnel_netif_index(void) { return 0; }
void tunnel_netif_get_stats(tunnel_stats_t *out) { if(out)memset(out,0,sizeof(*out)); }
