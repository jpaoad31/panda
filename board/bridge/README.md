# RoadStud panda bridge (USB-C / CDC-NCM)

Make a comma **panda** connect **directly** to an iPhone/iPad over USB-C and feed
the RoadStud app CAN data — no Pico, no WiFi/Pi (too much latency), no extra
USB-to-CAN adapter. The app connects **unchanged**.

**Validated on hardware** (see Status below): NCM + lwIP + DHCP + FDCAN + the
signing/boot chain all work, and the app reads a real car over the bridge. Beyond raw
CAN, the bridge also tunnels the panda's full native control surface over the same UDP
socket — see "Control plane (RSCP)".

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
| `main_bridge.c` | ✅ done + **validated on hardware** | Unity TU: panda init (boot `SAFETY_NOOUTPUT`) + OTG→TinyUSB IRQ routing + the bridge main loop (status LED ladder, re-flash escape hatch, 1 Hz safety/heartbeat tick for actuation). |

Tooling + sources are confirmed lined up: TinyUSB 0.20 (NCM + dwc2 STM32H7) and lwIP
2.2 are cloned at `~/github/tinyusb` + `~/github/lwip`; the macOS build env builds the
stock `panda_h7` firmware; and the bridge logic compiles against the pinned headers.

## Status: VALIDATED ON HARDWARE (2026-06-12)

A **red panda** flashed with this firmware was driven end-to-end from a Linux host
(Raspberry Pi): it enumerated as USB-CDC-NCM, brought up lwIP, leased the host
`192.168.4.2` from its DHCP server, and streamed **~1800 CAN frames/s across 29 unique
IDs** over the panda-bridge UDP protocol — verified with `board/bridge/tools/bridge_test.py`
(the host standing in for the iPhone). NCM + lwIP + DHCP + FDCAN read + the signing/boot
chain all work. Since then, **first real steering actuation on a 2017 Toyota RAV4 is
confirmed** (set-safety-mode + 2 Hz heartbeat gating the controls). Either a Raspberry Pi
or a **Mac** works as the flash/comms host — a Mac enumerates the panda over a proper cable
and flashes it directly with `board/bridge/tools/flash_app.py` from the local venv
(`board/bridge/macos_build.sh` builds it); an earlier note that a USB-C Mac "couldn't
enumerate" was a bad-cable issue, now resolved.

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

**Production follow-ups (it runs end-to-end; these harden it):**
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

> **⚠️ Known issue (2026-06-14): the BOOTSTUB reflash path is unproven from the NCM app.**
> The **app** reflash (`RSDFUAPP` → soft-flasher → `flash_app.py`) works reliably and
> survives power-cycles. The **bootstub** reflash (`RSDFUBTL` → ST ROM DFU →
> `flash_bootstub.py`) did **not** work on the Pi: after `RSDFUBTL`, the ST ROM DFU
> device (`0483:df11`) never enumerated (`PandaDFU.list()` empty, `lsusb` showed
> nothing), and the panda sat **dark — no LED, no USB** (it's in ROM DFU, which doesn't
> drive the LEDs). **It is not bricked:** a physical **replug** power-cycles it → the dev
> bootstub opens its flash window → boots the last-flashed app. So you can't change
> anything *in the bootstub* (e.g. the flash-window duration — `flasher.h` is set to 5 s
> in source but the device still runs the 15 s bootstub) until this path is fixed.
>
> Likely cause to investigate: the original dev bootstub was installed back when the
> panda ran the **factory** app, which speaks the panda USB protocol, so
> `Panda().reset(enter_bootloader=True)` could command it into ST ROM DFU directly. From
> the NCM app the only route is `RSDFUBTL` → `ENTER_BOOTLOADER_MAGIC` →
> `enter_bootloader()` in the bootstub — which may not bring up USB the way the STM32H7
> ROM DFU expects (different PHY/clock setup), or needs more enumeration settle time, or
> a different host. Confirm whether the panda actually reaches ROM DFU (vs. the dev
> bootstub's own soft-flasher window intercepting first), then make `flash_bootstub.py`
> find it. The app path is unaffected, so this doesn't block app firmware work.

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

The build/link, SCons target, glue, init wiring, and a full hardware bring-up are
**done** (see Status above). What's left:

0. **Fix the bootstub reflash path (ST ROM DFU from the NCM app).** `RSDFUBTL` →
   `flash_bootstub.py` doesn't enumerate ST ROM DFU on the Pi — see the "Known issue"
   callout under "Re-flashing" above. Not needed for app work (the `RSDFUAPP` app path is
   solid), but required before any bootstub change (e.g. the 5 s flash window) can ship.

1. ~~**Smoke test (path A as-is).**~~ ✅ **DONE (2026-06-12)** — flashed a red panda;
   enumerated as NCM → host got a 192.168.4.x DHCP lease → ~1800 CAN frames/s across 29
   IDs over UDP. Verified from a Linux host (`bridge_test.py`); the iPhone/iPad is the
   next host to try (should behave identically). The RGB status LED ladder
   (white→cyan→blue→purple→green) made bring-up debuggable at a glance.

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
   - drop `board/drivers/usb.h`, `pwm.h`, `bootkick.h` — but **keep** `can_comms.h`,
     `main_comms.h`, and the copied `set_safety_mode`/`is_car_safety_mode`: the RSCP
     control plane routes through `comms_control_handler` and actuation needs
     `set_safety_mode`, so they're now load-bearing, not dead weight.
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

## Control plane (RSCP)

Beyond streaming CAN, the bridge tunnels the panda's **native control transfers** over
the same UDP socket, so the app reaches everything the proprietary USB driver did
(health, version, serial, set-safety-mode, heartbeat, CAN speed, OBD mux). Rather than
reimplement each command, a request is marshalled into a `ControlPacket_t` and run
through the panda's own `comms_control_handler` — full parity for one dispatch shim.

Wire format (shares `:5555` with CAN frames, the keepalive, and the reflash sentinels):
- request `"RSCP"`: `[0..3]`magic `[4]`ver=1 `[5]`opcode(bRequest) `[6]`req_id `[7]`flags `[8..9]`param1(LE) `[10..11]`param2(LE) `[12..13]`resp_cap(LE) `[14..]`payload
- response `"RSCR"`: `[0..3]`magic `[4]`ver `[5]`opcode `[6]`req_id `[7]`status `[8..9]`payload_len(LE) `[10..]`payload

- Requests are recognised in the lwIP receive callback but **deferred to the main loop**
  (`process_control`) before calling the handler, since some commands re-init CAN.
- Disambiguation vs a CAN batch: the magic must match **and** byte[5] must not be a
  valid CAN XOR checksum (an opcode is ≥0xA8, never the 0x13 checksum of `"RSCP"`+ver).
- **Retransmit dedupe:** the last reply is cached and a retransmit (same `req_id`) is
  re-ACKed without re-running the handler, so non-idempotent OUT commands run once.

### Unsolicited health push (req_id = 0) + bridge-stats trailer

The bridge **pushes** an `RSCR` health datagram ~2 Hz without being asked (`bridge_send_health`),
so the app gets continuous `safety_mode` / `controls_allowed` / voltage without polling. It's
an ordinary `RSCR` with **`req_id = 0`** — the app never *sends* req_id 0, so it routes the
push to its health cache instead of matching a pending request. `opcode = 0xD2`, `status = 0`.

Its payload is the packed `health_t` (59 B) **plus a bridge-stats trailer** (push only — the
on-demand `0xD2` reply via `process_control` has no trailer, `payload_len = 59`):

| payload bytes | field | meaning |
|---|---|---|
| `[0 .. 58]` | `health_t` | packed panda health struct (`board/health.h`) — unchanged |
| `[59 .. 62]` | `uint32 LE tx_drops` | outbound datagrams the bridge dropped on a busy NCM TX buffer, since boot |
| `[63 .. 254]` | `can_health_t` ×3 (bus 0,1,2) | raw `0xC2` per-bus layout — `bus_off`, `transmit/receive_error_cnt`, `total_error_cnt`, `total_rx_lost_cnt`, IRQ call-rates. For diagnosing the armed-mode CAN error storm. |

So the push is `payload_len = 255` (health_t 59 + tx_drops 4 + 3×can_health 192). **App side:** the
existing `health_t` parser is unaffected (reads `[0..58]`); read `tx_drops` at offset 59, and the
three `can_health_t` blocks starting at offset 63 (64 B each, identical to the `0xC2` reply layout).
Log per-bus `bus_off` + error counters alongside `tx_drops` and `health_t`'s `rx_buffer_overflow`.
Under armed-mode load these distinguish the failure modes: climbing `transmit_error_cnt`/`bus_off` =
CAN error storm (the bus is sick); rising `tx_drops` = NCM shedding (host can't keep up);
`rx_buffer_overflow` = the main loop fell behind draining CAN. All read 0 in normal operation.

### OBD vs NORMAL routing (which bus a car is read on)

The panda only exposes the **OBD-II port** CAN on **bus 1** in OBD mode (`set_obd`,
`CAN_MODE_OBD_CAN2`). In NORMAL mode it reads the harness powertrain (bus 0) + camera
(bus 2) and the OBD-II pins are **not** muxed — so an OBD-tapped car reads **0 frames**
in NORMAL mode (electrically silent, no bus errors — the giveaway it's a routing issue,
not a bitrate one). The app enables OBD on connect and settles it per-car from the
openpilot `CanBus` (`needsOBD = canBus.all.contains(1)`).

## Safety mode + actuation heartbeat

Boot initialises via `set_safety_mode(SAFETY_NOOUTPUT)`: CAN receives + ACKs normally,
but all TX is gated by the safety model (read-only by default). `bridge_can.c` injects
app→car frames via `can_send(..., skip_tx_hook=false)`, so the panda safety model is the
gate between the app and the car's actuators.

For actuation the app sends `set_safety_mode` (0xDC) for the car + a **2 Hz heartbeat**
(0xF3). `main_bridge.c`'s `bridge_safety_tick()` ports the safety-relevant subset of the
stock 1 Hz tick (heartbeat counter, controls-allowed bookkeeping, the 3-strike engaged
mismatch, `safety_tick`) and **reverts to SILENT if the heartbeat lapses** — but only in
a car safety mode, so the read-only NOOUTPUT state is never forced to SILENT. Validated
on hardware: the mode round-trips, reverts on heartbeat loss, and holds with the 2 Hz
heartbeat flowing.

## What the app needs (unchanged) — acceptance checklist

- CDC-NCM device iOS recognizes as a network interface.
- panda IP `192.168.4.1/24`; DHCP leases host `192.168.4.x` with **no** router/DNS.
- UDP server on `:5555` that latches the client from any datagram (no handshake).
- car→app: bridge-format frames, batched ≤64 per datagram.
- app→car: parse concatenated bridge packets; ignore ≤1-byte heartbeats.
- idle keepalive (≥1 byte, ~250 ms) so the app's 2 s receive-timeout doesn't drop.
