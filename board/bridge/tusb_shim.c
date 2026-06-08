// Thin TinyUSB init wrapper. Kept separate so tusb.h (not -Werror/-Wstrict-
// prototypes clean) stays out of the strict unity TU (main_bridge.c).
#include "tusb.h"

void tusb_init_bridge(void) {
  tusb_rhport_init_t dev_init = {
    .role = TUSB_ROLE_DEVICE,
    .speed = TUSB_SPEED_FULL,
  };
  tusb_init(BOARD_TUD_RHPORT, &dev_init);
}
