/*
 * Host-side stand-in for the vault's crypto primitives.
 *
 * This is NOT cryptography and must never end up in the device binary: it is a
 * splitmix64 keystream with a keyed checksum for a tag, chosen because it is
 * twenty lines and has no dependencies. It exists so src/vault.c -- the slot
 * table, the acceptance rules, the file layout -- can be built and exercised on
 * a machine that has neither libcrypto nor a console's device id.
 *
 * What it does preserve is the property the tests rely on: decryption fails if
 * any byte of the key, the nonce, the header, the ciphertext or the tag
 * changes. Tamper detection is what the tests are checking; confidentiality is
 * not, and is not tested here.
 */
#include "vault_platform.h"

#include <string.h>

#include "vault_platform_stub.h"

static const uint8_t default_secret[VAULT_SECRET_LEN] = {
	0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88,
	0x99, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x00,
};

static uint8_t stub_secret[VAULT_SECRET_LEN];
static uint64_t stub_counter;
static int stub_random_fails;
static int stub_ready;

static uint64_t mix64(uint64_t z)
{
	z += 0x9E3779B97F4A7C15ULL;
	z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
	z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
	return z ^ (z >> 31);
}

static void stub_ensure(void)
{
	if (stub_ready) return;

	memcpy(stub_secret, default_secret, sizeof(stub_secret));
	stub_counter = 0;
	stub_random_fails = 0;
	stub_ready = 1;
}

void vault_stub_reset(void)
{
	stub_ready = 0;
	stub_ensure();
}

void vault_stub_set_secret(const uint8_t secret[VAULT_SECRET_LEN])
{
	stub_ensure();
	memcpy(stub_secret, secret, VAULT_SECRET_LEN);
}

void vault_stub_fail_random(int on)
{
	stub_ensure();
	stub_random_fails = on;
}

int vault_device_secret(uint8_t out[VAULT_SECRET_LEN])
{
	stub_ensure();
	memcpy(out, stub_secret, VAULT_SECRET_LEN);
	return 0;
}

int vault_random(void *out, size_t len)
{
	uint8_t *p = out;
	uint64_t z = 0;
	size_t i;

	stub_ensure();
	if (stub_random_fails) return -1;

	/* Counter based, so a reset replays the same sequence and a test that
	 * does not reset still gets different bytes on every call. */
	for (i = 0; i < len; i++) {
		if (i % 8 == 0) {
			stub_counter++;
			z = mix64(0x0C0FFEE1234567ULL ^ stub_counter);
		}
		p[i] = (uint8_t)(z >> (8 * (i % 8)));
	}
	return 0;
}

static uint64_t stream_seed(const uint8_t key[VAULT_KEY_LEN],
                            const uint8_t iv[VAULT_IV_LEN], uint64_t domain)
{
	uint64_t s = 0x243F6A8885A308D3ULL ^ domain;
	size_t i;

	for (i = 0; i < VAULT_KEY_LEN; i++) s = mix64(s ^ key[i]);
	for (i = 0; i < VAULT_IV_LEN; i++) s = mix64(s ^ iv[i]);
	return s;
}

static void stub_stream(uint64_t *s, uint8_t *buf, size_t len)
{
	size_t i;

	for (i = 0; i < len; i++) {
		if (i % 8 == 0) *s = mix64(*s);
		buf[i] ^= (uint8_t)(*s >> (8 * (i % 8)));
	}
}

static void stub_xor(const uint8_t key[VAULT_KEY_LEN],
                     const uint8_t iv[VAULT_IV_LEN], uint8_t *buf, size_t len)
{
	uint64_t s = stream_seed(key, iv, 0);

	stub_stream(&s, buf, len);
}

/* A keyed checksum over everything that must not change: header then data. */
static void stub_tag(const uint8_t key[VAULT_KEY_LEN],
                     const uint8_t iv[VAULT_IV_LEN],
                     const void *aad, size_t aad_len,
                     const uint8_t *data, size_t len,
                     uint8_t tag[VAULT_TAG_LEN])
{
	const uint8_t *a = aad;
	uint64_t s = stream_seed(key, iv, 0x544147ULL);
	size_t i;

	for (i = 0; i < aad_len; i++) s = mix64(s ^ a[i]);
	for (i = 0; i < len; i++) s = mix64(s ^ data[i]);

	for (i = 0; i < VAULT_TAG_LEN; i++) {
		if (i % 8 == 0) s = mix64(s);
		tag[i] = (uint8_t)(s >> (8 * (i % 8)));
	}
}

int vault_kdf(const uint8_t *secret, size_t secret_len,
              const uint8_t salt[VAULT_SALT_LEN], uint8_t key[VAULT_KEY_LEN])
{
	uint64_t s = 0x452821E638D01377ULL;
	size_t i;

	for (i = 0; i < secret_len; i++) s = mix64(s ^ secret[i]);
	for (i = 0; i < VAULT_SALT_LEN; i++) s = mix64(s ^ salt[i]);

	/* Not a work factor, just enough mixing that a wrong call order shows up
	 * as a wrong key rather than as a coincidence. */
	for (i = 0; i < 1000; i++) s = mix64(s);

	for (i = 0; i < VAULT_KEY_LEN; i++) {
		if (i % 8 == 0) s = mix64(s);
		key[i] = (uint8_t)(s >> (8 * (i % 8)));
	}
	return 0;
}

int vault_aead_encrypt(const uint8_t key[VAULT_KEY_LEN],
                       const uint8_t iv[VAULT_IV_LEN],
                       const void *aad, size_t aad_len,
                       const uint8_t *in, size_t len,
                       uint8_t *out, uint8_t tag[VAULT_TAG_LEN])
{
	memcpy(out, in, len);
	stub_xor(key, iv, out, len);
	stub_tag(key, iv, aad, aad_len, out, len, tag);
	return 0;
}

int vault_aead_decrypt(const uint8_t key[VAULT_KEY_LEN],
                       const uint8_t iv[VAULT_IV_LEN],
                       const void *aad, size_t aad_len,
                       const uint8_t *in, size_t len,
                       const uint8_t tag[VAULT_TAG_LEN], uint8_t *out)
{
	uint8_t want[VAULT_TAG_LEN];

	stub_tag(key, iv, aad, aad_len, in, len, want);
	if (memcmp(want, tag, VAULT_TAG_LEN) != 0) return -1;

	memcpy(out, in, len);
	stub_xor(key, iv, out, len);
	return 0;
}

void vault_wipe(void *p, size_t n)
{
	volatile uint8_t *v = p;

	while (n--) *v++ = 0;
}

int vault_selftest(void)
{
	/* The toy above has no library behind it to get wrong. */
	return 0;
}
