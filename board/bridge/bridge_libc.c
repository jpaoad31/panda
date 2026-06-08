// Minimal libc functions that TinyUSB + lwIP reference but the panda's board/libc.h
// doesn't provide. The build links no newlib (-nostdlib), and board/libc.h already
// supplies global memset/memcpy/memcmp; these fill the remaining gaps.
#include <stddef.h>
#include <stdint.h>

void *memmove(void *dest, const void *src, size_t n) {
  uint8_t *d = (uint8_t *)dest;
  const uint8_t *s = (const uint8_t *)src;
  if (d == s || n == 0U) { return dest; }
  if (d < s) {
    for (size_t i = 0U; i < n; i++) { d[i] = s[i]; }
  } else {
    for (size_t i = n; i > 0U; i--) { d[i - 1U] = s[i - 1U]; }
  }
  return dest;
}

size_t strlen(const char *s) {
  size_t n = 0U;
  while (s[n] != '\0') { n++; }
  return n;
}

int strcmp(const char *a, const char *b) {
  while ((*a != '\0') && (*a == *b)) { a++; b++; }
  return (int)(uint8_t)*a - (int)(uint8_t)*b;
}

char *strcpy(char *dest, const char *src) {
  char *d = dest;
  while ((*d++ = *src++) != '\0') { }
  return dest;
}

int strncmp(const char *a, const char *b, size_t n) {
  for (size_t i = 0U; i < n; i++) {
    uint8_t ca = (uint8_t)a[i], cb = (uint8_t)b[i];
    if (ca != cb) { return (int)ca - (int)cb; }
    if (ca == 0U) { break; }
  }
  return 0;
}

int atoi(const char *s) {
  int r = 0, sign = 1;
  while (*s == ' ') { s++; }
  if (*s == '-') { sign = -1; s++; } else if (*s == '+') { s++; }
  while ((*s >= '0') && (*s <= '9')) { r = (r * 10) + (*s - '0'); s++; }
  return r * sign;
}

// lwIP's IP-string parsing uses <ctype.h> (which dereferences newlib's _ctype_
// table). We use only static IPs, so that path is never exercised at runtime — a
// zeroed table just resolves the link symbol.
const char _ctype_[1 + 256] = {0};

// TinyUSB's dwc2 STM32H7 timing reads SystemCoreClock; clock_init() runs the CPU
// at 240 MHz. (Normally provided by CMSIS system_stm32h7xx.c, which the panda
// firmware doesn't compile.)
uint32_t SystemCoreClock = 240000000U;
