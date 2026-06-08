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
| `bridge_can.h/.c` | ✅ done + **validated** | UDP :5555 server: client latch, ≤64-frame batching, idle keepalive, RX drain from `can_rx_q`, TX inject via `can_send`. Faithful port of `main_usb.c`'s UDP section. **Verified to compile cleanly against the pinned lwIP 2.2 + TinyUSB 0.20 headers (clang `-fsyntax-only`, 0 errors).** |
| `tusb_config.h` | ✅ starting point | TinyUSB config for STM32H7 NCM (rhport 1, full speed, `CFG_TUD_NCM=1`). |
| `lwipopts.h` | ✅ staged (from 0.20 example) | lwIP NO_SYS config, version-matched. |
| `arch/` | ✅ staged (from 0.20 example) | lwIP `cc.h` / `bpstruct.h` / `epstruct.h`. |
| `usb_descriptors.ncm.reference.c` | 📋 reference | The 0.20 example's net descriptors — trim to NCM-only for the final `usb_descriptors.c`. |
| `main_bridge.c` | 🟨 template | Shows how it all assembles. `[COPY]` = copy from the example; `[PANDA]` = adapt to this fork. |

Tooling + sources are confirmed lined up: TinyUSB 0.20 (NCM + dwc2 STM32H7) and lwIP
2.2 are cloned at `~/github/tinyusb` + `~/github/lwip`; the macOS build env builds the
stock `panda_h7` firmware; and the bridge logic compiles against the pinned headers.

## Status: COMPILES + LINKS (unvalidated — no hardware yet)

`board/obj/panda_bridge/main.{elf,bin}` builds end-to-end (text ~94 KB, data ~23 KB,
bss ~455 KB — fits the H7 RAM regions). Build it:

```bash
board/bridge/vendor.sh          # one-time: copy TinyUSB/lwIP from ~/github clones
board/bridge/macos_build.sh board/obj/panda_bridge/main.bin
```

How it's wired (`main_bridge.c` is the unity TU):
- Reuses the panda's `clock_init` / `peripherals_init` / `current_board->init()` /
  `microsecond_timer_init` / `can_init_all`, then routes the **OTG_HS IRQ to
  TinyUSB's `dcd_int_handler`** instead of the panda USB class.
- TinyUSB (NCM) + lwIP + dhserver/dnserver compile as relaxed TUs (`renv` in
  SConscript) and link against the unity TU's globals (incl. `memcpy`/`memset`).
- `bridge_libc.c` fills the libc gaps under `-nostdlib` (`memmove`/`strlen`/`strncmp`/
  `atoi`/`strcmp`/`strcpy`), a dummy `_ctype_` (lwIP IP-string parsing is unused),
  and `SystemCoreClock = 240 MHz` (for dwc2 timing).

**Caveats to resolve on hardware (it links, but isn't proven to run):**
- *Path-A coupling:* to make `current_board->init()` link, `main_bridge.c` includes
  the full panda driver/comms/USB header set, so the panda USB class is compiled but
  **dormant** (we never call `usb_init()`; only TinyUSB drives OTG). For production,
  switch to a **minimal init** (manual clock + USB/CAN GPIO AF, no board/comms/SPI)
  to drop the dead weight and remove any chance of the two USB paths interfering.
- *bss ~455 KB* — lwIP pool sizing (`lwipopts.h` `LWIP_HIGH_THROUGHPUT`) may want
  trimming for headroom.
- *Validate in order:* NCM enumerates on iPad → DHCP lease → app Connected → CAN
  flows. The clock/PHY/GPIO bring-up is the part that "links but may not enumerate."

## Remaining work (on the panda)

The build/link, SCons target, glue, and init wiring are **done** (see Status above).
What's left needs the device:

1. **Smoke test (path A as-is).** Flash, plug into iPad: does it enumerate as NCM →
   get a 192.168.4.x DHCP lease → app shows Connected (UDP keepalive) → CAN frames
   appear? Fastest yes/no. **If it boot-loops, suspect an unfed panda watchdog** —
   that's the cue to do step 2. A status LED mirroring the Pico's
   cyan→blue→purple→green ladder makes bring-up much easier.

2. **Refactor `main_bridge.c` to a MINIMAL init (the cleanup).** Today it includes
   the full panda header set and calls `current_board->init()` so everything links,
   which also boots the whole panda runtime (SPI/comms/fan/harness/relays + any
   watchdogs) and compiles a dormant panda USB class. For something that runs on a
   moving car, replace that with only what the bridge needs:
   - `clock_init()` + `peripherals_init()` (clock + RCC enables),
   - USB GPIO AF for PA11/PA12 (replicate `gpio_usb_init()`, currently static in
     `peripherals.h`) — **not** the full `current_board->init()`,
   - FDCAN GPIO AF + `can_init_all()`,
   - `microsecond_timer_init()`,
   - drop `board/drivers/usb.h`, `can_comms.h`, `main_comms.h`, `pwm.h`, `bootkick.h`,
     `simple_watchdog.h` and the copied `set_safety_mode`/`is_car_safety_mode`.
   Payoff: no unfed watchdog resets, no stray IRQs, no second USB stack in the image,
   smaller/debuggable firmware. See "what the minimal init gets us" — it's the
   production shape, not needed just to *try* enumeration.

3. **Trim lwIP bss** (~455 KB): tune `lwipopts.h` pool sizes / `LWIP_HIGH_THROUGHPUT`.

4. **Bootstub + signing.** The target builds `main.{elf,bin}` only; add the bootstub
   build + `sign.py` step (mirror `build_project`) before it can actually boot.

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
