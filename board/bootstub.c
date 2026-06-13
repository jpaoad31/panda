#define VERS_TAG 0x53524556
#define MIN_VERSION 2

// ********************* Includes *********************
#include "board/config.h"

#include "board/drivers/led.h"
#include "board/drivers/pwm.h"
#include "board/drivers/usb.h"

#include "board/early_init.h"
#include "board/provision.h"

#include "board/crypto/rsa.h"
#include "board/crypto/sha.h"

#include "board/obj/cert.h"
#include "board/obj/gitversion.h"
#include "board/flasher.h"

// cppcheck-suppress unusedFunction ; used in headers not included in cppcheck
void __initialize_hardware_early(void) {
  early_initialization();
}

void fail(void) {
  soft_flasher_start();
}

// know where to sig check
extern void *_app_start[];

int main(void) {
  // Init interrupt table
  init_interrupts(true);

  disable_interrupts();
  clock_init();
  detect_board_type();

#ifdef PANDA_JUNGLE
  current_board->set_panda_power(true);
#endif

  if (enter_bootloader_mode == ENTER_SOFTLOADER_MAGIC) {
    enter_bootloader_mode = 0;
    soft_flasher_start();
  }

#ifdef BRIDGE_DEV_FLASH_WINDOW
  // RoadStud dev bootstub: on every boot, offer a flash window before launching the
  // app, so a wedged NCM app is always recoverable over USB (see board/bridge/README.md).
  // The skip flag lives in SRAM (survives the soft reset) so the post-window boot goes
  // straight to the app on a fresh USB peripheral instead of re-opening the window.
  #define BRIDGE_BOOT_SKIP       (*(volatile uint32_t *)0x38001FF8U)
  #define BRIDGE_BOOT_SKIP_MAGIC 0x50494B53U   // 'SKIP'
  if (BRIDGE_BOOT_SKIP == BRIDGE_BOOT_SKIP_MAGIC) {
    BRIDGE_BOOT_SKIP = 0U;               // consume: this boot launches the app
  } else {
    soft_flasher_window();               // returns only if no host flashed in the window
    BRIDGE_BOOT_SKIP = BRIDGE_BOOT_SKIP_MAGIC;
    NVIC_SystemReset();                  // clean reboot -> app comes up on a fresh USB
  }
#endif

  // validate length
  int len = (int)_app_start[0];
  if ((len < 8) || (len > (0x1000000 - 0x4000 - 4 - RSANUMBYTES))) goto fail;

  // compute SHA hash
  uint8_t digest[SHA_DIGEST_SIZE];
  SHA_hash(&_app_start[1], len-4, digest);

  // verify version, last bytes in the signed area
  uint32_t vers[2] = {0};
  memcpy(&vers, ((void*)&_app_start[0]) + len - sizeof(vers), sizeof(vers));
  if (vers[0] != VERS_TAG || vers[1] < MIN_VERSION) {
    goto fail;
  }

  // verify RSA signature
  if (RSA_verify(&release_rsa_key, ((void*)&_app_start[0]) + len, RSANUMBYTES, digest, SHA_DIGEST_SIZE)) {
    goto good;
  }

  // allow debug if built from source
#ifdef ALLOW_DEBUG
  if (RSA_verify(&debug_rsa_key, ((void*)&_app_start[0]) + len, RSANUMBYTES, digest, SHA_DIGEST_SIZE)) {
    goto good;
  }
#endif

// here is a failure
fail:
  fail();
  return 0;
good:
  // jump to flash
  ((void(*)(void)) _app_start[1])();
  return 0;
}
