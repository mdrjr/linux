/* SPDX-License-Identifier: GPL-2.0 */

#ifndef ASM_ARM_AES_H
#define ASM_ARM_AES_H
#define AES_MIN_KEY_SIZE	16
#define AES_MAX_KEY_SIZE	32
#define AES_KEYSIZE_128		16
#define AES_KEYSIZE_192		24
#define AES_KEYSIZE_256		32
#define AES_BLOCK_SIZE		16
#define AES_MAX_KEYLENGTH	(15 * 16)
#define AES_MAX_KEYLENGTH_U32	(AES_MAX_KEYLENGTH / sizeof(u32))

struct arm_aes_key {
	u32 key_enc[AES_MAX_KEYLENGTH_U32];
	u32 key_dec[AES_MAX_KEYLENGTH_U32];
	u32 key_length;
};

int arm_aes_setkey(struct arm_aes_key *key, const u8 *in_key, unsigned int key_len);

void arm_aes_cbc_decrypt(u8 const in[], u8 out[], int len, struct arm_aes_key *key, u8 iv[]);

#endif
