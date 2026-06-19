// RoadStud CAN-over-UDP bridge logic (panda side). See bridge_can.h.
//
// Ported from canbridge/src/main_usb.c (the known-good Pico implementation).
// Differences from the Pico:
//   - CAN source is the panda FDCAN RX queue (can_rx_q) instead of an MCP2515.
//   - CAN TX uses can_send() instead of the MCP2515 can_write().
//   - Frame translation uses bridge_protocol.h on CANPacket_t (not the Pico's
//     can_frame_t), but the on-the-wire bytes are identical.
//
// COMPILE NOTE: this file needs the lwIP raw API headers on the include path,
// which only exist once the lwIP + USB-NCM layer is vendored into this build
// (see board/bridge/README.md). It is intentionally not part of the default
// panda_h7 firmware build.

#include "bridge_can.h"

#include "lwip/udp.h"
#include "lwip/pbuf.h"
#include "lwip/ip_addr.h"

// CANPacket_t (clean, no CMSIS deps).
#include "opendbc/safety/can.h"
#include "board/bridge/bridge_protocol.h"

// ControlPacket_t + comms_control_handler (the panda's full USB control surface).
// The handler is defined in the unity TU (main_bridge.c includes main_comms.h);
// here we only need the declaration. No CMSIS deps.
#include "board/comms_definitions.h"

// can_ring is panda-internal (board/drivers/drivers.h) and drags in CMSIS-typed
// driver decls. We only ever pass &can_rx_q to can_pop and never touch its fields,
// so an opaque forward declaration keeps those heavy headers out of this TU. The
// real definitions live in the unity TU (main_bridge.c).
typedef struct can_ring can_ring;
extern can_ring can_rx_q;
bool can_pop(can_ring *q, CANPacket_t *elem);
void can_send(CANPacket_t *to_push, uint8_t bus_number, bool skip_tx_hook);

// Provided by main_bridge.c: set the bootstub re-entry magic + reset. The NCM app
// has no panda control endpoint, so a host can't request enter-bootstub the usual
// way; a magic UDP datagram (below) is the escape hatch instead.
//   dfu=false -> soft-flasher (reflash the app);  dfu=true -> ST ROM DFU (reflash the bootstub).
extern void bridge_request_reboot(bool dfu);

// Provided by lwip_glue.c: count of outbound datagrams dropped on a busy NCM TX
// buffer. Appended to the health push as a bridge-stats trailer (see bridge_send_health).
extern uint32_t bridge_net_tx_drops(void);

#define BRIDGE_UDP_PORT       5555U
#define HEARTBEAT_TIMEOUT_MS  500U
#define KEEPALIVE_INTERVAL_MS 250U

static struct udp_pcb *can_udp;
static ip_addr_t  client_addr;
static uint16_t   client_port;
static bool       client_connected;
static uint32_t   last_client_packet_ms;
static uint32_t   last_send_ms;
static uint32_t   last_can_frame_ms;   // last real CAN frame -> client (not a keepalive)
// Updated at the top of every bridge_can_poll(); read by the lwIP receive
// callback (which fires within the same NO_SYS poll loop) so it can timestamp
// client activity without plumbing now_ms through lwIP.
static uint32_t   current_ms;

static uint8_t tx_buf[BRIDGE_MAX_FRAMES_PER_UDP * BRIDGE_MAX_PACKET];
static int     tx_buf_len;
static uint32_t tx_buf_first_ms;   // when the current batch's first frame was queued

// Coalescing: hold a partial datagram up to this long before flushing, so we pack
// many CAN frames per UDP packet instead of ~1. The main loop spins far faster than
// frames arrive (~1 frame/550us at 1800 fps), so flushing every poll sent ~1.06
// frames/packet — paying the full per-datagram lwIP/pbuf/checksum tax ~1800x/s. This
// bounds the added car->app latency; the near-full check still bounds datagram size.
#define BRIDGE_TX_COALESCE_MS 5U

// --- RSCP control channel (RoadStud Control Protocol) ------------------------
// Tunnels the panda's native USB control transfers over UDP, so the app can reach
// the full comms_control_handler surface (health, version, safety mode, CAN speed,
// ...) the proprietary USB driver exposed. The request is recognised in the lwIP
// receive callback but DEFERRED to bridge_can_poll (main-loop context) before
// calling the handler — some requests re-init CAN / touch the safety hooks (e.g.
// set_safety_mode), which must not run from the lwIP/USB callback context. A single
// pending slot is enough: control traffic is low-rate. The app retransmits a request
// (same req_id) if it misses the reply; we cache the last response and re-ACK a
// retransmit WITHOUT re-running the handler, so non-idempotent OUT commands
// (set_safety_mode, set_obd, ...) execute exactly once.
//
//   request  "RSCP": [0..3]magic [4]ver=1 [5]opcode(bRequest) [6]req_id [7]flags
//                    [8..9]param1(LE) [10..11]param2(LE) [12..13]resp_cap(LE) [14..]payload
//   response "RSCR": [0..3]magic [4]ver  [5]opcode(echo) [6]req_id(echo) [7]status
//                    [8..9]payload_len(LE) [10..]payload (handler resp bytes)
#define RSCP_HDR_LEN 14U
#define RSCR_HDR_LEN 10U
#define RSCP_VERSION 1U

static bool            ctrl_pending;
static ControlPacket_t ctrl_req;
static uint8_t         ctrl_req_id;

// Last handled request's reply, cached for idempotent re-ACK of retransmits. The
// app's req_id is 1..255 (never 0), so last_done_reqid == 0 means "nothing yet".
static uint8_t  last_done_reqid;
static uint16_t cached_resp_len;
static uint8_t  cached_resp[RSCR_HDR_LEN + 64];

// Periodic unsolicited health push throttle (see bridge_send_health).
static uint32_t last_health_ms;

static void flush_tx_buffer(void) {
  if ((tx_buf_len == 0) || !client_connected) { return; }
  struct pbuf *p = pbuf_alloc(PBUF_TRANSPORT, (u16_t)tx_buf_len, PBUF_RAM);
  if (p == NULL) { return; }
  memcpy(p->payload, tx_buf, (size_t)tx_buf_len);
  udp_sendto(can_udp, p, &client_addr, client_port);
  pbuf_free(p);
  tx_buf_len = 0;
}

// 1-byte keepalive: the app treats any received datagram as proof-of-life, and a
// <=1-byte payload decodes to zero CAN frames. Keeps the link up on an idle bus.
static void send_keepalive(void) {
  if (!client_connected) { return; }
  struct pbuf *p = pbuf_alloc(PBUF_TRANSPORT, 1, PBUF_RAM);
  if (p == NULL) { return; }
  *(uint8_t *)p->payload = 0x00U;
  udp_sendto(can_udp, p, &client_addr, client_port);
  pbuf_free(p);
}

// Send a buffer to the current client (used for RSCR control replies + re-ACKs).
static void send_to_client(const uint8_t *buf, uint16_t len) {
  if (!client_connected) { return; }
  struct pbuf *p = pbuf_alloc(PBUF_TRANSPORT, (u16_t)len, PBUF_RAM);
  if (p == NULL) { return; }
  memcpy(p->payload, buf, (size_t)len);
  udp_sendto(can_udp, p, &client_addr, client_port);
  pbuf_free(p);
}

// Exact 8-byte re-flash sentinel match. Can't collide with a <=1-byte heartbeat,
// and no checksum-valid CAN frame can equal these bytes, so the match is unambiguous.
static bool sentinel_is(const struct pbuf *p, const char *tag) {
  if (p->tot_len != 8U) { return false; }
  const uint8_t *d = (const uint8_t *)p->payload;
  for (uint8_t i = 0U; i < 8U; i++) {
    if (d[i] != (uint8_t)tag[i]) { return false; }
  }
  return true;
}

// app -> car: parse concatenated bridge packets and write each to the bus.
static void udp_recv_cb(void *arg, struct udp_pcb *pcb, struct pbuf *p,
                        const ip_addr_t *addr, u16_t port) {
  (void)arg; (void)pcb;
  if (p == NULL) { return; }

  // Re-flash escape hatch (see README "Re-flashing gotcha"): a host dev script sends
  // these; the iOS app never does. bridge_request_reboot() does not return.
  if (sentinel_is(p, "RSDFUAPP")) { pbuf_free(p); bridge_request_reboot(false); return; }
  if (sentinel_is(p, "RSDFUBTL")) { pbuf_free(p); bridge_request_reboot(true);  return; }

  ip_addr_copy(client_addr, *addr);
  client_port = port;
  client_connected = true;
  last_client_packet_ms = current_ms;

  // RSCP control request: stash it and let bridge_can_poll run the handler in
  // main-loop context (the handler may re-init CAN, unsafe in the lwIP callback).
  // Unambiguous vs a CAN batch: a real frame carries a valid XOR checksum at byte[5]
  // (bridge_protocol.h), while an RSCP packet's byte[5] is an opcode (always >=0xA8,
  // never the 0x13 checksum of "RSCP"+ver). So "magic matches AND byte[5] is NOT a
  // valid CAN checksum" can't be aliased by a genuine CAN frame.
  {
    const uint8_t *d = (const uint8_t *)p->payload;
    if ((p->tot_len >= RSCP_HDR_LEN) &&
        (d[0] == (uint8_t)'R') && (d[1] == (uint8_t)'S') &&
        (d[2] == (uint8_t)'C') && (d[3] == (uint8_t)'P') &&
        ((uint8_t)(d[0] ^ d[1] ^ d[2] ^ d[3] ^ d[4]) != d[5])) {   // not a valid CAN header
      uint8_t reqid = d[6];
      // Retransmit of the request we just handled: re-ACK from cache, don't re-run
      // (so OUT commands like set_safety_mode execute exactly once).
      if ((reqid == last_done_reqid) && (cached_resp_len > 0U)) {
        send_to_client(cached_resp, cached_resp_len);
        pbuf_free(p);
        return;
      }
      ctrl_req.request = d[5];
      ctrl_req.param1  = (uint16_t)((uint16_t)d[8]  | ((uint16_t)d[9]  << 8));
      ctrl_req.param2  = (uint16_t)((uint16_t)d[10] | ((uint16_t)d[11] << 8));
      ctrl_req.length  = (uint16_t)((uint16_t)d[12] | ((uint16_t)d[13] << 8));
      ctrl_req_id  = reqid;
      ctrl_pending = true;
      pbuf_free(p);
      return;
    }
  }

  if (p->tot_len > 1) {
    const uint8_t *data = (const uint8_t *)p->payload;
    int remaining = (int)p->tot_len;
    int offset = 0;
    while (offset < remaining) {
      CANPacket_t pkt;
      int consumed = bridge_decode(&data[offset], remaining - offset, &pkt);
      if (consumed <= 0) { break; }
      // NOTE: skip_tx_hook=false runs the panda safety model. For a forwarding
      // bridge you must set an appropriate safety mode (or pass true to forward
      // raw) — see README "Safety mode".
      can_send(&pkt, pkt.bus, false);
      offset += consumed;
    }
  }
  pbuf_free(p);
}

// Run a deferred RSCP request through the panda comms handler and reply with RSCR.
// Called from bridge_can_poll (main-loop context). resp[64] covers every Phase-1
// read: health_t is 59 B, can_health_t is 64 B, version/serial are smaller; the
// panda EP0 path uses the same 64-byte (USBPACKET_MAX_SIZE) response buffer.
static void process_control(void) {
  if (!ctrl_pending) { return; }
  ctrl_pending = false;

  uint8_t resp[64];
  if (ctrl_req.length > (uint16_t)sizeof(resp)) { ctrl_req.length = (uint16_t)sizeof(resp); }
  int rlen = comms_control_handler(&ctrl_req, resp);   // may not return (reset/DFU opcodes)
  uint16_t plen = (rlen > 0) ? (uint16_t)rlen : 0U;
  if (plen > (uint16_t)sizeof(resp)) { plen = (uint16_t)sizeof(resp); }   // defense-in-depth vs a future handler

  // Build the RSCR into the cache so a retransmit can be re-ACKed without re-running.
  uint8_t *o = cached_resp;
  o[0] = (uint8_t)'R'; o[1] = (uint8_t)'S'; o[2] = (uint8_t)'C'; o[3] = (uint8_t)'R';
  o[4] = RSCP_VERSION;
  o[5] = ctrl_req.request;
  o[6] = ctrl_req_id;
  o[7] = 0U;                          // status: reserved (handler has no error channel)
  o[8] = (uint8_t)(plen & 0xFFU);
  o[9] = (uint8_t)(plen >> 8);
  if (plen > 0U) { memcpy(&o[RSCR_HDR_LEN], resp, plen); }
  cached_resp_len = (uint16_t)(RSCR_HDR_LEN + plen);
  last_done_reqid = ctrl_req_id;

  send_to_client(cached_resp, cached_resp_len);
}

// Periodic unsolicited health push (~2 Hz): an RSCR with req_id=0. The app never uses
// req_id 0, so it routes this to its health cache instead of matching a pending
// request — no new magic, reuses the RSCR framing + isControlResponse check. Serialised
// through the same comms_control_handler as the 0xD2 request path, but kept ENTIRELY
// separate from the request/retransmit cache (local buffers) so it can't be mistaken
// for, or clobber, a real reply.
static void bridge_send_health(uint32_t now_ms) {
  if (!client_connected) { return; }
  if ((now_ms - last_health_ms) < 500U) { return; }   // ~2 Hz
  last_health_ms = now_ms;

  ControlPacket_t pkt;
  pkt.request = 0xD2U;   // health_read
  pkt.param1 = 0U;
  pkt.param2 = 0U;
  pkt.length = 64U;
  uint8_t resp[64];
  int rlen = comms_control_handler(&pkt, resp);
  if (rlen <= 0) { return; }
  uint16_t hlen = (uint16_t)rlen;                              // health_t (59 bytes)
  if (hlen > (uint16_t)sizeof(resp)) { hlen = (uint16_t)sizeof(resp); }

  // Payload = packed health_t, then a bridge-stats trailer (PUSH ONLY — the on-demand
  // 0xD2 reply via process_control has no trailer). See PANDA_HEALTH_PUSH.md / README.
  //   [hlen .. hlen+4)        uint32 LE  tx_drops            (NCM-busy datagram drops, since boot)
  //   [hlen+4 .. +3*64)       can_health_t x3 (bus 0,1,2)    (raw 0xC2 layout — bus_off,
  //                                                           TEC/REC, total_error_cnt, irq rates;
  //                                                           for armed-mode error-storm diagnosis)
  uint8_t out[RSCR_HDR_LEN + 80U + (3U * 64U)];
  out[0] = (uint8_t)'R'; out[1] = (uint8_t)'S'; out[2] = (uint8_t)'C'; out[3] = (uint8_t)'R';
  out[4] = RSCP_VERSION;
  out[5] = 0xD2U;        // opcode echo
  out[6] = 0U;           // req_id = 0 -> unsolicited push
  out[7] = 0U;           // status: ok
  memcpy(&out[RSCR_HDR_LEN], resp, hlen);
  uint16_t off = hlen;

  uint32_t drops = bridge_net_tx_drops();
  out[RSCR_HDR_LEN + off + 0U] = (uint8_t)(drops & 0xFFU);
  out[RSCR_HDR_LEN + off + 1U] = (uint8_t)((drops >> 8) & 0xFFU);
  out[RSCR_HDR_LEN + off + 2U] = (uint8_t)((drops >> 16) & 0xFFU);
  out[RSCR_HDR_LEN + off + 3U] = (uint8_t)((drops >> 24) & 0xFFU);
  off += 4U;

  // Per-bus can_health via the same 0xC2 handler the app already knows (it refreshes
  // PSR/ECR via update_can_health_pkt). Lets the app log bus_off + error counters to
  // catch the armed-mode CAN error storm. Bounded so out[] can never overflow.
  for (uint8_t bus = 0U; bus < 3U; bus++) {
    ControlPacket_t cp;
    cp.request = 0xC2U;
    cp.param1 = bus;
    cp.param2 = 0U;
    cp.length = 64U;
    uint8_t ch[64];
    int chlen = comms_control_handler(&cp, ch);
    if (chlen <= 0) { continue; }
    uint16_t n = (uint16_t)chlen;
    if (n > (uint16_t)sizeof(ch)) { n = (uint16_t)sizeof(ch); }
    if (((uint32_t)off + n) > (uint32_t)(sizeof(out) - RSCR_HDR_LEN)) { break; }
    memcpy(&out[RSCR_HDR_LEN + off], ch, n);
    off += n;
  }

  out[8] = (uint8_t)(off & 0xFFU);
  out[9] = (uint8_t)(off >> 8);
  send_to_client(out, (uint16_t)(RSCR_HDR_LEN + off));
}

void bridge_can_init(void) {
  can_udp = udp_new();
  udp_bind(can_udp, IP_ADDR_ANY, BRIDGE_UDP_PORT);
  udp_recv(can_udp, udp_recv_cb, NULL);
  client_connected = false;
  tx_buf_len = 0;
}

bool bridge_can_client_connected(void) {
  return client_connected;
}

// True while real CAN frames (not just keepalives) reached the client recently.
bool bridge_can_frames_flowing(uint32_t now_ms) {
  return client_connected && (last_can_frame_ms != 0U) && ((now_ms - last_can_frame_ms) < 1000U);
}

void bridge_can_poll(uint32_t now_ms) {
  current_ms = now_ms;   // visible to udp_recv_cb (same NO_SYS loop)

  process_control();     // run any RSCP request the lwIP callback deferred

  // Drop the client after silence.
  if (client_connected && ((now_ms - last_client_packet_ms) > HEARTBEAT_TIMEOUT_MS)) {
    client_connected = false;
    tx_buf_len = 0;
  }

  // car -> app: drain CAN RX, batch into one datagram (classic frames only).
  if (client_connected) {
    CANPacket_t rxf;
    uint32_t budget = BRIDGE_MAX_FRAMES_PER_UDP;
    while ((budget-- > 0U) && can_pop(&can_rx_q, &rxf)) {
      if (rxf.fd != 0U) { continue; }   // bridge classic CAN only
      if (tx_buf_len == 0) { tx_buf_first_ms = now_ms; }   // start of a new batch
      tx_buf_len += bridge_encode(&rxf, &tx_buf[tx_buf_len]);
      if ((tx_buf_len + (int)BRIDGE_MAX_PACKET) > (int)sizeof(tx_buf)) {
        flush_tx_buffer();   // size-bound: datagram full, ship it now
        last_send_ms = now_ms;
        last_can_frame_ms = now_ms;
      }
    }
    // Coalesce: ship the batch once it's been held BRIDGE_TX_COALESCE_MS, instead of
    // every poll — packs many frames per datagram. Size-bound flush above handles
    // bursts; this is the latency bound for a partial batch.
    if ((tx_buf_len > 0) && ((now_ms - tx_buf_first_ms) >= BRIDGE_TX_COALESCE_MS)) {
      flush_tx_buffer();
      last_send_ms = now_ms;
      last_can_frame_ms = now_ms;
    } else if ((tx_buf_len == 0) && ((now_ms - last_send_ms) > KEEPALIVE_INTERVAL_MS)) {
      send_keepalive();
      last_send_ms = now_ms;
    }
  }

  bridge_send_health(now_ms);   // unsolicited ~2 Hz health push (self-gated)
}
