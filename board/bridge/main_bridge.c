// RoadStud panda bridge firmware — unity entry TU.
//
// Forks board/main.c's low-level bring-up (clock, peripherals, board, CAN, timers)
// but replaces the panda USB class with TinyUSB CDC-NCM and runs the CAN<->UDP
// bridge loop. Like board/main.c this is a single translation unit that pulls in
// the panda driver headers (with bodies); TinyUSB + lwIP + the bridge glue compile
// as separate TUs and link against the globals defined here (incl. memcpy/memset
// from board/libc.h, and bridge_now_ms()).
#include "board/config.h"

// Match board/main.c's full driver include set so the monolithic board init
// (current_board->init() pulls in pwm/spi/fan/comms/usb) links. We never call the
// panda usb_init(), so its USB class stays dormant — the OTG IRQ is routed to
// TinyUSB below.
#include "board/drivers/led.h"
#include "board/drivers/pwm.h"
#include "board/drivers/usb.h"
#include "board/drivers/simple_watchdog.h"
#include "board/drivers/bootkick.h"
#include "board/early_init.h"
#include "board/provision.h"

#include "opendbc/safety/safety.h"
#include "board/health.h"
#include "board/drivers/can_common.h"
#include "board/drivers/fdcan.h"
#include "board/sys/power_saving.h"

#include "board/obj/gitversion.h"

#include "board/can_comms.h"
#include "board/main_comms.h"

// --- TinyUSB device-controller interrupt entry (dwc2) ---
extern void dcd_int_handler(uint8_t rhport);
extern void tusb_init_bridge(void);   // thin wrapper, defined below

// --- bridge glue (separate TUs) ---
void bridge_net_init(void);      // lwip_glue.c: lwIP + DHCP + DNS
void bridge_net_service(void);   // lwip_glue.c: sys_check_timeouts()
bool bridge_net_usb_ready(void);      // lwip_glue.c: NCM device configured
bool bridge_net_has_dhcp_lease(void); // lwip_glue.c: a host holds a DHCP lease
void bridge_can_init(void);      // bridge_can.c: UDP :5555 server
void bridge_can_poll(uint32_t now_ms);
bool bridge_can_client_connected(void);          // bridge_can.c
bool bridge_can_frames_flowing(uint32_t now_ms); // bridge_can.c

// TinyUSB needs these; declared minimally to avoid pulling tusb.h into this TU
// (tusb headers aren't -Werror/-Wstrict-prototypes clean).
extern void tud_task_ext(uint32_t timeout_ms, bool in_isr);
#define BRIDGE_RHPORT 1U

// Required by the startup file.
void __initialize_hardware_early(void) {
  early_initialization();
}

// main_comms.h references these (defined in board/main.c, which we don't compile).
// Copied verbatim so the panda comms layer links; the bridge itself doesn't drive
// safety-mode changes (CAN stays in its default SILENT state).
void set_safety_mode(uint16_t mode, uint16_t param) {
  uint16_t mode_copy = mode;
  int err = set_safety_hooks(mode_copy, param);
  if (err == -1) {
    print("Error: safety set mode failed. Falling back to SILENT\n");
    mode_copy = SAFETY_SILENT;
    err = set_safety_hooks(mode_copy, 0U);
    assert_fatal(err == 0, "Error: Failed setting SILENT mode. Hanging\n");
  }
  safety_tx_blocked = 0;
  safety_rx_invalid = 0;

  switch (mode_copy) {
    case SAFETY_SILENT:
      set_intercept_relay(false, false);
      current_board->set_can_mode(CAN_MODE_NORMAL);
      can_silent = true;
      break;
    case SAFETY_NOOUTPUT:
      set_intercept_relay(false, false);
      current_board->set_can_mode(CAN_MODE_NORMAL);
      can_silent = false;
      break;
    case SAFETY_ELM327:
      set_intercept_relay(false, false);
      heartbeat_counter = 0U;
      heartbeat_lost = false;
      can_clear_send(CANIF_FROM_CAN_NUM(1), 1);
      if (param == 0U) {
        current_board->set_can_mode(CAN_MODE_OBD_CAN2);
      } else {
        current_board->set_can_mode(CAN_MODE_NORMAL);
      }
      can_silent = false;
      break;
    default:
      set_intercept_relay(true, false);
      heartbeat_counter = 0U;
      heartbeat_lost = false;
      current_board->set_can_mode(CAN_MODE_NORMAL);
      can_silent = false;
      break;
  }
  can_init_all();
}

bool is_car_safety_mode(uint16_t mode) {
  return (mode != SAFETY_SILENT) &&
         (mode != SAFETY_NOOUTPUT) &&
         (mode != SAFETY_ALLOUTPUT) &&
         (mode != SAFETY_ELM327);
}

// Some driver headers expect this symbol (UART debug echo); keep it a no-op.
void debug_ring_callback(uart_ring *ring) { (void)ring; }

static void __attribute__((noinline)) enable_fpu(void) {
  SCB->CPACR |= ((3UL << (10U * 2U)) | (3UL << (11U * 2U)));
}

// Millisecond clock for lwIP + TinyUSB + the bridge poll.
uint32_t bridge_now_ms(void) {
  return microsecond_timer_get() / 1000U;
}

// Re-flash escape hatch (see board/bridge/README.md "Re-flashing gotcha"). The NCM
// app has no panda control endpoint, so a host can't request enter-bootstub the
// normal way; a magic UDP datagram routes here instead. We set the bootstub
// re-entry magic — it lives in SRAM and survives the soft reset — then reset. Next
// boot the bootstub runs its flasher (soft-flasher for the app, ST DFU for itself).
void bridge_request_reboot(bool dfu) {
  enter_bootloader_mode = dfu ? ENTER_BOOTLOADER_MAGIC : ENTER_SOFTLOADER_MAGIC;
  NVIC_SystemReset();
}

// Bring-up status LED (red panda RGB on GPIOE 4/3/2). Worst-state-wins, mirroring
// the Pico bridge ladder so a sealed box is debuggable at a glance.
static void bridge_update_led(uint32_t now_ms) {
  bool r = false, g = false, b = false;
  if (!bridge_net_usb_ready()) {
    r = true; g = true; b = true;             // white: USB/NCM not up yet
  } else if (!bridge_net_has_dhcp_lease()) {
    g = true; b = true;                       // cyan: NCM up, host has no lease
  } else if (!bridge_can_client_connected()) {
    b = true;                                 // blue: lease, app not connected
  } else if (!bridge_can_frames_flowing(now_ms)) {
    r = true; b = true;                       // purple: app connected, no CAN yet
  } else {
    g = true;                                 // green: healthy, CAN flowing
  }
  led_set(LED_RED, r);
  led_set(LED_GREEN, g);
  led_set(LED_BLUE, b);
}

// go into SILENT when no heartbeat is received for this many seconds (stock values).
#define HEARTBEAT_IGNITION_CNT_ON  5U
#define HEARTBEAT_IGNITION_CNT_OFF 2U

// Port of the safety-relevant subset of board/main.c's 1Hz tick. The bridge's main
// loop replaces the stock 8Hz TICK_TIMER, so without this the heartbeat / controls
// bookkeeping never runs and set_safety_mode + heartbeat couldn't gate
// controls_allowed the way openpilot expects. Driven at 1Hz from main().
//
// Deliberate deviations from stock, appropriate for an OBD/bench bridge (not a
// relay-intercept install):
//   - peripheral ticks (fan/siren/sound/bootkick/IR/power-save) and the
//     harness-orientation re-init are omitted.
//   - the heartbeat-loss -> SILENT revert is gated on is_car_safety_mode(), so the
//     read-only NOOUTPUT boot state is NOT forced to SILENT when no heartbeat flows
//     (we only require a heartbeat once the app has armed a real car safety mode).
//   - stock's controls_allowed_countdown (used only to time the siren, which the
//     bridge has no use for) is dropped; the revert is driven by heartbeat_counter.
static void bridge_safety_tick(void) {
  if (heartbeat_counter < UINT32_MAX) { heartbeat_counter += 1U; }

  // disabling the heartbeat is not allowed while in a car safety mode
  if (is_car_safety_mode(current_safety_mode)) { heartbeat_disabled = false; }

  // drop controls if openpilot stops requesting engagement (stock 3-strike check)
  if (controls_allowed && !heartbeat_engaged) {
    heartbeat_engaged_mismatches += 1U;
    if (heartbeat_engaged_mismatches >= 3U) {
      controls_allowed = false;
    }
  } else {
    heartbeat_engaged_mismatches = 0U;
  }

  const bool started = harness_check_ignition() || ignition_can;

  // heartbeat gone too long -> revert to SILENT (only while actually armed in a car
  // mode; NOOUTPUT/ELM327 read-only states are left untouched).
  if (!heartbeat_disabled && is_car_safety_mode(current_safety_mode)) {
    if (heartbeat_counter >= (started ? HEARTBEAT_IGNITION_CNT_ON : HEARTBEAT_IGNITION_CNT_OFF)) {
      heartbeat_lost = true;
      heartbeat_engaged = false;
      set_safety_mode(SAFETY_SILENT, 0U);
    }
  }

  // clear CAN-detected ignition after a couple seconds of silence
  if (ignition_can_cnt > 2U) { ignition_can = false; }

  uptime_cnt += 1U;
  safety_mode_cnt += 1U;
  ignition_can_cnt += 1U;

  // synchronous safety check
  safety_tick(&current_safety_config);
}

// OTG_HS interrupt -> TinyUSB dwc2 handler.
static void bridge_otg_irq(void) {
  NVIC_DisableIRQ(OTG_HS_IRQn);
  dcd_int_handler(BRIDGE_RHPORT);
  NVIC_EnableIRQ(OTG_HS_IRQn);
}

int main(void) {
  init_interrupts(true);
  disable_interrupts();

  clock_init();
  peripherals_init();
  detect_board_type();
  led_init();
  adc_init(ADC1);

  // Board init sets up USB + CAN GPIO alternate functions.
  current_board->init();
  current_board->set_can_mode(CAN_MODE_NORMAL);
  harness_init();

  enable_fpu();
  microsecond_timer_init();

  // Bring up CAN via NOOUTPUT: this initialises the safety hooks (so safety_tick
  // and the TX gate work) while keeping CAN in normal mode — receive everything,
  // ACK frames, but block all transmits until the app arms a car safety mode.
  // Mirrors how the stock firmware boots into a known safety state (board/main.c).
  set_safety_mode(SAFETY_NOOUTPUT, 0U);   // calls can_init_all()
  enable_can_transceivers(true);

  // Route the OTG interrupt to TinyUSB.
  REGISTER_INTERRUPT(OTG_HS_IRQn, bridge_otg_irq, 1500000U, FAULT_INTERRUPT_RATE_USB)
  NVIC_EnableIRQ(OTG_HS_IRQn);

  // Interrupt priority: USB/OTG must out-rank the CAN ISRs (lower number = higher
  // priority on Cortex-M). When armed, the relay-intercept forwarding floods the
  // FDCAN interrupts (2x traffic + a forward per frame, and an ACK/error storm if
  // the intercept upsets the car's bus). With all IRQs at the reset-default 0 they
  // can't preempt each other, so a CAN storm starves the OTG ISR and the USB link
  // wedges (needs a replug). Demote the CAN IRQs below OTG so USB always gets
  // serviced. (The USB *processing* — tud_task/lwIP — still runs in the main loop;
  // this protects the hardware-level ISR. Throttling the CAN error storm is the
  // companion fix once the can_health data confirms it.)
  NVIC_SetPriority(OTG_HS_IRQn, 1U);
  const IRQn_Type bridge_can_irqs[] = {
    FDCAN1_IT0_IRQn, FDCAN1_IT1_IRQn, FDCAN2_IT0_IRQn,
    FDCAN2_IT1_IRQn, FDCAN3_IT0_IRQn, FDCAN3_IT1_IRQn,
  };
  for (unsigned int i = 0U; i < (sizeof(bridge_can_irqs) / sizeof(bridge_can_irqs[0])); i++) {
    NVIC_SetPriority(bridge_can_irqs[i], 2U);
  }

  enable_interrupts();

  // TinyUSB device stack on OTG_HS (rhport 1, full speed).
  tusb_init_bridge();

  // lwIP + DHCP/DNS, then the CAN<->UDP bridge.
  bridge_net_init();
  bridge_can_init();

  // Software liveness watchdog: records FAULT_HEARTBEAT_LOOP_WATCHDOG if the main
  // loop stalls past the threshold (reported to the app; does not self-reset). Fed
  // every iteration, so it directly reflects loop health — e.g. a wedged TinyUSB
  // xmit-wait or lwIP deadlock would trip it. NOTE: for true auto-recovery on a car
  // (no host to power-cycle a hung bridge over USB), arm the hardware IWDG too.
  simple_watchdog_init(FAULT_HEARTBEAT_LOOP_WATCHDOG, (3U * 1000000U / 8U));

  uint32_t last_led_ms = 0U;
  uint32_t last_safety_ms = 0U;
  while (true) {
    simple_watchdog_kick();
    tud_task_ext(0U, false);     // service USB
    bridge_net_service();        // pump lwIP timers + RX
    uint32_t now = bridge_now_ms();
    bridge_can_poll(now);
    if ((now - last_safety_ms) >= 1000U) {   // 1 Hz safety/heartbeat bookkeeping
      last_safety_ms = now;
      bridge_safety_tick();
    }
    if ((now - last_led_ms) >= 100U) {   // throttle GPIO writes to ~10 Hz
      last_led_ms = now;
      bridge_update_led(now);
    }
  }
  return 0;
}
