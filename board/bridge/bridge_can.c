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

// can_ring is panda-internal (board/drivers/drivers.h) and drags in CMSIS-typed
// driver decls. We only ever pass &can_rx_q to can_pop and never touch its fields,
// so an opaque forward declaration keeps those heavy headers out of this TU. The
// real definitions live in the unity TU (main_bridge.c).
typedef struct can_ring can_ring;
extern can_ring can_rx_q;
bool can_pop(can_ring *q, CANPacket_t *elem);
void can_send(CANPacket_t *to_push, uint8_t bus_number, bool skip_tx_hook);

#define BRIDGE_UDP_PORT       5555U
#define HEARTBEAT_TIMEOUT_MS  500U
#define KEEPALIVE_INTERVAL_MS 250U

static struct udp_pcb *can_udp;
static ip_addr_t  client_addr;
static uint16_t   client_port;
static bool       client_connected;
static uint32_t   last_client_packet_ms;
static uint32_t   last_send_ms;
// Updated at the top of every bridge_can_poll(); read by the lwIP receive
// callback (which fires within the same NO_SYS poll loop) so it can timestamp
// client activity without plumbing now_ms through lwIP.
static uint32_t   current_ms;

static uint8_t tx_buf[BRIDGE_MAX_FRAMES_PER_UDP * BRIDGE_MAX_PACKET];
static int     tx_buf_len;

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

// app -> car: parse concatenated bridge packets and write each to the bus.
static void udp_recv_cb(void *arg, struct udp_pcb *pcb, struct pbuf *p,
                        const ip_addr_t *addr, u16_t port) {
  (void)arg; (void)pcb;
  if (p == NULL) { return; }

  ip_addr_copy(client_addr, *addr);
  client_port = port;
  client_connected = true;
  last_client_packet_ms = current_ms;

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

void bridge_can_poll(uint32_t now_ms) {
  current_ms = now_ms;   // visible to udp_recv_cb (same NO_SYS loop)

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
      tx_buf_len += bridge_encode(&rxf, &tx_buf[tx_buf_len]);
      if ((tx_buf_len + (int)BRIDGE_MAX_PACKET) > (int)sizeof(tx_buf)) {
        flush_tx_buffer();
      }
    }
    if (tx_buf_len > 0) {
      flush_tx_buffer();
      last_send_ms = now_ms;
    } else if ((now_ms - last_send_ms) > KEEPALIVE_INTERVAL_MS) {
      send_keepalive();
      last_send_ms = now_ms;
    }
  }
}
