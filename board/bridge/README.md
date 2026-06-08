# RoadStud panda bridge (USB-C / CDC-NCM)

Make a comma **panda** connect **directly** to an iPhone/iPad over USB-C and feed
the RoadStud app CAN data — no Pico, no WiFi/Pi (too much latency), no extra
USB-to-CAN adapter. The app connects **unchanged**.

This is a **scaffold**: the hardware-independent logic is written and correct;
the USB/lwIP/build assembly is documented and must be brought up against real
hardware following the methodology below.

## Why this is mostly firmware, not hardware

The panda's full-size USB port is **already a USB peripheral**: the firmware forces
device mode (`FDMOD` in `board/stm32h7/llusb.h`) on `USB_OTG_HS` via the embedded
**full-speed** PHY, currently enumerating as a vendor-specific device. So:

- No board modification. Plug **panda USB-A → USB-C iPhone/iPad** with an A-to-C
  cable. The panda is bus-powered from the car/OBD side, so host power is a non-issue.
- MCU is **STM32H725** (1 MB flash, 320 KB+ RAM) — ample room for lwIP + NCM.
- Full-speed (12 Mbps) is plenty for CAN.

## Approach (and the #1 lesson)

From `canbridge/CLAUDE.md`: **do not write USB-NCM from scratch.** Multiple attempts
failed. The working recipe is: copy TinyUSB's `net_lwip_webserver` example verbatim,
verify enumeration + DHCP on hardware, *then* add the CAN bridge logic on top.

So: build a **dedicated CDC-NCM firmware** using **TinyUSB** (its `dwc2` port
supports STM32H7) + lwIP, reusing the Pico bridge's known-good NCM/lwIP/DHCP, and
swap only the CAN backend (panda **FDCAN** instead of the Pico's MCP2515). The app
talks UDP to `192.168.4.1:5555` and can't tell which transport it's on.

## The one real gotcha: wire format

The panda's **native** USB CAN format (`board/can_comms.h`) packs `addr<<3 | flags`.
The RoadStud bridge/app format (`canbridge/include/panda_protocol.h` /
`PandaProtocol.swift`) is a 6-byte header with a **raw 32-bit big-endian address**
+ XOR checksum. They are **not** interchangeable. `bridge_protocol.h` here does the
translation on the panda's `CANPacket_t`. Do **not** forward `comms_can_read` bytes.

## Building on macOS (folder-local, no global installs) — VERIFIED

`board/bridge/macos_build.sh` builds the firmware with everything kept inside the
repo. First run creates a local uv venv (Python 3.12 + `opendbc`) and the
downloaded interpreter + cache under `.venv/`, `.uv-python/`, `.uv-cache/` (all
gitignored). Nothing touches your global environment.

```bash
brew install uv                      # one-time (if not present)
brew install --cask gcc-arm-embedded # ARM GNU Toolchain (newlib) -> /Applications
board/bridge/macos_build.sh          # builds panda_h7 etc.; re-run after edits
```

Confirmed working: produces signed `board/obj/panda_h7.bin.signed`.

Two macOS gotchas the script handles (the stock docs miss these):
- **Homebrew's `arm-none-eabi-gcc` is a bare compiler with no newlib** — it fails
  with `fatal error: stdint.h: No such file or directory`. Use ARM's toolchain in
  `/Applications/ArmGNUToolchain` instead (the script auto-finds it).
- **`board/crypto/sign.py` runs via `#!/usr/bin/env python3`** — the venv must be
  first on `PATH` or it uses system python3 (no `pycryptodome`) and fails with
  `ModuleNotFoundError: No module named 'Crypto'`. The script puts `.venv/bin` first.

## Files in this directory

| File | Status | Role |
|------|--------|------|
| `bridge_protocol.h` | ✅ done, hardware-independent | Encode/decode the bridge wire format on `CANPacket_t`. Mirrors the app byte-for-byte. |
| `bridge_can.h/.c` | ✅ done (compiles only in the assembled build) | UDP :5555 server: client latch, ≤64-frame batching, idle keepalive, RX drain from `can_rx_q`, TX inject via `can_send`. Faithful port of `main_usb.c`'s UDP section. |
| `tusb_config.h` | ✅ starting point | TinyUSB config for STM32H7 NCM (rhport 1, full speed, `CFG_TUD_NCM=1`). |
| `main_bridge.c` | 🟨 template | Shows how it all assembles. `[COPY]` = copy from the Pico example; `[PANDA]` = adapt to this fork. |

## To finish (on hardware)

1. **Vendor TinyUSB + lwIP** into a new SCons build target (mirror `build_project`
   for `panda_h7` in `SConscript`; produce a separate `panda_bridge` binary). Add
   TinyUSB (`src/tusb.c`, `class/net/*`, `portable/synopsys/dwc2/*`) and lwIP core.
2. **Copy verbatim from `canbridge`:** `usb_descriptors.c` (NCM, `bcdUSB=0x0201`,
   BOS/MS-OS-2.0), the `init_lwip()` / `service_traffic()` / `linkoutput_fn` glue
   and `dhcp_config` from `src/main_usb.c`, `include/usb/lwipopts.h`, and the
   `dhcpserver/`. Keep the **router/DNS DHCP options = 0.0.0.0** (this is what lets
   iOS keep WiFi/cellular as its default route).
3. **`[PANDA]` init:** clock + OTG_HS (FS PHY, device) — reuse this fork's existing
   USB clock setup from `board/main.c`/`llusb.h`; FDCAN bring-up for the buses to
   bridge; a `panda_millis()` from the existing microsecond timer.
4. **Wire `bridge_can_*`** into the main loop (see `main_bridge.c`): `tud_task()` →
   `service_traffic()` → `bridge_can_poll(panda_millis())`. Call `bridge_can_init()`
   after lwIP is up.
5. **Verify in order** (each step is a known checkpoint): NCM enumerates on the iPad
   → host gets a 192.168.4.x DHCP lease → app shows Connected (the UDP keepalive)
   → real CAN frames appear in the app. A status LED mirroring the Pico's
   cyan→blue→purple→green ladder makes bring-up much easier.

## Safety mode

`bridge_can.c` injects app→car frames via `can_send(&pkt, pkt.bus, /*skip_tx_hook=*/false)`,
so the panda safety model applies. For RoadStud's read-only use that's irrelevant
(the app sends nothing but heartbeats). When actuation is enabled, set an
appropriate safety mode, or pass `skip_tx_hook=true` for a raw forwarding bridge —
decide deliberately; this is the gate between the app and the car's actuators.

## What the app needs (unchanged) — acceptance checklist

- CDC-NCM device iOS recognizes as a network interface.
- panda IP `192.168.4.1/24`; DHCP leases host `192.168.4.x` with **no** router/DNS.
- UDP server on `:5555` that latches the client from any datagram (no handshake).
- car→app: bridge-format frames, batched ≤64 per datagram.
- app→car: parse concatenated bridge packets; ignore ≤1-byte heartbeats.
- idle keepalive (≥1 byte, ~250 ms) so the app's 2 s receive-timeout doesn't drop.
