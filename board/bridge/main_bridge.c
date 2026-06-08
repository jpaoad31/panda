// RoadStud panda bridge firmware — entry point (TEMPLATE).
//
// A dedicated firmware that makes the panda enumerate as a USB CDC-NCM device and
// bridges the car's CAN to the RoadStud iOS app over UDP :5555 — the same path
// the Pico bridge's USB mode uses, so the app connects unchanged.
//
// This is a TEMPLATE showing how the pieces assemble. It will NOT build until the
// TinyUSB + lwIP layer is vendored in (see README.md). Sections marked
// [COPY] should be copied VERBATIM from the known-good Pico bridge
// (canbridge/src/main_usb.c + usb_descriptors.c), per canbridge/CLAUDE.md's
// hard-won lesson: don't write USB-NCM/lwIP glue from scratch.
//
// Sections marked [PANDA] are the only real divergences from the Pico: clock/PHY
// init and the CAN backend (FDCAN instead of MCP2515).

#include "board/bridge/bridge_can.h"

// [COPY] from the Pico example / TinyUSB:
//   - tusb.h, lwip/init.h, lwip/timeouts.h, dhserver.h
//   - init_lwip(), service_traffic(), linkoutput_fn(), netif_init_cb(),
//     tud_network_recv_cb()/xmit_cb(), tud_network_mac_address[]
//   - the dhcp_config (192.168.4.1, leases .2-.4, router/DNS = 0.0.0.0)
//   - usb_descriptors.c (NCM, bcdUSB=0x0201, BOS + MS OS 2.0)

extern void init_lwip(void);          // [COPY]
extern void service_traffic(void);    // [COPY]

// [PANDA] panda support — adapt to this fork's APIs (see board/main.c):
//   - clock + USB OTG_HS (FS PHY, device mode) init
//   - FDCAN init for the buses you want to bridge
//   - a free-running millisecond clock
extern void panda_clock_and_usb_init(void);   // [PANDA] TODO
extern void panda_can_init(void);              // [PANDA] TODO (FDCAN bring-up)
extern uint32_t panda_millis(void);            // [PANDA] e.g. microsecond_timer_get()/1000

int main(void) {
  panda_clock_and_usb_init();   // [PANDA]

  // [COPY] bring up TinyUSB device stack on the OTG_HS (rhport 1, full speed).
  // tusb_rhport_init_t dev_init = { .role = TUSB_ROLE_DEVICE, .speed = TUSB_SPEED_FULL };
  // tusb_init(BOARD_TUD_RHPORT, &dev_init);

  // [COPY] lwIP + DHCP server (no router/DNS option -> iOS keeps its default route).
  // init_lwip();
  // while (!netif_is_up(&netif_data));
  // while (dhserv_init(&dhcp_config) != ERR_OK);

  bridge_can_init();            // our UDP :5555 server + CAN translation

  panda_can_init();             // [PANDA]

  for (;;) {
    // tud_task();              // [COPY] service USB
    // service_traffic();       // [COPY] pump lwIP (sys_check_timeouts + RX)
    bridge_can_poll(panda_millis());   // drain CAN RX -> UDP, inject TX, keepalive
  }
  return 0;
}
