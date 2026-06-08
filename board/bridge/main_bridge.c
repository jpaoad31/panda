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
void bridge_can_init(void);      // bridge_can.c: UDP :5555 server
void bridge_can_poll(uint32_t now_ms);

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

  // Bring up CAN RX (no safety hooks needed for read forwarding; TX is gated by
  // the safety model, left in its default SILENT state).
  can_silent = false;
  can_init_all();
  enable_can_transceivers(true);

  // Route the OTG interrupt to TinyUSB.
  REGISTER_INTERRUPT(OTG_HS_IRQn, bridge_otg_irq, 1500000U, FAULT_INTERRUPT_RATE_USB)
  NVIC_EnableIRQ(OTG_HS_IRQn);

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

  while (true) {
    simple_watchdog_kick();
    tud_task_ext(0U, false);     // service USB
    bridge_net_service();        // pump lwIP timers + RX
    bridge_can_poll(bridge_now_ms());
  }
  return 0;
}
