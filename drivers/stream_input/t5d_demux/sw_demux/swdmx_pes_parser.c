/***************************************************************************
 * Copyright (C) 2018 Amlogic, Inc. All rights reserved.                   *
 ***************************************************************************/

#include "swdemux_internal.h"

#define PES_PRIVATE_STREAM_ID 0xfc

/*Parse the PES packet.*/
static int
pes_pkt_parse (struct swdmx_pespacket *pkt)
{
    u8  *p    = pkt->data;
    int  left = pkt->data_len;

    if (left < 6)
        return SWDMX_ERR;

    if ((p[0] != 0) || (p[1] != 0) || (p[2] != 1))
        return SWDMX_ERR;

    pkt->stream_id = p[3];
    //int PES_packet_length = p[4] << 8 | p[5];
    p    += 6;
    left -= 6;

    if ((pkt->stream_id != 0xbc)
            && (pkt->stream_id != 0xbf)
            && (pkt->stream_id != 0xf0)
            && (pkt->stream_id != 0xf1)
            && (pkt->stream_id != 0xff)
            && (pkt->stream_id != 0xf2)
            && (pkt->stream_id != 0xf8)
            && (pkt->stream_id != 0xbe)
            && (pkt->stream_id != PES_PRIVATE_STREAM_ID)) {
        u8     flags, hlen;
        u8    *h;
        int    hleft;

        if (left < 3)
            return SWDMX_ERR;

        pkt->scramble = (p[0] >> 4) & 3;
        flags         = p[1];
        hlen          = p[2];

        p    += 3;
        left -= 3;

        h     = p;
        hleft = hlen;

        if (flags & 0x80) {
            if (hleft < 5)
                return SWDMX_ERR;

            pkt->has_pts = SWDMX_TRUE;
            pkt->pts = (((u64)(h[0] & 0x0e)) << 29)
                | (((u64)h[1]) << 22)
                | (((u64)(h[2] & 0xfe)) << 14)
                | (((u64)h[3]) << 7)
                | (((u64)(h[4] & 0xfe)) >> 1);

            h     += 5;
            hleft -= 5;
        }

        p    += hlen;
        left -= hlen;

        pkt->payload     = p;
        pkt->payload_len = left;
        return SWDMX_OK;
    } else if (pkt->stream_id == PES_PRIVATE_STREAM_ID) {
        /*
        parse secure buf info from pes payload
        pts    4 byte
        index  1 byte
        size   4 byte
        addr   4 byte
        creat pes packet as following

        packet_start_code_prefix 3 byte    00 00 01
        stream_id                1 byte    0xfc
        PES_packet_length        2 byte    00 0d (13 byte len)
        PTS                      4 byte
        index                    1 byte
        size                     4 byte
        addr                     4 byte
        */

        u8     hlen = left;
        u8    *h;
        int    hleft;

        if (left < 3)
            return SWDMX_ERR;

        h     = p;
        hleft = hlen;

        pkt->has_pts = SWDMX_TRUE;
        pkt->pts = (((u64)(h[0])) << 24)
            | (((u64)h[1]) << 16)
            | (((u64)(h[2])) << 8)
            | (((u64)h[3]) << 7);

        h     += 4;
        hleft -= 4;

        p    += hlen;
        left -= hlen;

        pkt->payload     = p;
        pkt->payload_len = left;
        return SWDMX_OK;
    } else {
        return SWDMX_ERR;
    }
}

/*Output the current PES packet.*/
static void
pes_pkt_output (struct swdmx_pesparser *pp)
{
    struct swdmx_cb_entry *ce, *nce;
    struct swdmx_pespacket *pkt = &pp->pkt;
    int r;

    pkt->valid   = SWDMX_FALSE;
    pkt->has_pts = SWDMX_FALSE;

    r = pes_pkt_parse(&pp->pkt);

    if (r == SWDMX_OK)
        pkt->valid = SWDMX_TRUE;
    else
        pkt->scramble = pp->ts_scramble;

    SWDMX_LIST_FOR_EACH_SAFE(ce, nce, &pp->cb_list, ln) {
        swdmx_pespacket_cb cb = ce->cb;

        cb(&pp->pkt, ce->data);
    }

    pp->start = SWDMX_FALSE;
}

struct swdmx_pesparser*
swdmx_pes_parser_new (void)
{
    struct swdmx_pesparser *pp;

    pp = swdmx_malloc(sizeof(struct swdmx_pesparser));
    SWDMX_ASSERT(pp);

    pp->start       = SWDMX_FALSE;
    pp->ts_scramble = SWDMX_FALSE;
    swdmx_list_init(&pp->cb_list);

    pp->pkt.data     = NULL;
    pp->pkt.data_cap = 0;
    pp->pkt.data_len = 0;

    return pp;
}

void
swdmx_pes_parser_free (struct swdmx_pesparser *parser)
{
    SWDMX_ASSERT(parser);

    if (parser->pkt.data)
        swdmx_free(parser->pkt.data);

    swdmx_cb_list_clear(&parser->cb_list);

    swdmx_free(parser);
}

void
swdmx_pes_parser_add_pes_packet_cb (
            struct swdmx_pesparser *parser,
            swdmx_pespacket_cb cb,
            void *data)
{
    SWDMX_ASSERT(parser);

    swdmx_cb_list_add(&parser->cb_list, cb, data);
}

void
swdmx_pes_parser_remove_pes_packet_cb (
            struct swdmx_pesparser *parser,
            swdmx_pespacket_cb cb,
            void *data)
{
    SWDMX_ASSERT(parser);

    swdmx_cb_list_remove(&parser->cb_list, cb, data);
}

static int
swdmx_pes_packet_resize (struct swdmx_pespacket *pkt, int len)
{
    u8 *nptr;

    len = max(len, 32 * 1024);
    len = max(len, pkt->data_cap * 2);

    nptr = swdmx_realloc(pkt->data, len);
    if (!nptr)
        return SWDMX_ERR;

    pkt->data = nptr;
    pkt->data_cap = len;

    return SWDMX_OK;
}

void
swdmx_pes_parser_ts_packet_cb (
            struct swdmx_tspacket *ts,
            void *data)
{
    struct swdmx_pesparser *pp = data;
    struct swdmx_pespacket *pkt = &pp->pkt;
    int len;

    SWDMX_ASSERT(ts && pp);

    if (ts->payload_start) {
        if (pp->start)
            pes_pkt_output(pp);

        pp->start       = SWDMX_TRUE;
        pp->ts_scramble = ts->scramble;
        pkt->data_len = 0;
    }

    if (!pp->start)
        return;

    len = pkt->data_len + ts->payload_len;

    if (pkt->data_cap < len) {
        if (swdmx_pes_packet_resize(pkt, len) == SWDMX_ERR)
            return;
    }

    memcpy(pkt->data + pkt->data_len, ts->payload, ts->payload_len);

    pkt->data_len = len;
}
