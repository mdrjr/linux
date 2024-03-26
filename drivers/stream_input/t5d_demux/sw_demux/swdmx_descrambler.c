/***************************************************************************
 * Copyright (C) 2018 Amlogic, Inc. All rights reserved.                   *
 ***************************************************************************/

#include "swdemux_internal.h"

struct swdmx_descrambler*
swdmx_descrambler_new (void)
{
    struct swdmx_descrambler *desc;

    desc = swdmx_malloc(sizeof(struct swdmx_descrambler));
    SWDMX_ASSERT(desc);

    swdmx_list_init(&desc->chan_list);
    swdmx_list_init(&desc->cb_list);

    return desc;
}

struct swdmx_descchannel*
swdmx_descrambler_alloc_channel (struct swdmx_descrambler *desc)
{
    struct swdmx_descchannel *chan;

    SWDMX_ASSERT(desc);

    chan = swdmx_malloc(sizeof(struct swdmx_descchannel));
    SWDMX_ASSERT(chan);

    chan->pid    = 0xffff;
    chan->enable = SWDMX_FALSE;
    chan->scb_as_is = SWDMX_FALSE;
    chan->scb      = 0;
    chan->odd_key  = NULL;
    chan->even_key = NULL;
    chan->zero_key = NULL;
    chan->odd_iv   = NULL;
    chan->even_iv  = NULL;
    chan->zero_iv  = NULL;
    chan->desc_cb  = NULL;

    swdmx_list_append(&desc->chan_list, &chan->ln);

    return chan;
}

void
swdmx_descrambler_ts_packet_cb (
            struct swdmx_tspacket *pkt,
            void *data)
{
    struct swdmx_descrambler *desc = (struct swdmx_descrambler*)data;
    struct swdmx_descchannel *ch, *nch;
    struct swdmx_cb_entry    *ce, *nce;

    SWDMX_ASSERT(pkt && desc);

    if (pkt->payload) {
        SWDMX_LIST_FOR_EACH_SAFE(ch, nch, &desc->chan_list, ln) {
            if ((ch->enable) && (ch->pid == pkt->pid) && ch->desc_cb) {
                int r;

                r = ch->desc_cb(ch, pkt);
                if ((r == 0) && !ch->scb_as_is) {
                    pkt->scramble   = ch->scb;
                    pkt->packet[3] &= 0x3f;
                    pkt->packet[3] |= (ch->scb << 6);
                }

                break;
            }
        }
    }

    SWDMX_LIST_FOR_EACH_SAFE(ce, nce, &desc->cb_list, ln) {
        swdmx_tspacket_cb cb = ce->cb;

        cb(pkt, ce->data);
    }
}

int
swdmx_descrambler_add_ts_packet_cb (
            struct swdmx_descrambler *desc,
            swdmx_tspacket_cb  cb,
            void          *data)
{
    SWDMX_ASSERT(desc && cb);

    swdmx_cb_list_add(&desc->cb_list, cb, data);

    return SWDMX_OK;
}

int
swdmx_descrambler_remove_ts_packet_cb (
            struct swdmx_descrambler *desc,
            swdmx_tspacket_cb   cb,
            void          *data)
{
    SWDMX_ASSERT(desc && cb);

    swdmx_cb_list_remove(&desc->cb_list, cb, data);

    return SWDMX_OK;
}

void
swdmx_descrambler_free (struct swdmx_descrambler *desc)
{
    SWDMX_ASSERT(desc);

    while (!swdmx_list_is_empty(&desc->chan_list)) {
        struct swdmx_descchannel *chan;

        chan = SWDMX_CONTAINEROF(desc->chan_list.next,
                    struct swdmx_descchannel, ln);

        swdmx_desc_channel_free(chan);
    }

    swdmx_cb_list_clear(&desc->cb_list);

    swdmx_free(desc);
}

int
swdmx_desc_channel_set_cb (
            struct swdmx_descchannel *chan,
            swdmx_desc_cb cb)
{
    SWDMX_ASSERT(chan);

    chan->desc_cb = cb;

    return SWDMX_OK;
}

int
swdmx_desc_channel_set_pid (
            struct swdmx_descchannel *chan,
            u16       pid)
{
    SWDMX_ASSERT(chan);

    if (!swdmx_is_valid_pid(pid) || (pid == 0x1fff)) {
        swdmx_log("illegal PID 0x%04x", pid);
        return SWDMX_ERR;
    }

    chan->pid = pid;

    return SWDMX_OK;
}

int
swdmx_desc_channel_set_key (
            struct swdmx_descchannel *chan,
            enum swdmx_keyparity parity,
            struct swdmx_key *key)
{
    SWDMX_ASSERT(chan && key);

    switch (parity) {
    case SWDMX_DESC_ODD_KEY:
        chan->odd_key = key;
        break;
	case SWDMX_DESC_EVEN_KEY:
        chan->even_key = key;
        break;
	case SWDMX_DESC_00_KEY:
        chan->zero_key = key;
        break;
	case SWDMX_DESC_ODD_IV:
        chan->odd_iv = key;
        break;
	case SWDMX_DESC_EVEN_IV:
        chan->even_iv = key;
        break;
	case SWDMX_DESC_00_IV:
        chan->zero_iv = key;
        break;
    }

    return SWDMX_OK;
}

int
swdmx_desc_channel_set_scb (
            struct swdmx_descchannel *chan,
            int as_is,
			int scb)
{
    SWDMX_ASSERT(chan);

    chan->scb_as_is = as_is;
    chan->scb = scb;

    return SWDMX_OK;
}

int
swdmx_desc_channel_enable (struct swdmx_descchannel *chan)
{
    SWDMX_ASSERT(chan);

    if (!swdmx_is_valid_pid(chan->pid)) {
        swdmx_log("descrambler channel's PID has not been set");
        return SWDMX_ERR;
    }

    chan->enable = SWDMX_TRUE;

    return SWDMX_OK;
}

int
swdmx_desc_channel_disable (struct swdmx_descchannel *chan)
{
    SWDMX_ASSERT(chan);

    chan->enable = SWDMX_FALSE;

    return SWDMX_OK;
}

void
swdmx_desc_channel_free (struct swdmx_descchannel *chan)
{
    SWDMX_ASSERT(chan);

    swdmx_list_remove(&chan->ln);

    swdmx_free(chan);
}

/**
 * Clear the key entry.
 * \param key The key entry.
 * \retval SWDMX_OK On success.
 * \retval SWDMX_ERR On error.
 */
int
swdmx_key_clear (struct swdmx_key *key)
{
    SWDMX_ASSERT(key);

    if (key->used) {
        if (key->free)
            key->free(key);

        key->used = SWDMX_FALSE;
        key->algo = SWDMX_DESC_ALGO_NONE;
    }

    return SWDMX_OK;
}

/**
 * Set the key value.
 * \param key The key entry.
 * \param v The key value.
 * \param len The key's length.
 * \retval SWDMX_OK On success.
 * \retval SWDMX_ERR On error.
 */
int
swdmx_key_set (struct swdmx_key *key, const u8 *v, int len)
{
    SWDMX_ASSERT(key && v);

    if (!key->used) {
        swdmx_log("the key is not used.");
        return SWDMX_ERR;
    }

    key->set(key, v, len);

    return SWDMX_OK;
}
