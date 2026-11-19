/*
 * The primitives the password vault needs from the platform, and nothing else.
 *
 * They live behind this interface for one reason: the device build gets them
 * from libcrypto and the kernel, and the host test build has neither. Keeping
 * the split here means src/vault.c -- where the slot layout, the key comparison
 * and the file format live -- is compiled and exercised on the host, which is
 * where the mistakes that matter are made.
 *
 * Two implementations exist:
 *
 *   src/vault_platform_ossl.c   AES-256-GCM + PBKDF2 + OpenPsId. The real one.
 *   test/vault_platform_stub.c  A deterministic toy, host tests only.
 */
#ifndef VITASSH_VAULT_PLATFORM_H
#define VITASSH_VAULT_PLATFORM_H

#include <stddef.h>
#include <stdint.h>

#define VAULT_KEY_LEN 32
#define VAULT_SALT_LEN 16
#define VAULT_IV_LEN 12
#define VAULT_TAG_LEN 16
#define VAULT_SECRET_LEN 16

/*
 * Stretches the per-console secret and the file's salt into the vault key.
 * Returns 0 on success.
 */
int vault_kdf(const uint8_t *secret, size_t secret_len,
              const uint8_t salt[VAULT_SALT_LEN], uint8_t key[VAULT_KEY_LEN]);

/*
 * AES-256-GCM. out must have room for len bytes and may not overlap in. The
 * tag is always VAULT_TAG_LEN bytes. Returns 0 on success.
 */
int vault_aead_encrypt(const uint8_t key[VAULT_KEY_LEN],
                       const uint8_t iv[VAULT_IV_LEN],
                       const void *aad, size_t aad_len,
                       const uint8_t *in, size_t len,
                       uint8_t *out, uint8_t tag[VAULT_TAG_LEN]);

/*
 * Returns 0 only when the tag matches, in which case out holds len bytes.
 * A non-zero return means the data is not what was written -- wrong key,
 * altered file or both -- and out must be treated as garbage.
 */
int vault_aead_decrypt(const uint8_t key[VAULT_KEY_LEN],
                       const uint8_t iv[VAULT_IV_LEN],
                       const void *aad, size_t aad_len,
                       const uint8_t *in, size_t len,
                       const uint8_t tag[VAULT_TAG_LEN], uint8_t *out);

/*
 * Fills out with len cryptographically unpredictable bytes. Must not fail
 * quietly: the caller aborts the save when this returns non-zero rather than
 * fall back to a constant, because a predictable GCM nonce breaks the cipher
 * outright.
 */
int vault_random(void *out, size_t len);

/*
 * The identifier the vault is bound to, so that secrets.dat copied off the
 * memory card is useless on its own. Returns 0 on success.
 */
int vault_device_secret(uint8_t out[VAULT_SECRET_LEN]);

/* Scrubs n bytes in a way the compiler may not optimise away. */
void vault_wipe(void *p, size_t n);

/*
 * Known-answer tests over the primitives above, run once at startup so a
 * mis-linked or mis-called libcrypto is caught on the device instead of
 * silently producing files nothing can read. Returns 0 when every vector
 * passed, otherwise the 1-based number of the first one that failed.
 */
int vault_selftest(void);

#endif /* VITASSH_VAULT_PLATFORM_H */
