/***************************************************************************
 * Copyright (C) 2018 Amlogic, Inc. All rights reserved.                   *
 ***************************************************************************/

#include "swdemux_internal.h"
#include "aes.h"

/**AES Key.*/
struct swdmx_aes_key {
    AES_KEY key;
};

/**AES IV data.*/
struct swdmx_aes_iv {
    u8 iv[16];
};

static void
aes_cbc_dec_desc_pkt (AES_KEY *key, u8 *iv, struct swdmx_tspacket *pkt)
{
    u8 *p    = pkt->payload;
    int left = pkt->payload_len, len;
    int tail;
    u8  obuf[184];
    u8  ivbuf[16];
    u8 *in;
    u8 *out  = obuf;

    tail = left & 15;
    left -= tail;

    memcpy(ivbuf, iv, 16);

    in  = p;
    len = left;

    AES_cbc_encrypt(in, out, len, key, ivbuf, 0);

    memcpy(in, out, len);
}

int
swdmx_aes_cbc_dec_desc_cb (struct swdmx_descchannel *desc_chan, struct swdmx_tspacket *pkt)
{
    struct swdmx_key *key = NULL, *iv = NULL;
    struct swdmx_aes_key *akey;
    struct swdmx_aes_iv *aiv;

    if (pkt->scramble == 2) {
        key = desc_chan->even_key;
        iv  = desc_chan->even_iv;
    } else if (pkt->scramble == 3) {
        key = desc_chan->odd_key;
        iv  = desc_chan->odd_iv;
    } else if (pkt->scramble == 0) {
        key = desc_chan->zero_key;
        iv  = desc_chan->zero_iv;
    } else {
        swdmx_log("illegal scramble control field");
        return -1;
    }

    if (!key || !iv || !key->used || !iv->used || (key->algo != SWDMX_DESC_ALGO_AES_DEC) || !iv->is_iv)
        return -1;

    akey = key->data;
    aiv  = iv->data;

    aes_cbc_dec_desc_pkt(&akey->key, aiv->iv, pkt);

    return 0;
}

static void
aes_iv_set (struct swdmx_key *key, const u8 *v, int len)
{
    struct swdmx_aes_iv *iv = key->data;

    memcpy(iv->iv, v, 16);
}

static void
aes_iv_free (struct swdmx_key *key)
{
    struct swdmx_aes_iv *iv = key->data;

    swdmx_free(iv);
}

static void
aes_dec_key_set (struct swdmx_key *key, const u8 *v, int len)
{
    struct swdmx_aes_key *akey = key->data;

    AES_set_decrypt_key(v, 128, &akey->key);
}

static void
aes_key_free (struct swdmx_key *key)
{
    struct swdmx_aes_key *akey = key->data;

    swdmx_free(akey);
}

int
swdmx_aes_dec_key_init(struct swdmx_key *key)
{
    swdmx_key_clear(key);

    key->used = SWDMX_TRUE;
    key->algo = SWDMX_DESC_ALGO_AES_DEC;

    if (key->is_iv) {
        struct swdmx_aes_iv *iv;

        iv = swdmx_malloc(sizeof(struct swdmx_aes_iv));
        SWDMX_ASSERT(iv);

        key->data = iv;
        key->set  = aes_iv_set;
        key->free = aes_iv_free;
    } else {
        struct swdmx_aes_key *akey;

        akey = swdmx_malloc(sizeof(struct swdmx_aes_key));
        SWDMX_ASSERT(akey);

        key->data = akey;
        key->set  = aes_dec_key_set;
        key->free = aes_key_free;
    }

    return SWDMX_OK;
}
