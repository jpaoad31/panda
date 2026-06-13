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

## Building & flashing on a Raspberry Pi (Linux) — VERIFIED

A Pi is the easiest *host* for flashing: native USB-A host ports + the documented
Linux path (a USB-C-only Mac fights the panda's USB-A device port over cable/role
negotiation). `board/bridge/tools/pi_build.sh` builds there; same `sign.py` PATH
trick as macOS. One-time setup:

```bash
sudo apt install -y gcc-arm-none-eabi binutils-arm-none-eabi libnewlib-arm-none-eabi dfu-util
python3 -m venv ~/pandaenv
~/pandaenv/bin/pip install scons pycryptodome numpy pycapnp   # pycapnp: opendbc CarParams
git clone --depth 1 https://github.com/commaai/opendbc ~/opendbc
# panda udev rules (VIDs 0483/3801/bbaa) -> /etc/udev/rules.d/11-panda.rules
```

Build, then flash (panda powered from the car, USB-A into a Pi USB-A port):
```bash
board/bridge/tools/pi_build.sh board/obj/panda_bridge.bin.signed board/obj/bootstub.panda_bridge.bin
~/pandaenv/bin/python board/bridge/tools/flash_bootstub.py   # once: dev bootstub via DFU
~/pandaenv/bin/python board/bridge/tools/flash_app.py        # flash the signed NCM app
~/pandaenv/bin/python board/bridge/tools/bridge_test.py      # comms test: Pi acts as the app
```

`bridge_test.py` makes the Pi play the iPhone — it gets a 192.168.4.x DHCP lease over
the NCM link, speaks the panda-bridge UDP protocol to `192.168.4.1:5555`, and prints
CAN throughput + unique IDs (so we can confirm the bridge works without iOS). It also
takes `--reflash` / `--reflash-bootstub` to fire the escape-hatch sentinels.

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

## Flashing

The build emits two artifacts under `board/obj/`: `panda_bridge.bin.signed` (our app)
and `bootstub.panda_h7.bin` (the stock **debug** bootstub, from the `panda_h7` target).

```bash
board/bridge/vendor.sh        # one-time: vendor TinyUSB/lwIP
board/bridge/macos_build.sh   # builds all targets incl. the signed bridge app
```

1. **Install a debug bootstub (one-time, only if the panda has a release/factory
   bootstub).** A release bootstub has no `-DALLOW_DEBUG`, so it rejects our
   debug-signed app and falls into the flasher. DFU-flash the debug bootstub:
   ```bash
   python3 -c "from panda import Panda; Panda().recover()"
   ```
   `recover()` reads the hardcoded `bootstub.panda_h7.bin` (now a debug bootstub) and
   then reflashes the stock app — that's fine, we overwrite the app in step 2. A
   panda already running source-built firmware has a debug bootstub; skip this step.

2. **Flash the bridge app:**
   ```bash
   python3 -c "from panda import Panda; Panda().flash('board/obj/panda_bridge.bin.signed', reconnect=False)"
   ```
   **`reconnect=False` is required.** After flashing, the panda reboots into the NCM
   app, which does **not** speak the panda USB protocol — the library's default
   reconnect would hang. From here it should enumerate as a USB-NCM network interface.

### Re-flashing (escape hatch)

Once the NCM app is running you **cannot force it back into the flasher over USB** the
normal way: the app has no panda control endpoint, so the host's
`reset(enter_bootstub=True)` (`0xd1`) request goes unanswered. The bootstub runs on
every boot but only *stays* in the flasher if the app signature fails or the boot magic
is set. There are no buttons / exposed BOOT0 on red, so without an escape hatch
re-flashing would mean opening the case for SWD.

**The app provides the escape hatch** (`bridge_can.c` → `main_bridge.c`
`bridge_request_reboot`): two exact 8-byte UDP sentinels write the boot magic to
`enter_bootloader_mode` (SRAM `0x38001FFC`, survives the soft reset) and `NVIC_SystemReset`.
A host dev script sends them; the **iOS app never does**.

```bash
# reflash the APP (next boot enters the bootstub soft-flasher):
python3 -c "import socket; socket.socket(2,2).sendto(b'RSDFUAPP', ('192.168.4.1',5555))"
python3 -c "from panda import Panda; Panda().flash('board/obj/panda_bridge.bin.signed', reconnect=False)"

# reflash the BOOTSTUB too (next boot enters ST ROM DFU):
python3 -c "import socket; socket.socket(2,2).sendto(b'RSDFUBTL', ('192.168.4.1',5555))"
python3 -c "from panda import Panda; Panda().recover()"
```

The sentinels can't collide with a ≤1-byte heartbeat or a checksum-valid CAN frame, so
the match is unambiguous.

#### Belt-and-suspenders: the dev bootstub (`bootstub.panda_bridge.bin`)

The UDP sentinel only works once the app reaches its UDP loop. If a build wedges
*before* that (a hang in early init / `tud_task`), the sentinel can't help — and red
has no button/BOOT0. For that case the build also produces a **dev bootstub** that
opens a flash window on **every** boot, so any app, however broken, is always
recoverable over plain USB.

Behaviour (`-DBRIDGE_DEV_FLASH_WINDOW`, in `bootstub.c` + `flasher.h`): each boot it
enumerates as the panda flasher for ~15 s (blue LED blinking). If a host starts flashing
in that window it stays in the flasher; otherwise it resets straight into the app (a
skip flag in SRAM avoids re-opening the window). Cost: ~15 s added boot time + a brief
flasher→NCM re-enumerate — dev-only. The stock `bootstub.panda_h7.bin` is unflagged and
unaffected.

Install it once via DFU (`recover()` is hardcoded to `bootstub.panda_h7.bin`, so use
`PandaDFU` directly):
```python
from panda import Panda, PandaDFU
import time
p = Panda(); p.reset(enter_bootstub=True); p.reset(enter_bootloader=True)  # -> ST ROM DFU
time.sleep(1)
d = PandaDFU(PandaDFU.list()[0])
d.program_bootstub(open("board/obj/bootstub.panda_bridge.bin", "rb").read())
d.reset()
```
Then re-flash the app any time by power-cycling and running `Panda().flash(...)` within
the window. Miss the window and it just boots the (old) app — power-cycle and retry; a
bad app can never block the next boot's window.

### Status LED (red panda, GPIOE 4/3/2)

`main_bridge.c` drives the RGB LED as a worst-state-wins bring-up ladder (mirrors the
Pico bridge), so a sealed box is debuggable at a glance — invaluable since USB is busy
being NCM:

| Color | Meaning |
|-------|---------|
| white | USB/NCM not up yet (booting) |
| cyan | NCM up, host has **no DHCP lease** (the flaky-bring-up tell) |
| blue | host leased, **app not connected** |
| purple | app connected, **no CAN frames** (check car wiring / harness) |
| green | healthy — app connected + CAN flowing |

## Remaining work (on the panda)

The build/link, SCons target, glue, and init wiring are **done** (see Status above).
What's left needs the device:

1. **Smoke test (path A as-is).** Flash, plug into iPad: does it enumerate as NCM →
   get a 192.168.4.x DHCP lease → app shows Connected (UDP keepalive) → CAN frames
   appear? Fastest yes/no. The RGB status LED (see "Status LED" above) shows exactly
   where bring-up stalls — watch it walk white→cyan→blue→purple→green.

2. **Refactor `main_bridge.c` to a MINIMAL init (the cleanup).** Today it includes
   the full panda header set and calls `current_board->init()` so everything links,
   which also boots the whole panda runtime (SPI/comms/fan/harness/relays) and
   compiles a dormant panda USB class. For something that runs on a moving car,
   replace that with only what the bridge needs:
   - `clock_init()` + `peripherals_init()` (clock + RCC enables),
   - USB GPIO AF for PA11/PA12 (replicate `gpio_usb_init()`, currently static in
     `peripherals.h`) — **not** the full `current_board->init()`,
   - FDCAN GPIO AF + `can_init_all()`,
   - `microsecond_timer_init()`,
   - drop `board/drivers/usb.h`, `can_comms.h`, `main_comms.h`, `pwm.h`, `bootkick.h`
     and the copied `set_safety_mode`/`is_car_safety_mode`.
   Payoff: no stray IRQs/peripherals running unserviced, no second USB stack in the
   image, smaller/debuggable firmware. It's the production shape, not needed just to
   *try* enumeration.

3. **Watchdog — keep it.** `simple_watchdog` is software-only: it records
   `FAULT_HEARTBEAT_LOOP_WATCHDOG` (reported to the app) if the main loop stalls past
   ~375 ms; it does **not** self-reset, and there is **no** active hardware IWDG in
   this firmware. `main_bridge.c` already inits + kicks it each loop, giving a real
   liveness signal (e.g. a wedged TinyUSB xmit-wait would trip it). Keep this through
   the minimal-init refactor. For true auto-recovery on a car — where no host can
   power-cycle a hung bridge over USB (unlike a comma 3) — **arm the hardware IWDG1**
   (`IND_WDG`) and refresh it in the loop. Deliberate add; validate timing on device.

4. **Trim lwIP bss** (~455 KB): tune `lwipopts.h` pool sizes / `LWIP_HIGH_THROUGHPUT`.

5. **Bootstub + signing — DONE.** The target now signs the app (`SETLEN=1 sign.py`,
   mirroring `build_project`) and emits `board/obj/panda_bridge.bin.signed`
   (`[len][app][VERS+version][1024-bit RSA sig]`, debug key in a source build). No
   bridge-specific bootstub is built: the bootstub is app-agnostic (it only
   sig-checks then jumps to `0x8020000`), so the stock debug `bootstub.panda_h7.bin`
   runs the signed image. See "Flashing" below.

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
