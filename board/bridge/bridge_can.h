// RoadStud CAN-over-UDP bridge logic (panda side).
//
// This is the transport-agnostic "our logic" layer: it owns the UDP server on
// :5555, latches the app client, drains the panda CAN RX queue to the client in
// the bridge wire format, and injects app->car frames onto the bus. It is a
// faithful port of the Pico bridge's UDP section (canbridge/src/main_usb.c).
//
// It depends ONLY on:
//   - lwIP raw API (udp_*, pbuf_*, ip_addr_t) — provided by the lwIP + USB-NCM
//     layer copied from the TinyUSB net_lwip_webserver example (see README.md).
//   - the panda CAN queues: can_rx_q + can_pop(), and can_send().
//
// The NCM device, lwIP netif bring-up, and DHCP server are NOT here — those are
// copied verbatim from the working example per canbridge/CLAUDE.md ("don't write
// USB NCM from scratch"). This module is meant to be called from that firmware's
// main loop.
#pragma once

#include <stdint.h>
#include <stdbool.h>

// Create the UDP PCB, bind :5555, and register the receive callback.
// Call once after lwIP is up.
void bridge_can_init(void);

// Service the bridge: drop a stale client, drain CAN RX -> UDP (batched), and
// emit an idle keepalive. Call every main-loop iteration. `now_ms` is a
// free-running millisecond clock supplied by the firmware.
void bridge_can_poll(uint32_t now_ms);

// True once an app client has been seen and hasn't timed out — handy for a
// status LED.
bool bridge_can_client_connected(void);

// True while real CAN frames (not just keepalives) reached the client in the last
// ~second — the "green" state of the bring-up LED.
bool bridge_can_frames_flowing(uint32_t now_ms);
