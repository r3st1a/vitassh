/*
 * The vault's primitives, on the console: libcrypto for the cipher and the KDF,
 * the kernel for the device id and for random bytes.
 *
 * This is the security boundary of the whole feature -- everything else in the
 * vault is bookkeeping -- so it is the one file that has to be right about
 * every argument it passes. The known-answer self-test at the bottom is here
 * because a wrong argument to EVP does not fail loudly: it produces a tag that
 * verifies against itself and a file nothing else can read.
 */
#include "vault_platform.h"

#include <psp2/kernel/openpsid.h>
#include <psp2/kernel/rng.h>

#include <openssl/crypto.h>
#include <openssl/evp.h>

#include <limits.h>
#include <string.h>

#include "log.h"

/*
 * The work factor is written here rather than stored in the file. Storing it
 * would mean reading it before the key exists, which lets a corrupt file ask
 * for a billion rounds and hang the console at startup. Changing it means a new
 * magic, since the file has to be re-derived anyway.
 *
 * It is deliberately low: the input is a 128-bit hardware identifier, not a
 * password somebody chose, so there is no guessing to make expensive. This is
 * not a master-password vault and does not pretend to be one -- see README.
 */
#define VAULT_KDF_ITERATIONS 10000

/* The kernel generator refuses anything larger in one call. */
#define VAULT_RNG_MAX 64

int vault_kdf(const uint8_t *secret, size_t secret_len,
              const uint8_t salt[VAULT_SALT_LEN], uint8_t key[VAULT_KEY_LEN])
{
	if (secret == NULL || salt == NULL || key == NULL) return -1;
	if (secret_len == 0 || secret_len > VAULT_RNG_MAX) return -1;

	if (PKCS5_PBKDF2_HMAC((const char *)secret, (int)secret_len, salt,
	                      VAULT_SALT_LEN, VAULT_KDF_ITERATIONS,
	                      EVP_sha256(), VAULT_KEY_LEN, key) != 1)
		return -1;

	return 0;
}

int vault_aead_encrypt(const uint8_t key[VAULT_KEY_LEN],
                       const uint8_t iv[VAULT_IV_LEN],
                       const void *aad, size_t aad_len,
                       const uint8_t *in, size_t len,
                       uint8_t *out, uint8_t tag[VAULT_TAG_LEN])
{
	EVP_CIPHER_CTX *ctx;
	uint8_t tail[EVP_MAX_BLOCK_LENGTH];
	int n = 0;
	int rc = -1;

	if (key == NULL || iv == NULL || tag == NULL) return -1;
	if ((aad_len > 0 && aad == NULL) || (len > 0 && (in == NULL || out == NULL)))
		return -1;
	if (aad_len > INT_MAX || len > INT_MAX) return -1;

	ctx = EVP_CIPHER_CTX_new();
	if (ctx == NULL) return -1;

	if (EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), NULL, NULL, NULL) != 1)
		goto done;
	if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, VAULT_IV_LEN,
	                        NULL) != 1)
		goto done;
	if (EVP_EncryptInit_ex(ctx, NULL, NULL, key, iv) != 1) goto done;

	/* NULL output means "this is additional data": authenticated, not
	 * encrypted. It has to go in before the plaintext. */
	if (aad_len > 0 && EVP_EncryptUpdate(ctx, NULL, &n, aad, (int)aad_len) != 1)
		goto done;
	if (len > 0 && EVP_EncryptUpdate(ctx, out, &n, in, (int)len) != 1)
		goto done;

	/* GCM emits nothing here, but the call is what finalises the tag and an
	 * empty plaintext is a legitimate input, so it is always made. */
	if (EVP_EncryptFinal_ex(ctx, tail, &n) != 1) goto done;
	if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, VAULT_TAG_LEN, tag) != 1)
		goto done;

	rc = 0;

done:
	EVP_CIPHER_CTX_free(ctx);
	return rc;
}

int vault_aead_decrypt(const uint8_t key[VAULT_KEY_LEN],
                       const uint8_t iv[VAULT_IV_LEN],
                       const void *aad, size_t aad_len,
                       const uint8_t *in, size_t len,
                       const uint8_t tag[VAULT_TAG_LEN], uint8_t *out)
{
	EVP_CIPHER_CTX *ctx;
	uint8_t tail[EVP_MAX_BLOCK_LENGTH];
	int n = 0;
	int rc = -1;

	if (key == NULL || iv == NULL || tag == NULL) return -1;
	if ((aad_len > 0 && aad == NULL) || (len > 0 && (in == NULL || out == NULL)))
		return -1;
	if (aad_len > INT_MAX || len > INT_MAX) return -1;

	ctx = EVP_CIPHER_CTX_new();
	if (ctx == NULL) return -1;

	if (EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), NULL, NULL, NULL) != 1)
		goto done;
	if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, VAULT_IV_LEN,
	                        NULL) != 1)
		goto done;
	if (EVP_DecryptInit_ex(ctx, NULL, NULL, key, iv) != 1) goto done;

	if (aad_len > 0 && EVP_DecryptUpdate(ctx, NULL, &n, aad, (int)aad_len) != 1)
		goto done;
	if (len > 0 && EVP_DecryptUpdate(ctx, out, &n, in, (int)len) != 1)
		goto done;

	/* The tag has to be set after the data and before the final call, which
	 * is where GCM actually checks it. Everything GCM already wrote to out
	 * is unauthenticated until this returns, which is why the caller is told
	 * to discard out when it fails. */
	if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, VAULT_TAG_LEN,
	                        (void *)tag) != 1)
		goto done;
	if (EVP_DecryptFinal_ex(ctx, tail, &n) != 1) goto done;

	rc = 0;

done:
	EVP_CIPHER_CTX_free(ctx);
	return rc;
}

int vault_random(void *out, size_t len)
{
	uint8_t *p = out;

	if (out == NULL && len > 0) return -1;

	/*
	 * There is no weaker fallback and there must not be one: a predictable
	 * GCM nonce under a reused key is a break, not a weakness, so a failure
	 * here has to abandon the save.
	 */
	while (len > 0) {
		SceSize chunk = (len > VAULT_RNG_MAX) ? VAULT_RNG_MAX : (SceSize)len;

		if (sceKernelGetRandomNumber(p, chunk) < 0) return -1;

		p += chunk;
		len -= chunk;
	}
	return 0;
}

int vault_device_secret(uint8_t out[VAULT_SECRET_LEN])
{
	SceKernelOpenPsId id;
	int i;
	int nonzero = 0;

	if (out == NULL) return -1;
	if (sceKernelGetOpenPsId(&id) < 0) return -1;

	for (i = 0; i < VAULT_SECRET_LEN; i++)
		if (id.id[i] != 0) nonzero = 1;

	/*
	 * An all-zero id is what a failed or intercepted call looks like. A key
	 * derived from it would be a key every console with the same problem
	 * shares, so it is refused rather than used.
	 */
	if (!nonzero) return -1;

	memcpy(out, id.id, VAULT_SECRET_LEN);
	OPENSSL_cleanse(&id, sizeof(id));
	return 0;
}

void vault_wipe(void *p, size_t n)
{
	if (p != NULL && n > 0) OPENSSL_cleanse(p, n);
}

/* --- known answers ------------------------------------------------------ */

/*
 * Every constant below was produced by an independent implementation
 * (OpenSSL 3.5.7's `openssl kdf` and `openssl enc -aes-256-ecb`) rather than
 * copied from this code's own output, which is the only thing that makes them
 * worth checking against.
 *
 * The vectors are deliberately the two calls production actually makes --
 * PBKDF2-SHA256 with a 16-byte secret and a 16-byte salt, and AES-256-GCM over
 * a block-aligned buffer -- instead of the RFC's convenience cases, so that the
 * arguments under test are the arguments used.
 */
static const uint8_t kat_secret[VAULT_SECRET_LEN] = {
	0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
	0x88, 0x99, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF,
};

static const uint8_t kat_salt[VAULT_SALT_LEN] = {
	0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
	0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F,
};

static const uint8_t kat_key[VAULT_KEY_LEN] = {
	0x54, 0x16, 0x8F, 0x5B, 0x61, 0x8C, 0x2C, 0x92,
	0xB3, 0x82, 0x35, 0x16, 0xAD, 0xEE, 0x0D, 0x8C,
	0xAA, 0x4D, 0xF9, 0xA6, 0xE1, 0x11, 0x7D, 0x96,
	0x4E, 0xDC, 0xDF, 0x8F, 0x95, 0x27, 0x6F, 0xE1,
};

/* AES-256-GCM under an all-zero key and nonce. The tag over no data at all is
 * E(K, 0^127||1); the ciphertext of one zero block is E(K, 0^127||2). Both come
 * straight out of AES-256-ECB, which is how they were checked. */
static const uint8_t zero_key[VAULT_KEY_LEN];
static const uint8_t zero_iv[VAULT_IV_LEN];

static const uint8_t kat_gcm_tag[VAULT_TAG_LEN] = {
	0x53, 0x0F, 0x8A, 0xFB, 0xC7, 0x45, 0x36, 0xB9,
	0xA9, 0x63, 0xB4, 0xF1, 0xC4, 0xCB, 0x73, 0x8B,
};

static const uint8_t kat_gcm_ct[16] = {
	0xCE, 0xA7, 0x40, 0x3D, 0x4D, 0x60, 0x6B, 0x6E,
	0x07, 0x4E, 0xC5, 0xD3, 0xBA, 0xF3, 0x9D, 0x18,
};

/* What a save packs: 16 slots of 226 bytes. Spelled out so that this file and
 * vault.c are two places that have to agree. */
#define SELFTEST_LEN 3616

static int report(int vector)
{
	log_printf("vault: crypto self-test vector %d failed", vector);
	return vector;
}

int vault_selftest(void)
{
	uint8_t key[VAULT_KEY_LEN];
	uint8_t tag[VAULT_TAG_LEN];
	uint8_t iv[VAULT_IV_LEN] = {0};
	uint8_t in[16] = {0};
	uint8_t out[16];
	uint8_t tmp[VAULT_TAG_LEN];
	static uint8_t plain[SELFTEST_LEN];
	static uint8_t cipher[SELFTEST_LEN];
	static uint8_t back[SELFTEST_LEN];
	uint8_t rnd_a[16];
	uint8_t rnd_b[16];
	size_t i;

	/* 1: the key derivation, with the parameters production uses. */
	if (vault_kdf(kat_secret, sizeof(kat_secret), kat_salt, key) != 0 ||
	    memcmp(key, kat_key, sizeof(key)) != 0)
		return report(1);

	/* 2: GCM over no data at all. An empty vault still has to produce a
	 * tag, and this is the one vector whose tag can be derived by hand. */
	if (vault_aead_encrypt(zero_key, zero_iv, NULL, 0, NULL, 0, NULL, tag) != 0 ||
	    memcmp(tag, kat_gcm_tag, VAULT_TAG_LEN) != 0)
		return report(2);

	/* 3: and over exactly one block, which is where a wrong output pointer
	 * or a skipped update shows up. */
	if (vault_aead_encrypt(zero_key, zero_iv, NULL, 0, in, sizeof(in), out,
	                       tag) != 0 ||
	    memcmp(out, kat_gcm_ct, sizeof(out)) != 0)
		return report(3);

	/* 4: a full-size round trip: the whole plaintext going out and coming
	 * back, with the output buffer being a different one each way. This is
	 * also where the generator gets exercised. */
	for (i = 0; i < sizeof(plain); i++) plain[i] = (uint8_t)(i * 7 + 3);
	if (vault_random(iv, sizeof(iv)) != 0) return report(4);
	if (vault_aead_encrypt(kat_key, iv, "VSSHVLT1", 8, plain, sizeof(plain),
	                       cipher, tag) != 0)
		return report(4);
	if (vault_aead_decrypt(kat_key, iv, "VSSHVLT1", 8, cipher, sizeof(cipher),
	                       tag, back) != 0)
		return report(4);
	if (memcmp(plain, back, sizeof(plain)) != 0) return report(4);

	/* 5: the tag has to cover the data. A ciphertext with one bit changed
	 * must not authenticate -- if it does, GCM has been reduced to an
	 * unauthenticated cipher and the vault would read altered slots. */
	if (vault_aead_decrypt(kat_key, iv, "VSSHVLT1", 8, cipher,
	                       sizeof(cipher), tag, back) != 0)
		return report(5);
	cipher[0] ^= 1;
	if (vault_aead_decrypt(kat_key, iv, "VSSHVLT1", 8, cipher, sizeof(cipher),
	                       tag, back) == 0)
		return report(5);
	cipher[0] ^= 1;

	/* 6: and the tag itself. */
	memcpy(tmp, tag, sizeof(tmp));
	tmp[VAULT_TAG_LEN - 1] ^= 1;
	if (vault_aead_decrypt(kat_key, iv, "VSSHVLT1", 8, cipher, sizeof(cipher),
	                       tmp, back) == 0)
		return report(6);

	/* 7: and the nonce, or two saves could share one. */
	iv[0] ^= 1;
	if (vault_aead_decrypt(kat_key, iv, "VSSHVLT1", 8, cipher, sizeof(cipher),
	                       tag, back) == 0)
		return report(7);
	iv[0] ^= 1;

	/* 8: and the header, which is passed as additional data. Nothing else
	 * checks that the AAD actually reaches the cipher -- a call that drops
	 * it would still encrypt and decrypt consistently with itself. */
	if (vault_aead_decrypt(kat_key, iv, "VSSHVLT2", 8, cipher, sizeof(cipher),
	                       tag, back) == 0)
		return report(8);

	/* 9: two draws from the generator must not be the same draw. This is
	 * not a test of randomness -- nothing here could be -- only that the
	 * call is not returning something fixed, which would make every save
	 * reuse a nonce. */
	if (vault_random(rnd_a, sizeof(rnd_a)) != 0) return report(9);
	if (vault_random(rnd_b, sizeof(rnd_b)) != 0) return report(9);
	if (memcmp(rnd_a, rnd_b, sizeof(rnd_a)) == 0) return report(9);

	OPENSSL_cleanse(key, sizeof(key));
	OPENSSL_cleanse(plain, sizeof(plain));
	OPENSSL_cleanse(back, sizeof(back));
	OPENSSL_cleanse(rnd_a, sizeof(rnd_a));
	OPENSSL_cleanse(rnd_b, sizeof(rnd_b));
	return 0;
}
