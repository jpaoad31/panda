// RoadStud CAN bridge wire format — panda side.
//
// Translates between the panda's internal CANPacket_t and the "panda bridge"
// wire format the RoadStud iOS app speaks over UDP. This is the SAME 6-byte
// format the Pico bridge uses (canbridge/include/panda_protocol.h and the app's
// PandaProtocol.swift) — NOT the panda's native USB comms format (can_comms.h),
// which packs addr<<3 with flags. We translate so the existing app connects
// unchanged.
//
// Wire format (per frame, 6-byte header + 0..8 payload bytes):
//   [0]   (DLC << 4) | (bus << 1) | FD        // DLC = 4b dlc code, bus = 3b, FD = 1b
//   [1-4] CAN address, 32-bit BIG-endian
//   [5]   XOR checksum of bytes [0..4]
//   [6..] data, length = dlc_to_len[DLC] (classic CAN: <= 8)
//
// Requires the panda CAN headers (CANPacket_t) to be included first, e.g. via
// "can.h" / "opendbc/safety/can.h".
#pragma once

#include <stdint.h>
#include <string.h>

#define BRIDGE_HEADER_SIZE 6U
#define BRIDGE_MAX_DATA    8U                       // classic CAN only over the bridge
#define BRIDGE_MAX_PACKET  (BRIDGE_HEADER_SIZE + BRIDGE_MAX_DATA)
// 100 frames * 14B max = 1400B per datagram — under the 1472B UDP-payload MTU
// (1500 IP - 20 IP hdr - 8 UDP hdr), so it ships as one Ethernet frame with no IP
// fragmentation. Don't raise past ~105 or datagrams will fragment.
#define BRIDGE_MAX_FRAMES_PER_UDP 100U

// DLC code -> data byte count (classic + FD), mirrors the app's table.
static const uint8_t bridge_dlc_to_len[16] = {0U,1U,2U,3U,4U,5U,6U,7U,8U,12U,16U,20U,24U,32U,48U,64U};

static inline uint8_t bridge_len_to_dlc(uint8_t len) {
  for (uint8_t i = 0U; i < 16U; i++) {
    if (bridge_dlc_to_len[i] >= len) { return i; }
  }
  return 15U;
}

// Encode one panda CANPacket_t into the bridge wire format at `buf`.
// Returns bytes written. FD frames are not bridged (classic CAN only) — the
// caller should skip pkt->fd != 0 before calling.
static inline int bridge_encode(const CANPacket_t *pkt, uint8_t *buf) {
  uint8_t dlc = (uint8_t)(pkt->data_len_code & 0x0FU);
  uint8_t data_len = bridge_dlc_to_len[dlc];
  if (data_len > BRIDGE_MAX_DATA) { data_len = BRIDGE_MAX_DATA; }

  buf[0] = (uint8_t)((dlc << 4) | ((pkt->bus & 0x07U) << 1));   // FD bit left 0
  uint32_t addr = (uint32_t)pkt->addr;
  buf[1] = (uint8_t)(addr >> 24);
  buf[2] = (uint8_t)(addr >> 16);
  buf[3] = (uint8_t)(addr >> 8);
  buf[4] = (uint8_t)(addr);
  buf[5] = (uint8_t)(buf[0] ^ buf[1] ^ buf[2] ^ buf[3] ^ buf[4]);

  memcpy(&buf[6], pkt->data, data_len);
  return (int)(BRIDGE_HEADER_SIZE + data_len);
}

// Decode bridge bytes into a CANPacket_t ready for can_send().
// Returns bytes consumed, or 0 on short buffer / checksum mismatch.
static inline int bridge_decode(const uint8_t *buf, int buf_len, CANPacket_t *pkt) {
  if (buf_len < (int)BRIDGE_HEADER_SIZE) { return 0; }

  uint8_t dlc = (uint8_t)(buf[0] >> 4);
  uint8_t bus = (uint8_t)((buf[0] >> 1) & 0x07U);
  uint8_t data_len = bridge_dlc_to_len[dlc & 0x0FU];
  if (data_len > BRIDGE_MAX_DATA) { data_len = BRIDGE_MAX_DATA; }

  if (buf_len < (int)(BRIDGE_HEADER_SIZE + data_len)) { return 0; }
  if ((uint8_t)(buf[0] ^ buf[1] ^ buf[2] ^ buf[3] ^ buf[4]) != buf[5]) { return 0; }

  uint32_t addr = ((uint32_t)buf[1] << 24) | ((uint32_t)buf[2] << 16) |
                  ((uint32_t)buf[3] << 8)  |  (uint32_t)buf[4];

  memset(pkt, 0, sizeof(*pkt));
  pkt->addr = addr & 0x1FFFFFFFU;                 // 29-bit field
  pkt->bus = (uint8_t)(bus & 0x07U);
  pkt->data_len_code = bridge_len_to_dlc(data_len);
  pkt->extended = (addr > 0x7FFU) ? 1U : 0U;      // >11 bits -> extended ID
  pkt->fd = 0U;
  memcpy(pkt->data, &buf[6], data_len);
  return (int)(BRIDGE_HEADER_SIZE + data_len);
}
