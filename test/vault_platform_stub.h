/*
 * Knobs the host tests need on the toy primitives in vault_platform_stub.c.
 * Test-only: nothing under src/ may include this.
 */
#ifndef VITASSH_VAULT_PLATFORM_STUB_H
#define VITASSH_VAULT_PLATFORM_STUB_H

#include <stdint.h>

#include "vault_platform.h"

/* Back to the default device secret, a fresh random sequence, a working RNG. */
void vault_stub_reset(void);

/* Pretend to be a different console, so files written before stop decrypting. */
void vault_stub_set_secret(const uint8_t secret[VAULT_SECRET_LEN]);

/* Make vault_random() fail, to prove a save is abandoned and not weakened. */
void vault_stub_fail_random(int on);

#endif /* VITASSH_VAULT_PLATFORM_STUB_H */
