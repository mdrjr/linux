// SPDX-License-Identifier: GPL-2.0-only
/*
 * aes-ce-glue.c - wrapper code for ARMv8 AES
 *
 * Copyright (C) 2015 Linaro Ltd <ard.biesheuvel@linaro.org>
 */

#include <asm/hwcap.h>
#include <asm/neon.h>
#include <asm/simd.h>
#include <asm/unaligned.h>
#include "arm_aes.h"

/* defined in aes-ce-core.S */
asmlinkage u32 ce_aes_sub(u32 input);
asmlinkage void ce_aes_invert(void *dst, void *src);
asmlinkage void ce_aes_cbc_decrypt(u8 out[], u8 const in[], u32 const rk[],
				   int rounds, int blocks, u8 iv[]);

struct aes_block {
	u8 b[AES_BLOCK_SIZE];
};

static int num_rounds(struct arm_aes_key *key)
{
	/*
	 * # of rounds specified by AES:
	 * 128 bit key		10 rounds
	 * 192 bit key		12 rounds
	 * 256 bit key		14 rounds
	 * => n byte key	=> 6 + (n/4) rounds
	 */
	return 6 + key->key_length / 4;
}

static int ce_aes_expandkey(struct arm_aes_key *key, const u8 *in_key,
			    unsigned int key_len)
{
	/*
	 * The AES key schedule round constants
	 */
	static u8 const rcon[] = {
		0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80, 0x1b, 0x36,
	};

	u32 kwords = key_len / sizeof(u32);
	struct aes_block *key_enc, *key_dec;
	int i, j;

	if (key_len != AES_KEYSIZE_128 &&
	    key_len != AES_KEYSIZE_192 &&
	    key_len != AES_KEYSIZE_256)
		return -EINVAL;

	key->key_length = key_len;
	for (i = 0; i < kwords; i++)
		key->key_enc[i] = get_unaligned_le32(in_key + i * sizeof(u32));

	kernel_neon_begin();
	for (i = 0; i < sizeof(rcon); i++) {
		u32 *rki = key->key_enc + (i * kwords);
		u32 *rko = rki + kwords;

		rko[0] = ror32(ce_aes_sub(rki[kwords - 1]), 8);
		rko[0] = rko[0] ^ rki[0] ^ rcon[i];
		rko[1] = rko[0] ^ rki[1];
		rko[2] = rko[1] ^ rki[2];
		rko[3] = rko[2] ^ rki[3];

		if (key_len == AES_KEYSIZE_192) {
			if (i >= 7)
				break;
			rko[4] = rko[3] ^ rki[4];
			rko[5] = rko[4] ^ rki[5];
		} else if (key_len == AES_KEYSIZE_256) {
			if (i >= 6)
				break;
			rko[4] = ce_aes_sub(rko[3]) ^ rki[4];
			rko[5] = rko[4] ^ rki[5];
			rko[6] = rko[5] ^ rki[6];
			rko[7] = rko[6] ^ rki[7];
		}
	}

	/*
	 * Generate the decryption keys for the Equivalent Inverse Cipher.
	 * This involves reversing the order of the round keys, and applying
	 * the Inverse Mix Columns transformation on all but the first and
	 * the last one.
	 */
	key_enc = (struct aes_block *)key->key_enc;
	key_dec = (struct aes_block *)key->key_dec;
	j = num_rounds(key);

	key_dec[0] = key_enc[j];
	for (i = 1, j--; j > 0; i++, j--)
		ce_aes_invert(key_dec + i, key_enc + j);
	key_dec[i] = key_enc[0];

	kernel_neon_end();
	return 0;
}

int arm_aes_setkey(struct arm_aes_key *key, const u8 *in_key, unsigned int key_len)
{
	return ce_aes_expandkey(key, in_key, key_len);
}

void arm_aes_cbc_decrypt(u8 const in[], u8 out[], int len,
					struct arm_aes_key *key, u8 iv[])
{
	unsigned int blocks;

	blocks = (len / AES_BLOCK_SIZE);
	kernel_neon_begin();
	ce_aes_cbc_decrypt(out, in,
			key->key_dec, num_rounds(key), blocks,
			iv);
	kernel_neon_end();
}
