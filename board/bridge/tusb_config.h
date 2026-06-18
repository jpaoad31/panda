// TinyUSB config for the RoadStud panda bridge (dedicated CDC-NCM device).
//
// Starting point for the STM32H7 (STM32H725) bridge build. The panda's USB is
// USB_OTG_HS used as a FULL-SPEED device via the embedded FS PHY, which maps to
// TinyUSB's dwc2 port on rhport 1.
//
// Pair this with the descriptors copied VERBATIM from the Pico bridge
// (canbridge/src/usb_descriptors.c) — they already advertise NCM with the
// required bcdUSB=0x0201 + BOS / MS OS 2.0 descriptors. Do not re-derive them.
#pragma once

#define CFG_TUSB_MCU              OPT_MCU_STM32H7
#define CFG_TUSB_OS               OPT_OS_NONE

// The panda runs OTG_HS as a full-speed device (embedded FS PHY). In TinyUSB's
// STM32H7 dwc2 port, OTG_HS is rhport 1.
#define BOARD_TUD_RHPORT          1
#define CFG_TUSB_RHPORT1_MODE     (OPT_MODE_DEVICE | OPT_MODE_FULL_SPEED)

#define CFG_TUD_ENABLED           1

// Memory alignment / placement for the dwc2 core (match other STM32H7 targets).
#ifndef CFG_TUSB_MEM_SECTION
#define CFG_TUSB_MEM_SECTION
#endif
#ifndef CFG_TUSB_MEM_ALIGN
#define CFG_TUSB_MEM_ALIGN        __attribute__((aligned(4)))
#endif

// Endpoint 0 size.
#define CFG_TUD_ENDPOINT0_SIZE    64

// --- Class configuration: NCM only (dedicated bridge device) ---
#define CFG_TUD_CDC               0
#define CFG_TUD_MSC               0
#define CFG_TUD_HID               0
#define CFG_TUD_VENDOR            0
#define CFG_TUD_NCM               1

// Network MTU + NCM buffering. Mirrors the Pico bridge example.
#define CFG_TUD_NET_MTU           1514
#define CFG_TUD_NCM_OUT_NTB_MAX_SIZE 3200
#define CFG_TUD_NCM_IN_NTB_MAX_SIZE  3200
// Transmit (device->host) NTB count. TinyUSB defaults to 1 = no double-buffering,
// so a momentary host-IN gap forced linkoutput_fn to drop. More NTBs let the device
// queue datagrams ahead (each ~3200B NTB holds a couple of our <=1472B datagrams via
// NCM aggregation), cutting drops under armed-mode 2x load. Statically allocated in
// USB-DMA RAM (~3200B each), so it's link-time only — no heap, no runtime risk.
#define CFG_TUD_NCM_IN_NTB_N         8
