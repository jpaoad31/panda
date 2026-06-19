# Path B — USB/net servicing in a timer ISR

Fallback firmware build for the RoadStud panda NCM bridge. Branch
`path-b-usb-timer-isr`. This is **path A + a structural restructure**: it moves
the USB/lwIP/CAN-drain servicing out of the main loop and into a high-priority
hardware-timer ISR so the CAN interrupt storm (armed-mode relay-intercept
forwarding) can no longer starve USB. Flash this only if the other (path A or a
different) fix fails on the bench.

If path B misbehaves: `git checkout master` (or the path-A commit) and reflash —
that build keeps USB servicing in the main loop and is the known-good fallback.

## The bug it fixes

Single-threaded NO_SYS main loop in `board/bridge/main_bridge.c`. When actuation
is armed the panda forwards bus0<->bus2, ~doubling FDCAN interrupt load (plus an
ACK/error storm if the intercept upsets the car bus). Because USB processing
(`tud_task` + lwIP) ran in the main loop (thread mode), every FDCAN ISR preempted
it; under the storm the loop was starved and the USB/NCM link wedged (needed a
replug). Path A demoted the CAN IRQs below OTG so the hardware-level USB ISR
always ran, but the *processing* was still in the loop — so the wedge persisted.

## What changed (all in `board/bridge/`)

- **`main_bridge.c`**
  - New `bridge_service_tick()` ISR body: runs `tud_task_ext(0,false)` +
    `bridge_net_service()` + `bridge_can_poll(now)`. These three were **removed
    from the main loop.**
  - New `bridge_service_timer_init()`: starts **TIM12** (the `TICK_TIMER`
    macro slot — unused by the bridge, RCC clock already enabled) at **2 kHz**,
    routed through the existing `TIM8_BRK_TIM12_IRQHandler` via
    `REGISTER_INTERRUPT`. Started only **after** `bridge_net_init()` /
    `bridge_can_init()` so the first tick can't touch lwIP before it's up.
  - NVIC: `SERVICE_TIMER_IRQ` set to priority **1**, the **same as `OTG_HS`**
    (both above the FDCAN IRQs at 2). See the locking decision below.
  - Main loop is now housekeeping only: `simple_watchdog_kick`,
    `bridge_safety_tick` (1 Hz), the LED. `bridge_safety_tick` is wrapped in
    `ENTER_CRITICAL`/`EXIT_CRITICAL` (see the set_safety_mode race below).
  - Re-entry guard `service_busy` on the ISR body.
- **`bridge_can.c`, `lwip_glue.c`**: comments updated — the lwIP context is now
  "the service-timer ISR", not "the main loop". No functional change.

## Timer rate: 2 kHz (500 us period)

- **Latency:** 500 us between USB/net services is well under any iOS-side
  tolerance and an enormous improvement over the unbounded main-loop latency
  under a CAN storm.
- **CAN starvation / bounded work:** each tick drains at most
  `BRIDGE_MAX_FRAMES_PER_UDP` (100) frames. Worst-case CAN arrival is
  `CAN_INTERRUPT_RATE` (~16 kHz). 2 kHz x 100 = 200k frames/s of drain capacity
  vs 16k/s arrival — the software `can_rx_q` (depth `CAN_RX_BUFFER_SIZE` = 4096)
  stays near-empty (~8 frames accumulate between ticks). The drain is bounded per
  tick so one ISR can't run long enough to let the small hardware FDCAN RX FIFO
  (drained by the prio-2 FDCAN ISR between our ticks) overflow.
- Range 1-4 kHz would all work; 2 kHz is the balance (low latency, low ISR
  overhead, generous CAN headroom). The `REGISTER_INTERRUPT` rate-fault ceiling
  is set to 2.5 kHz (25% headroom) so a healthy 2 kHz never false-trips
  `FAULT_INTERRUPT_RATE_TICK`.

## The `tud_task` / `dcd_int_handler` locking decision — SAME priority (1)

This is the load-bearing correctness call. Verified by reading the vendored
TinyUSB (`device/usbd.c`, `osal/osal_none.h`, `portable/synopsys/dwc2/`):

- TinyUSB's dcd **event queue** has one producer and one consumer:
  - PRODUCER = the OTG ISR (`dcd_int_handler` -> `dcd_event_handler` ->
    `queue_event` -> `osal_queue_send(..., in_isr=true)`). With `in_isr=true`,
    `osal_queue_send` does **not** touch interrupt masking — it just writes the
    FIFO, trusting that nothing else writes concurrently.
  - CONSUMER = `tud_task` (-> `osal_queue_receive`), which we now run in the
    timer ISR. `osal_queue_receive` brackets the FIFO read with
    `usbd_int_set(false/true)` = `dcd_int_disable/enable` =
    **`NVIC_DisableIRQ/EnableIRQ(OTG_HS)`** (confirmed in
    `portable/synopsys/dwc2/dwc2_stm32.h`). So the consumer masks the producer
    while it reads — independent of which context the consumer runs in.
- The one remaining race is the **reverse**: the timer ISR (consumer) preempting
  the OTG ISR (producer) mid-`tu_fifo_write_n`. That is prevented **only** by the
  two ISRs being at **equal NVIC priority**: on Cortex-M, equal-priority
  exceptions never preempt each other — they tail-chain. So producer and consumer
  are serialised and the single-producer/single-consumer FIFO invariant holds.
- **Therefore the timer ISR MUST be at the same priority as OTG_HS (1). Do NOT
  raise it above OTG.** Both are above the FDCAN IRQs (2), so both still preempt
  the CAN storm — which is the whole point.

`tud_task_ext` is called with `in_isr=false`; that parameter is ignored by the
consumer path (`(void) in_isr;` in `tud_task_ext`) and only the consumer runs in
the timer ISR, so the value is irrelevant here.

## The `set_safety_mode` cross-context race (new in path B)

`set_safety_mode` calls `can_init_all()` (re-inits the FDCAN peripheral). It can
be reached from **two** contexts now:

1. main loop, via `bridge_safety_tick` on heartbeat-loss revert; and
2. the service ISR, via an RSCP `0xDC` control request ->
   `process_control` -> `comms_control_handler` -> `set_safety_mode`.

In path A both ran in the main loop (serialised). In path B the ISR (prio 1) can
preempt the main loop mid-`can_init_all`, corrupting the re-init. Fix: the
main-loop `bridge_safety_tick` is wrapped in `ENTER_CRITICAL`/`EXIT_CRITICAL`
(masks all IRQs) so the service ISR can't preempt it. The mask is held only for
the 1 Hz tick, and only the (rare) mode-change branch does heavy work.
(`bridge_safety_tick` must stay in the main loop precisely because it re-inits
CAN — it must not run from the high-priority servicing ISR.)

## Watchdog caveat

`simple_watchdog` (FAULT_HEARTBEAT_LOOP_WATCHDOG) is now fed by the near-idle
main loop, which no longer does USB/CAN work — so it **no longer detects a wedged
TinyUSB/lwIP**. A wedged service ISR would instead surface as
`FAULT_INTERRUPT_RATE_TICK` (if it overruns) or as stalled USB at the app. Watch
for this on the bench. (For true car auto-recovery, arming the hardware IWDG is
still the right follow-up — unchanged from path A.)

## Build / flash / test (Pi only — cannot build on the Mac)

opendbc isn't on the dev Mac and the build is Pi-hosted (raw-USB host for the
red panda). See `board/bridge/README.md`.

```bash
# On the Pi (from the panda repo root):
board/bridge/tools/pi_build.sh          # build the bridge firmware
# flash per board/bridge/README.md (soft-flasher for the app image)
```

Test sequence on the bench:
1. Flash, confirm enumeration: NCM up, DHCP lease handed out, app connects
   (LED ladder: white -> cyan -> blue -> purple -> green).
2. Idle bus: confirm CAN frames flow to the app and the ~2 Hz health push lands.
3. **The actual repro:** arm a car safety mode so relay-intercept forwarding
   floods the CAN buses, and sustain it. Path A wedged the USB link here; path B
   should keep USB alive. Watch:
   - app stays connected, CAN keeps flowing, RSCP requests still answered;
   - health push `tx_drops` and per-bus `can_health` (bus_off / TEC-REC /
     total_error_cnt) for the error storm;
   - no `FAULT_INTERRUPT_RATE_TICK` and no loop-watchdog fault.
4. Exercise RSCP control under load (read health, set OBD mux, set safety mode)
   to confirm the deferred `process_control` + the safety-tick critical section
   coexist without a CAN re-init glitch.

## Risks / unknowns to watch on the bench

- **`tud_task` from an ISR is unusual.** The analysis says same-priority makes it
  safe, but this hasn't run on hardware. If enumeration is flaky or the link
  drops, suspect the queue locking first.
- **ISR duration.** If the per-tick work (lwIP + 100-frame drain + a health push
  building a ~270 B datagram) ever approaches 500 us, raise the period (lower
  `SERVICE_TIMER_HZ`) or lower the drain budget. The `service_busy` guard drops an
  overrunning tick rather than re-entering.
- **`can_init_all` under the critical section.** Masking all IRQs for a heavy
  re-init at 1 Hz is a brief, bounded USB hiccup; acceptable, but note it if you
  see periodic 1 Hz USB stutter.
- **Watchdog blind spot** (above): a wedged ISR won't trip the loop watchdog.
- **TIM12 assumption.** Confirmed unused by the bridge (no `tick_timer_init`
  call) and RCC-enabled in `peripherals_init`. If a future change starts using
  TICK_TIMER, pick another free TIM (TIM7 is also free).
