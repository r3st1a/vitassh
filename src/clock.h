/*
 * Monotonic milliseconds since boot. sceKernelGetSystemTimeWide() is the only
 * clock a Vita app can rely on without setting up a wall clock.
 */
#ifndef VITASSH_CLOCK_H
#define VITASSH_CLOCK_H

#include <stdint.h>

uint64_t now_ms(void);

#endif /* VITASSH_CLOCK_H */
