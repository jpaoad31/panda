// lwIP + USB-NCM glue for the panda bridge. Ported from TinyUSB 0.20's
// net_lwip_webserver example (version-matched to board/bridge/vendor/), adapted:
//   - device IP 192.168.4.1/24, DHCP leases 192.168.4.2-.4
//   - DHCP router + DNS options ZEROED so iOS keeps WiFi/cellular as default route
//   - timing from the panda microsecond timer (bridge_now_ms, defined in main_bridge.c)
#include "tusb.h"
#include "dhserver.h"
#include "dnserver.h"
#include "lwip/init.h"
#include "lwip/timeouts.h"
#include "lwip/ethip6.h"
#include "lwip/etharp.h"
#include "netif/ethernet.h"

#define INIT_IP4(a, b, c, d) { PP_HTONL(LWIP_MAKEU32(a, b, c, d)) }

// Provided by the unity TU (microsecond_timer_get()/1000).
uint32_t bridge_now_ms(void);

static struct netif netif_data;

// lwIP virtual MAC; first byte 0x02 = locally administered. Differs from the
// host's by the LSbit toggle in bridge_net_init().
uint8_t tud_network_mac_address[6] = {0x02, 0x02, 0x84, 0x6A, 0x96, 0x00};

static const ip4_addr_t ipaddr  = INIT_IP4(192, 168, 4, 1);
static const ip4_addr_t netmask = INIT_IP4(255, 255, 255, 0);
static const ip4_addr_t gateway = INIT_IP4(0, 0, 0, 0);

static dhcp_entry_t entries[] = {
  {{0}, INIT_IP4(192, 168, 4, 2), 24 * 60 * 60},
  {{0}, INIT_IP4(192, 168, 4, 3), 24 * 60 * 60},
  {{0}, INIT_IP4(192, 168, 4, 4), 24 * 60 * 60},
};

static const dhcp_config_t dhcp_config = {
  .router = INIT_IP4(0, 0, 0, 0),   // no gateway -> iOS keeps its own default route
  .port = 67,
  .dns = INIT_IP4(0, 0, 0, 0),      // no DNS -> host uses its own
  "usb",
  TU_ARRAY_SIZE(entries),
  entries
};

// stack -> host. NON-BLOCKING by design: if the NCM TX buffer is busy, DROP this
// datagram rather than spin. The stock TinyUSB example here was `for(;;){ ...
// tud_task(); }`, which (1) wedges the whole main loop — and with it CAN read, the
// lwIP timers, and ICMP — whenever the host can't drain as fast as we produce
// (e.g. armed/relay-intercept mode roughly doubles CAN traffic via bus0<->bus2
// forwarding), and (2) re-enters this very output path from tud_task() (incoming
// frame -> lwIP input -> ARP/ICMP reply -> linkoutput), which NO_SYS lwIP can't do
// safely. Our traffic is UDP (CAN batches + health pushes) and tolerates loss; the
// service-timer ISR calls tud_task() at 2 kHz, so the TX buffer still drains there.
// Count of outbound datagrams dropped because the NCM TX buffer was busy (the
// non-blocking drop below). Surfaced on the health push so the app can see how
// often we shed under load. Monotonic since boot.
static uint32_t tx_drop_count;
uint32_t bridge_net_tx_drops(void) { return tx_drop_count; }

static err_t linkoutput_fn(struct netif *netif, struct pbuf *p) {
  (void)netif;
  if (!tud_ready()) { return ERR_USE; }
  if (!tud_network_can_xmit(p->tot_len)) {   // NCM TX busy -> drop (non-blocking)
    tx_drop_count++;
    return ERR_OK;
  }
  tud_network_xmit(p, 0);
  return ERR_OK;
}

static err_t ip4_output_fn(struct netif *netif, struct pbuf *p, const ip4_addr_t *addr) {
  return etharp_output(netif, p, addr);
}

static err_t netif_init_cb(struct netif *netif) {
  LWIP_ASSERT("netif != NULL", (netif != NULL));
  netif->mtu = CFG_TUD_NET_MTU;
  netif->flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP | NETIF_FLAG_LINK_UP | NETIF_FLAG_UP;
  netif->state = NULL;
  netif->name[0] = 'E';
  netif->name[1] = 'X';
  netif->linkoutput = linkoutput_fn;
  netif->output = ip4_output_fn;
  return ERR_OK;
}

// host -> stack: a received NCM frame becomes an lwIP pbuf.
bool tud_network_recv_cb(const uint8_t *src, uint16_t size) {
  struct netif *netif = &netif_data;
  if (size) {
    struct pbuf *p = pbuf_alloc(PBUF_RAW, size, PBUF_POOL);
    if (p == NULL) { return false; }
    pbuf_take(p, src, size);
    if (netif->input(p, netif) != ERR_OK) { pbuf_free(p); }
    tud_network_recv_renew();
  }
  return true;
}

// stack -> host: copy an outgoing pbuf into the NCM transmit buffer.
uint16_t tud_network_xmit_cb(uint8_t *dst, void *ref, uint16_t arg) {
  struct pbuf *p = (struct pbuf *)ref;
  (void)arg;
  return pbuf_copy_partial(p, dst, p->tot_len, 0);
}

void tud_network_init_cb(void) {
  // free the pending RX frame, if any (re-init path)
}

static bool dns_query_proc(const char *name, ip4_addr_t *addr) {
  (void)name; (void)addr;
  return false;   // bridge answers no names; host uses its own DNS
}

// lwIP single-threaded (NO_SYS) protect stubs + millisecond time source.
sys_prot_t sys_arch_protect(void) { return 0; }
void sys_arch_unprotect(sys_prot_t pval) { (void)pval; }
uint32_t sys_now(void) { return bridge_now_ms(); }

// TinyUSB timing (no BSP board layer on the panda).
uint32_t tusb_time_millis_api(void) { return bridge_now_ms(); }
void tusb_time_delay_ms_api(uint32_t ms) {
  uint32_t start = bridge_now_ms();
  while ((bridge_now_ms() - start) < ms) { }
}

// Bring up lwIP + the DHCP/DNS servers. Call after tusb_init().
void bridge_net_init(void) {
  struct netif *netif = &netif_data;

  lwip_init();

  netif->hwaddr_len = sizeof(tud_network_mac_address);
  memcpy(netif->hwaddr, tud_network_mac_address, sizeof(tud_network_mac_address));
  netif->hwaddr[5] ^= 0x01;   // lwIP MAC must differ from the host's

  netif = netif_add(netif, &ipaddr, &netmask, &gateway, NULL, netif_init_cb, ethernet_input);
  netif_set_default(netif);
  tud_network_link_state(BOARD_TUD_RHPORT, true);

  while (!netif_is_up(&netif_data)) { }
  while (dhserv_init(&dhcp_config) != ERR_OK) { }
  while (dnserv_init(IP_ADDR_ANY, 53, dns_query_proc) != ERR_OK) { }
}

// Pump lwIP timers. Call every main-loop iteration.
void bridge_net_service(void) {
  sys_check_timeouts();
}

// --- status helpers for the bring-up LED (main_bridge.c) ---

// USB device configured (the NCM network interface is live).
bool bridge_net_usb_ready(void) {
  return tud_ready();
}

// A host holds a DHCP lease: dhserver writes the client MAC into an entry on ACK,
// so a non-zero MAC in any slot means a lease has been handed out.
bool bridge_net_has_dhcp_lease(void) {
  for (unsigned int i = 0U; i < TU_ARRAY_SIZE(entries); i++) {
    const uint8_t *m = entries[i].mac;
    if ((m[0] | m[1] | m[2] | m[3] | m[4] | m[5]) != 0U) { return true; }
  }
  return false;
}
