/*
 * Copyright (C) 2017 Amlogic, Inc. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 */

#include "t5d_hw_dsc.h"
#include "t5d_demux.h"

#define print_err(fmt, args...)   \
	dprintk(LOG_ERROR, debug_demux, fmt, ## args)
#define print_dbg(fmt, args...)   \
	dprintk(LOG_DBG, debug_demux, fmt, ## args)

MODULE_PARM_DESC(debug_demux, "\n\t\t Enable demux debug information");
static int debug_demux;
module_param(debug_demux, int, 0644);

/*The DVB device.*/
static struct t5d_dvb dvbdev;

/*Initialize a stream.*/
static void
stream_init(struct t5d_stream *s, enum t5d_stream_type type, enum t5d_demux_source source)
{
	s->type   = type;
	s->source = source;
	s->sw_ts_parser = swdmx_ts_parser_new();
	s->sw_desc      = swdmx_descrambler_new();
	s->sw_demux     = swdmx_demux_new();

	swdmx_ts_parser_add_ts_packet_cb(s->sw_ts_parser, swdmx_descrambler_ts_packet_cb, s->sw_desc);
	swdmx_descrambler_add_ts_packet_cb(s->sw_desc, swdmx_demux_ts_packet_cb, s->sw_demux);

	INIT_LIST_HEAD(&s->demuxes);
}

/*Release a stream.*/
static void
stream_deinit(struct t5d_stream *s)
{
	swdmx_ts_parser_free(s->sw_ts_parser);
	swdmx_descrambler_free(s->sw_desc);
	swdmx_demux_free(s->sw_demux);

	if (s->ts_buffer)
		swdmx_free(s->ts_buffer);
}

/*Get the stream from the source type.*/
static struct t5d_stream *
stream_get(enum t5d_demux_source source)
{
	struct t5d_stream *s = NULL;
	int id;

	if (source >= T5D_FRONTEND_TS0) {
		id = source - T5D_FRONTEND_TS0;

		if (id >= T5D_HW_STREAM_NUM)
			return s;

		s = &dvbdev.hw_streams[id];
	} else {
		id = source - T5D_DMA_0;

		if (id >= T5D_DMA_STREAM_NUM)
			return s;

		s = &dvbdev.dma_streams[id];
	}

	return s;
}

/*Clear the resource in the filter.*/
static void
filter_clear(struct t5d_filter *filter)
{
	if (filter->sw_sec_filter) {
		swdmx_sec_filter_free(filter->sw_sec_filter);
		filter->sw_sec_filter = NULL;
	}

	if (filter->sw_ts_filter) {
		swdmx_ts_filter_free(filter->sw_ts_filter);
		filter->sw_ts_filter = NULL;
	}

	if (filter->sw_pes_parser) {
		swdmx_pes_parser_free(filter->sw_pes_parser);
		filter->sw_pes_parser = NULL;
	}
}

/*PCR data callback.*/
static void
pcr_callback(struct swdmx_tspacket *pkt, void *udata)
{
	struct t5d_filter *filter = udata;
	u8  *p;
	u64  pcr;

	if (pkt->adp_field_len < 6)
		return;

	p = pkt->adp_field;
	if (!(p[0] & 0x10))
		return;

	pcr = (((u64)(p[1])) << 25)
	      | (((u64)p[2]) << 17)
	      | (((u64)(p[3])) << 9)
	      | (((u64)p[4]) << 1)
	      | ((((u64)p[5]) & 0x80) >> 7);

	if (filter->cb)
		filter->cb(filter->dmx_id, filter->id, (u8 *)&pcr, sizeof(pcr), filter->user_data);
}

/*DVR TS callback.*/
static void
dvr_callback(struct swdmx_tspacket *pkt, void *udata)
{
	struct t5d_filter *filter = udata;

	if (filter->cb)
		filter->cb(filter->dmx_id, filter->id, pkt->packet, pkt->packet_len, filter->user_data);
}

/*ES data callback.*/
static void
es_callback(struct swdmx_pespacket *pkt, void *udata)
{
	struct t5d_filter *filter = udata;

	if (filter->cb) {
		struct t5d_es_data es;

		es.valid     = pkt->valid;
		es.scrambled = pkt->scramble;
		es.has_pts   = pkt->has_pts;
		es.pts       = pkt->pts;
		es.len       = pkt->valid ? pkt->payload_len : 0;
		es.data      = pkt->valid ? pkt->payload : NULL;

		filter->cb(filter->dmx_id, filter->id, (u8 *)&es, sizeof(es), filter->user_data);
	}
}

/*PES data callback.*/
static void
pes_callback(struct swdmx_pespacket *pkt, void *udata)
{
	struct t5d_filter *filter = udata;

	if (filter->cb)
		filter->cb(filter->dmx_id, filter->id, pkt->data, pkt->data_len, filter->user_data);
}

/*Section callback.*/
static void
sec_callback(u8 *data, int len, void *udata)
{
	struct t5d_filter *filter = udata;

	if (filter->cb)
		filter->cb(filter->dmx_id, filter->id, data, len, filter->user_data);
}

/*Set the PES filter.*/
static void
pes_filter_set(struct t5d_filter *filter, struct t5d_stream *s)
{
	filter->sw_ts_filter = swdmx_demux_alloc_ts_filter(s->sw_demux);

	if (filter->pes_params.pid != 0x1fff) {
		struct swdmx_tsfilter_params p;
		enum dmx_ts_pes pes_type;
		enum dmx_output output;

		memset(&p, 0, sizeof(p));

		p.pid = filter->pes_params.pid;

		swdmx_ts_filter_set_params(filter->sw_ts_filter, &p);

		output   = filter->pes_params.output;
		pes_type = filter->pes_params.pes_type;

		if ((pes_type == DMX_PES_PCR0)
		    || (pes_type == DMX_PES_PCR1)
		    || (pes_type == DMX_PES_PCR2)
		    || (pes_type == DMX_PES_PCR3)) {
			swdmx_ts_filter_add_ts_packet_cb(filter->sw_ts_filter, pcr_callback, filter);
		} else if (output == DMX_OUT_TS_TAP) {
			swdmx_ts_filter_add_ts_packet_cb(filter->sw_ts_filter, dvr_callback, filter);
		} else if (((pes_type == DMX_PES_VIDEO0)
			    || (pes_type == DMX_PES_VIDEO1)
			    || (pes_type == DMX_PES_VIDEO2)
			    || (pes_type == DMX_PES_VIDEO3)
			    || (pes_type == DMX_PES_AUDIO0)
			    || (pes_type == DMX_PES_AUDIO1)
			    || (pes_type == DMX_PES_AUDIO2)
			    || (pes_type == DMX_PES_AUDIO3))
			   && (filter->pes_params.flags & DMX_ES_OUTPUT)) {
			filter->sw_pes_parser = swdmx_pes_parser_new();
			swdmx_ts_filter_add_ts_packet_cb(filter->sw_ts_filter, swdmx_pes_parser_ts_packet_cb, filter->sw_pes_parser);
			swdmx_pes_parser_add_pes_packet_cb(filter->sw_pes_parser, es_callback, filter);
		} else {
			filter->sw_pes_parser = swdmx_pes_parser_new();
			swdmx_ts_filter_add_ts_packet_cb(filter->sw_ts_filter, swdmx_pes_parser_ts_packet_cb, filter->sw_pes_parser);
			swdmx_pes_parser_add_pes_packet_cb(filter->sw_pes_parser, pes_callback, filter);
		}
	}

	if (filter->started)
		swdmx_ts_filter_enable(filter->sw_ts_filter);
}

/*Set section filter.*/
static void
sec_filter_set(struct t5d_filter *filter, struct t5d_stream *s)
{
	filter->sw_sec_filter = swdmx_demux_alloc_sec_filter(s->sw_demux);

	if (filter->sec_params.pid != 0x1fff) {
		struct swdmx_secfilter_params p;

		memset(&p, 0, sizeof(p));

		p.pid = filter->sec_params.pid;

		memcpy(p.value, filter->sec_params.filter.filter, DMX_FILTER_SIZE);
		memcpy(p.mask, filter->sec_params.filter.mask, DMX_FILTER_SIZE);
		memcpy(p.mode, filter->sec_params.filter.mode, DMX_FILTER_SIZE);

		if (filter->sec_params.flags & DMX_CHECK_CRC)
			p.crc32 = true;

		swdmx_sec_filter_set_params(filter->sw_sec_filter, &p);

		swdmx_sec_filter_add_section_cb(filter->sw_sec_filter, sec_callback, filter);
	}

	if (filter->started)
		swdmx_sec_filter_enable(filter->sw_sec_filter);
}

/*Clear the descrambler.*/
static void
desc_clear(struct t5d_descrambler *desc)
{
	if (desc->algo == CA_ALGO_CSA2 || desc->algo == CA_ALGO_CSA3) {
		t5d_hwdmx_desc_channel_free(desc->hw_desc_chan);
		desc->hw_desc_chan = NULL;
		return;
	} else if (desc->sw_desc_chan) {
		swdmx_desc_channel_free(desc->sw_desc_chan);
		desc->sw_desc_chan = NULL;
	}
}

/*Set the descrambler's key.*/
static void
desc_key_set(struct t5d_descrambler *desc, enum ca_sc2_key_type type)
{
	struct t5d_key *key = NULL;
	enum swdmx_keyparity parity;
	int key_id = -1;

	switch (type) {
	case CA_KEY_EVEN_TYPE:
		parity = SWDMX_DESC_EVEN_KEY;
		key_id = desc->even_key_id;
		break;
	case CA_KEY_EVEN_IV_TYPE:
		parity = SWDMX_DESC_EVEN_IV;
		key_id = desc->even_iv_id;
		break;
	case CA_KEY_ODD_TYPE:
		parity = SWDMX_DESC_ODD_KEY;
		key_id = desc->odd_key_id;
		break;
	case CA_KEY_ODD_IV_TYPE:
		parity = SWDMX_DESC_ODD_IV;
		key_id = desc->odd_iv_id;
		break;
	case CA_KEY_00_TYPE:
		parity = SWDMX_DESC_00_KEY;
		key_id = desc->zero_key_id;
		break;
	case CA_KEY_00_IV_TYPE:
		parity = SWDMX_DESC_00_IV;
		key_id = desc->zero_iv_id;
		break;
	}

	if (key_id != -1) {
		key = &dvbdev.keys[key_id];
		if (desc->sw_desc_chan) {
			swdmx_desc_channel_set_key(desc->sw_desc_chan, parity, &key->sw_key);
		} else if (desc->hw_desc_chan &&
			   (desc->algo == CA_ALGO_CSA2 || desc->algo == CA_ALGO_CSA3)) {
			t5d_hwdmx_desc_channel_set_key(desc->hw_desc_chan, (enum t5d_hwdmx_keyparity)parity, &key->hw_key);
		}
	}
}

/*Set the descrambler.*/
static void
desc_set(struct t5d_descrambler *desc, struct t5d_stream *s)
{
	if (desc->algo == CA_ALGO_CSA2 || desc->algo == CA_ALGO_CSA3) {
		desc->hw_desc_chan = t5d_hwdmx_desc_alloc_channel(s->source);
		if (desc->pid != 0x1fff)
			t5d_hwdmx_desc_channel_set_pid(desc->hw_desc_chan, desc->pid);
	} else {
		desc->sw_desc_chan = swdmx_descrambler_alloc_channel(s->sw_desc);
		if (desc->pid != 0x1fff)
			swdmx_desc_channel_set_pid(desc->sw_desc_chan, desc->pid);

		if (desc->algo == CA_ALGO_AES_CBC_CLR_END)
			swdmx_desc_channel_set_cb(desc->sw_desc_chan, swdmx_aes_cbc_dec_desc_cb);

		swdmx_desc_channel_set_scb(desc->sw_desc_chan, desc->scb_as_is, desc->scb_value);
	}

	if (desc->even_key_id != -1)
		desc_key_set(desc, CA_KEY_EVEN_TYPE);
	if (desc->even_iv_id != -1)
		desc_key_set(desc, CA_KEY_EVEN_IV_TYPE);
	if (desc->odd_key_id != -1)
		desc_key_set(desc, CA_KEY_ODD_TYPE);
	if (desc->odd_iv_id != -1)
		desc_key_set(desc, CA_KEY_ODD_IV_TYPE);
	if (desc->zero_key_id != -1)
		desc_key_set(desc, CA_KEY_00_TYPE);
	if (desc->zero_iv_id != -1)
		desc_key_set(desc, CA_KEY_00_IV_TYPE);

	if (desc->algo == CA_ALGO_CSA2 || desc->algo == CA_ALGO_CSA3)
		t5d_hwdmx_desc_channel_enable(desc->hw_desc_chan);
	else
		swdmx_desc_channel_enable(desc->sw_desc_chan);
}

/**
 * Initialize the DVB device.
 * @retval 0 On success.
 * @retval -1 On error.
 */
int
t5d_init_device(void)
{
	int i, j;

	memset(&dvbdev, 0, sizeof(dvbdev));

	mutex_init(&dvbdev.lock);
	for (i = 0; i < T5D_HW_STREAM_NUM; i ++) {
		struct t5d_stream *s = &dvbdev.hw_streams[i];

		stream_init(s, T5D_STREAM_HW, T5D_FRONTEND_TS0 + i);
	}

	for (i = 0; i < T5D_DMA_STREAM_NUM; i ++) {
		struct t5d_stream *s = &dvbdev.dma_streams[i];

		stream_init(s, T5D_STREAM_DMA, T5D_DMA_0 + i);
	}

	for (i = 0; i < T5D_DEMUX_PER_DEVICE; i ++) {
		struct t5d_demux *dmx = &dvbdev.demuxes[i];
		struct t5d_stream *s = &dvbdev.hw_streams[2];

		list_add(&dmx->lh, &s->demuxes);

		dmx->source = s->source;
		dmx->id = i;

		for (j = 0; j < T5D_FILTER_PER_DEMUX; j ++) {
			struct t5d_filter *filter = &dmx->filters[j];

			filter->dmx_id = dmx->id;
			filter->id     = j;
			filter->pes_params.pid = 0x1fff;
			filter->sec_params.pid = 0x1fff;
		}

		for (j = 0; j < T5D_DESC_PER_DEMUX; j ++) {
			struct t5d_descrambler *desc = &dmx->descs[j];

			desc->id = j;
		}
	}

	for (i = 0; i < T5D_KEY_PER_DEVICE; i ++) {
		struct t5d_key *key = &dvbdev.keys[i];

		key->id = i;
	}

	return 0;
}

/**
 * Release the DVB device.
 * @retval 0 On success.
 * @retval -1 On error.
 */
int
t5d_deinit_device(void)
{
	int i, j;

	for (i = 0; i < T5D_DEMUX_PER_DEVICE; i ++) {
		struct t5d_demux *dmx = &dvbdev.demuxes[i];

		for (j = 0; j < T5D_FILTER_PER_DEMUX; j ++) {
			struct t5d_filter *filter = &dmx->filters[j];

			filter_clear(filter);
		}

		for (j = 0; j < T5D_DESC_PER_DEMUX; j ++) {
			struct t5d_descrambler *desc = &dmx->descs[j];

			desc_clear(desc);
		}
	}

	for (i = 0; i < T5D_HW_STREAM_NUM; i ++) {
		struct t5d_stream *s = &dvbdev.hw_streams[i];

		stream_deinit(s);
	}

	for (i = 0; i < T5D_DMA_STREAM_NUM; i ++) {
		struct t5d_stream *s = &dvbdev.dma_streams[i];

		stream_deinit(s);
	}

	mutex_destroy(&dvbdev.lock);

	return 0;
}

/**
 * Set the demux device's input source.
 * @param dmx_id Demux device's index.
 * @param source The input source.
 * @retval 0 On success.
 * @retval -1 On error.
 */
int
t5d_set_demux_source(
	int dmx_id,
	enum t5d_demux_source source)
{
	struct t5d_demux *dmx;
	struct t5d_stream *s;
	int r, i;

	t5d_param_check((dmx_id >= 0) && (dmx_id < T5D_DEMUX_PER_DEVICE));

	dmx = &dvbdev.demuxes[dmx_id];

	if (dmx->source != source) {
		mutex_lock(&dvbdev.lock);

		/*Remove from the old stream.*/
		s = stream_get(dmx->source);

		list_del(&dmx->lh);

		for (i = 0; i < T5D_FILTER_PER_DEMUX; i ++) {
			struct t5d_filter *filter = &dmx->filters[i];

			if (filter->used)
				filter_clear(filter);
		}

		for (i = 0; i < T5D_DESC_PER_DEMUX; i ++) {
			struct t5d_descrambler *desc = &dmx->descs[i];

			if (desc->used)
				desc_clear(desc);
		}

		/*Add to the new stream.*/
		s = stream_get(source);

		list_add(&dmx->lh, &s->demuxes);

		for (i = 0; i < T5D_FILTER_PER_DEMUX; i ++) {
			struct t5d_filter *filter = &dmx->filters[i];

			if (filter->used) {
				if (filter->type == T5D_FILTER_PES)
					pes_filter_set(filter, s);
				else
					sec_filter_set(filter, s);
			}
		}

		for (i = 0; i < T5D_DESC_PER_DEMUX; i ++) {
			struct t5d_descrambler *desc = &dmx->descs[i];

			if (desc->used)
				desc_set(desc, s);
		}

		dmx->source = source;

		mutex_unlock(&dvbdev.lock);
	}

	r = 0;

	return r;
}

/**
 * Get the demux's source.
 * @param dmx_id Demux device's index.
 * @return The demux's source.
 */
enum t5d_demux_source
t5d_get_demux_source(int dmx_id) {
	struct t5d_demux *dmx;
	enum t5d_demux_source src;

	t5d_param_check((dmx_id >= 0) &&(dmx_id < T5D_DEMUX_PER_DEVICE));

	dmx = &dvbdev.demuxes[dmx_id];

	mutex_lock(&dvbdev.lock);
	src = dmx->source;
	mutex_unlock(&dvbdev.lock);

	return src;
}

/**
 * Get the demux's source without lock.
 * @param dmx_id Demux device's index.
 * @return The demux's source.
 */
enum t5d_demux_source
t5d_get_demux_source_unlock(int dmx_id) {
	struct t5d_demux *dmx;
	enum t5d_demux_source src;

	t5d_param_check((dmx_id >= 0) &&(dmx_id < T5D_DEMUX_PER_DEVICE));

	dmx = &dvbdev.demuxes[dmx_id];
	src = dmx->source;

	return src;
}

/**
 * Allocate a filter.
 * @param dmx_id Demux device's index.
 * @return The filter's index.
 * @retval -1 On error.
 */
int
t5d_alloc_filter(
	int dmx_id)
{
	struct t5d_demux *dmx;
	int i, fid = -1;

	t5d_param_check((dmx_id >= 0) && (dmx_id < T5D_DEMUX_PER_DEVICE));

	dmx = &dvbdev.demuxes[dmx_id];

	mutex_lock(&dvbdev.lock);

	for (i = 0; i < T5D_FILTER_PER_DEMUX; i ++) {
		struct t5d_filter *f = &dmx->filters[i];

		if (!f->used) {
			f->used = true;
			fid = i;

			break;
		} else
			print_dbg("%#x used: %d\n", f, f->used);
	}

	mutex_unlock(&dvbdev.lock);

	if (fid == -1)
		print_err("cannot allocate filter");

	return fid;
}

/**
 * Free a filter.
 * @param dmx_id Demux device's index.
 * @param filter_id The filter's index to be freed.
 * @return The filter's index.
 * @retval -1 On error.
 */
int
t5d_free_filter(
	int dmx_id,
	int filter_id)
{
	struct t5d_demux *dmx;
	struct t5d_filter *filter;
	int r;

	t5d_param_check((dmx_id >= 0) && (dmx_id < T5D_DEMUX_PER_DEVICE));
	t5d_param_check((filter_id >= 0) && (filter_id < T5D_FILTER_PER_DEMUX));

	dmx = &dvbdev.demuxes[dmx_id];
	filter = &dmx->filters[filter_id];

	mutex_lock(&dvbdev.lock);

	if (filter->used) {
		filter_clear(filter);

		filter->used    = false;
		filter->started = false;
		filter->cb      = NULL;
		filter->pes_params.pid = 0x1fff;
		filter->sec_params.pid = 0x1fff;

		r = 0;
	} else {
		print_dbg("filter %d is not used", filter_id);
		r = -1;
	}

	mutex_unlock(&dvbdev.lock);

	return r;
}

/**
 * Set the PES filter's parameters.
 * @param dmx_id Demux device's index.
 * @param filter_id The filter's index.
 * @param params The filter's parameters.
 * @retval 0 On success.
 * @retval -1 On error.
 */
int
t5d_set_pes_filter(
	int dmx_id,
	int filter_id,
	struct dmx_pes_filter_params *params)
{
	struct t5d_demux *dmx;
	struct t5d_filter *filter;
	int r;

	t5d_param_check((dmx_id >= 0) && (dmx_id < T5D_DEMUX_PER_DEVICE));
	t5d_param_check((filter_id >= 0) && (filter_id < T5D_FILTER_PER_DEMUX));

	dmx = &dvbdev.demuxes[dmx_id];
	filter = &dmx->filters[filter_id];

	mutex_lock(&dvbdev.lock);

	if (filter->used) {
		struct t5d_stream *s = stream_get(dmx->source);

		filter_clear(filter);

		filter->type = T5D_FILTER_PES;
		filter->pes_params = *params;

		pes_filter_set(filter, s);

		r = 0;
	} else {
		print_err("filter %d is not used", filter_id);
		r = -1;
	}

	mutex_unlock(&dvbdev.lock);

	return r;
}

/**
 * Set the section filter's parameters.
 * @param dmx_id Demux device's index.
 * @param filter_id The filter's index.
 * @param params The filter's parameters.
 * @retval 0 On success.
 * @retval -1 On error.
 */
int
t5d_set_sec_filter(
	int dmx_id,
	int filter_id,
	struct dmx_sct_filter_params *params)
{
	struct t5d_demux *dmx;
	struct t5d_filter *filter;
	int r;

	t5d_param_check((dmx_id >= 0) && (dmx_id < T5D_DEMUX_PER_DEVICE));
	t5d_param_check((filter_id >= 0) && (filter_id < T5D_FILTER_PER_DEMUX));

	dmx = &dvbdev.demuxes[dmx_id];
	filter = &dmx->filters[filter_id];

	mutex_lock(&dvbdev.lock);

	if (filter->used) {
		struct t5d_stream *s = stream_get(dmx->source);

		filter_clear(filter);

		filter->type = T5D_FILTER_SEC;
		filter->sec_params = *params;

		sec_filter_set(filter, s);

		r = 0;
	} else {
		print_err("filter %d is not used", filter_id);
		r = -1;
	}

	mutex_unlock(&dvbdev.lock);

	return r;
}

/**
 * Set the filter's callback functions.
 * @param dmx_id Demux device's index.
 * @param filter_id The filter's index.
 * @param cb The callback function.
 * @param user_data The user defined data used in the callback function.
 * @retval 0 On success.
 * @retval -1 On error.
 */
int
t5d_set_filter_callback(
	int dmx_id,
	int filter_id,
	t5d_filter_cb cb,
	void *user_data)
{
	struct t5d_demux *dmx;
	struct t5d_filter *filter;
	int r;

	t5d_param_check((dmx_id >= 0) && (dmx_id < T5D_DEMUX_PER_DEVICE));
	t5d_param_check((filter_id >= 0) && (filter_id < T5D_FILTER_PER_DEMUX));

	dmx = &dvbdev.demuxes[dmx_id];
	filter = &dmx->filters[filter_id];

	mutex_lock(&dvbdev.lock);

	if (filter->used) {
		filter->cb        = cb;
		filter->user_data = user_data;

		r = 0;
	} else {
		print_err("filter %d is not used", filter_id);
		r = -1;
	}

	mutex_unlock(&dvbdev.lock);

	return r;
}

/**
 * Start a filter.
 * @param dmx_id Demux device's index.
 * @param filter_id The filter's index.
 * @retval 0 On success.
 * @retval -1 On error.
 */
int
t5d_start_filter(
	int dmx_id,
	int filter_id)
{
	struct t5d_demux *dmx;
	struct t5d_filter *filter;
	int r;

	t5d_param_check((dmx_id >= 0) && (dmx_id < T5D_DEMUX_PER_DEVICE));
	t5d_param_check((filter_id >= 0) && (filter_id < T5D_FILTER_PER_DEMUX));

	dmx = &dvbdev.demuxes[dmx_id];
	filter = &dmx->filters[filter_id];

	mutex_lock(&dvbdev.lock);

	if (filter->used) {
		if (filter->type == T5D_FILTER_PES) {
			if (filter->sw_ts_filter) {
				swdmx_ts_filter_enable(filter->sw_ts_filter);
				r = 0;
			} else {
				print_err("filter %d is not set", filter_id);
				r = -1;
			}
		} else {
			if (filter->sw_sec_filter) {
				swdmx_sec_filter_enable(filter->sw_sec_filter);
				r = 0;
			} else {
				print_err("filter %d is not set", filter_id);
				r = -1;
			}
		}

		if (r == 0)
			filter->started = true;
	} else {
		print_err("filter %d is not used", filter_id);
		r = -1;
	}

	mutex_unlock(&dvbdev.lock);

	return r;
}

/**
 * Stop a filter.
 * @param dmx_id Demux device's index.
 * @param filter_id The filter's index.
 * @retval 0 On success.
 * @retval -1 On error.
 */
int
t5d_stop_filter(
	int dmx_id,
	int filter_id)
{
	struct t5d_demux *dmx;
	struct t5d_filter *filter;
	int r;

	t5d_param_check((dmx_id >= 0) && (dmx_id < T5D_DEMUX_PER_DEVICE));
	t5d_param_check((filter_id >= 0) && (filter_id < T5D_FILTER_PER_DEMUX));

	dmx = &dvbdev.demuxes[dmx_id];
	filter = &dmx->filters[filter_id];

	mutex_lock(&dvbdev.lock);

	if (filter->used) {
		if (filter->type == T5D_FILTER_PES) {
			if (filter->sw_ts_filter) {
				swdmx_ts_filter_disable(filter->sw_ts_filter);
				r = 0;
			} else {
				print_err("filter %d is not set", filter_id);
				r = -1;
			}
		} else {
			if (filter->sw_sec_filter) {
				swdmx_sec_filter_disable(filter->sw_sec_filter);
				r = 0;
			} else {
				print_err("filter %d is not set", filter_id);
				r = -1;
			}
		}

		if (r == 0)
			filter->started = false;
	} else {
		print_err("filter %d is not used", filter_id);
		r = -1;
	}

	mutex_unlock(&dvbdev.lock);

	return r;
}

/**
 * Process the TS stream.
 * @param source The input source type.
 * @param data The TS data buffer.
 * @param len The data length.
 * @retval 0 On success.
 * @retval -1 On error.
 */
int
t5d_process_stream(
	enum t5d_demux_source source,
	const u8 *data,
	int len)
{
	struct t5d_stream *s;
	const u8 *src = data;
	int src_left  = len;
	u8  *buf;

	s = stream_get(source);

	if (!s->ts_buffer) {
		s->ts_buffer = swdmx_malloc(T5D_STREAM_TS_BUFFER_SIZE);
	}

	buf = s->ts_buffer;

	mutex_lock(&dvbdev.lock);

	if (src_left + s->cache_len < 188) {
		memcpy(s->cache_buffer + s->cache_len, src, src_left);
		s->cache_len += src_left;
	} else {
		int buffered = 0;
		int cpy;

		if (s->cache_len) {
			memcpy(buf, s->cache_buffer, s->cache_len);
			buffered += s->cache_len;
		}

		while (1) {
			int solved, buf_left;

			cpy = min(src_left, T5D_STREAM_TS_BUFFER_SIZE - buffered);

			memcpy(buf + buffered, src, cpy);
			buffered += cpy;
			src      += cpy;
			src_left -= cpy;

			if (cpy == 0)
				print_err("start parser, src_left:%d, cpy:%d\n", src_left, cpy);
			solved = swdmx_ts_parser_run(s->sw_ts_parser, buf, buffered);

			buf_left = buffered - solved;

			if (src_left) {
				if (buf_left) {
					memmove(buf, buf + solved, buf_left);
				}
				buffered = buf_left;
			} else {
				if (buf_left)
					memcpy(s->cache_buffer, buf + solved, buf_left);

				s->cache_len = buf_left;
				break;
			}
		}
	}

	mutex_unlock(&dvbdev.lock);

	return 0;
}

/**
 * Dump all CA channel infos.
 * @param buf The output CA channel infos.
 * @retval 0 On success.
 * @retval -1 On error.
 */
int
t5d_dump_ca_info(char *buf)
{
	int i, j;
	int r, total = 0;
	struct t5d_demux *dmx;
	struct t5d_descrambler *desc = NULL;

	mutex_lock(&dvbdev.lock);
	for (i = 0; i < T5D_DEMUX_PER_DEVICE; i++) {
		dmx = &dvbdev.demuxes[i];
		r = sprintf(buf, "dmx%d\n", i);
		buf += r;
		total += r;
		for (j = 0; j < T5D_DESC_PER_DEMUX; j ++) {
			desc = &dmx->descs[j];

			if (!desc->used) {
				continue;
			}
			r = sprintf(buf, "pid:%#x, slot:%d, algo:%s\n",
				    desc->pid, j,
				    (desc->algo == CA_ALGO_AES_CBC_CLR_END) ? "aes-cbc" : "csa");
			buf += r;
			total += r;

			r = sprintf(buf, "    even:%d, even iv:%d, odd:%d, odd iv:%d\n",
				    desc->even_key_id,
				    desc->even_iv_id,
				    desc->odd_key_id,
				    desc->odd_iv_id);
			buf += r;
			total += r;
		}
	}

	mutex_unlock(&dvbdev.lock);

	return total;
}

/**
 * Allocate a descrambler channel.
 * @param ca_id The CA device index.
 * @param pid PID.
 * @param algo Descrambling algorithm.
 * @param type Module type.
 * @retval 0 On success.
 * @retval -1 On error.
 */
int
t5d_alloc_ca_chan(
	int ca_id,
	u16 pid,
	enum ca_sc2_algo_type algo,
	enum ca_sc2_dsc_type type)
{
	struct t5d_demux *dmx;
	int i, chan_id = -1;

	t5d_param_check((ca_id >= 0) && (ca_id < T5D_DEMUX_PER_DEVICE));

	dmx = &dvbdev.demuxes[ca_id];

	mutex_lock(&dvbdev.lock);

	for (i = 0; i < T5D_DESC_PER_DEMUX; i ++) {
		struct t5d_descrambler *desc = &dmx->descs[i];

		if (!desc->used) {
			struct t5d_stream *s = stream_get(dmx->source);

			chan_id = i;
			desc->used = true;
			desc->pid  = pid;
			desc->algo = algo;
			desc->scb_as_is   = false;
			desc->scb_value   = 0;
			desc->odd_key_id  = -1;
			desc->even_key_id = -1;
			desc->zero_key_id = -1;
			desc->odd_iv_id   = -1;
			desc->even_iv_id  = -1;
			desc->zero_iv_id  = -1;

			desc_set(desc, s);

			break;
		}
	}

	mutex_unlock(&dvbdev.lock);

	if (chan_id == -1)
		print_err("cannot allocate descrambler");

	return chan_id;
}

/**
 * Free a descrambler channel.
 * @param ca_id The CA device index.
 * @param chan_id The descrambler channel index.
 * @retval 0 On success.
 * @retval -1 On error.
 */
int
t5d_free_ca_chan(
	int ca_id,
	int chan_id)
{
	struct t5d_demux *dmx;
	struct t5d_descrambler *desc;
	int r;

	t5d_param_check((ca_id >= 0) && (ca_id < T5D_DEMUX_PER_DEVICE));
	t5d_param_check((chan_id >= 0) && (chan_id < T5D_DESC_PER_DEMUX));

	dmx = &dvbdev.demuxes[ca_id];
	desc = &dmx->descs[chan_id];

	mutex_lock(&dvbdev.lock);

	if (desc->used) {
		desc_clear(desc);

		desc->used = false;

		r = 0;
	} else {
		r = -1;
		print_err("descrambler %d is not used", chan_id);
	}

	mutex_unlock(&dvbdev.lock);

	return r;
}

/**
 * Set the descrambler's key.
 * @param ca_id The CA device index.
 * @param chan_id The descrambler channel index.
 * @param type The key's parity.
 * @param key_id The ke entry's index.
 * @retval 0 On success.
 * @retval -1 On error.
 */
int
t5d_set_ca_key(
	int ca_id,
	int chan_id,
	enum ca_sc2_key_type type,
	int key_id)
{
	struct t5d_demux *dmx;
	struct t5d_descrambler *desc;
	int r;

	t5d_param_check((ca_id >= 0) && (ca_id < T5D_DEMUX_PER_DEVICE));
	t5d_param_check((chan_id >= 0) && (chan_id < T5D_DESC_PER_DEMUX));
	t5d_param_check((key_id >= 0) && (key_id < T5D_KEY_PER_DEVICE));

	dmx = &dvbdev.demuxes[ca_id];
	desc = &dmx->descs[chan_id];

	mutex_lock(&dvbdev.lock);

	if (desc->used) {
		switch (type) {
		case CA_KEY_EVEN_TYPE:
			desc->even_key_id = key_id;
			break;
		case CA_KEY_EVEN_IV_TYPE:
			desc->even_iv_id = key_id;
			break;
		case CA_KEY_ODD_TYPE:
			desc->odd_key_id = key_id;
			break;
		case CA_KEY_ODD_IV_TYPE:
			desc->odd_iv_id = key_id;
			break;
		case CA_KEY_00_TYPE:
			desc->zero_key_id = key_id;
			break;
		case CA_KEY_00_IV_TYPE:
			desc->zero_iv_id = key_id;
			break;
		}

		desc_key_set(desc, type);
		r = 0;
	} else {
		r = -1;
		print_err("descrambler %d is not used", chan_id);
	}

	mutex_unlock(&dvbdev.lock);

	return r;
}

/**
 * Reset the descrambler's algorithm.
 * @param ca_id The CA device index.
 * @param chan_id The descrambler channel index.
 * @param algo The algorithm.
 * @retval 0 On success.
 * @retval -1 On error.
 */
int
t5d_set_ca_algo(
	int ca_id,
	int chan_id,
	enum ca_sc2_algo_type algo)
{
	struct t5d_demux *dmx;
	struct t5d_descrambler *desc;
	int r;

	t5d_param_check((ca_id >= 0) && (ca_id < T5D_DEMUX_PER_DEVICE));
	t5d_param_check((chan_id >= 0) && (chan_id < T5D_DESC_PER_DEMUX));

	dmx = &dvbdev.demuxes[ca_id];
	desc = &dmx->descs[chan_id];

	mutex_lock(&dvbdev.lock);

	if (desc->used) {
		desc->algo = algo;

		if (desc->algo == CA_ALGO_CSA2 || desc->algo == CA_ALGO_CSA3) {
			r = 0;
			print_dbg("descrambler set algo:%d\n", desc->algo);
		} else if (desc->sw_desc_chan) {
			if (desc->algo == CA_ALGO_AES_CBC_CLR_END) {
				swdmx_desc_channel_set_cb(desc->sw_desc_chan, swdmx_aes_cbc_dec_desc_cb);
				print_dbg("descrambler set algo:%d\n", desc->algo);
				r = 0;
			} else {
				print_err("descrambler does not support the algo: %d\n", desc->algo);
				r = -1;
			}
		} else {
			print_err("descrambler %d is not allocated", chan_id);
			r = -1;
		}
	} else {
		r = -1;
		print_err("descrambler %d is not used", chan_id);
	}

	mutex_unlock(&dvbdev.lock);

	return r;
}

/**
 * Set the output scrambling control flag.
 * @param ca_id The CA device index.
 * @param chan_id The descrambler channel index.
 * @param as_is Keep the origin scrambling control.
 * @param scb The new scrambling control.
 * @retval 0 On success.
 * @retval -1 On error.
 */
int
t5d_set_ca_scb(
	int ca_id,
	int chan_id,
	bool as_is,
	int scb)
{
	struct t5d_demux *dmx;
	struct t5d_descrambler *desc;
	int r;

	t5d_param_check((ca_id >= 0) && (ca_id < T5D_DEMUX_PER_DEVICE));
	t5d_param_check((chan_id >= 0) && (chan_id < T5D_DESC_PER_DEMUX));

	dmx = &dvbdev.demuxes[ca_id];
	desc = &dmx->descs[chan_id];

	mutex_lock(&dvbdev.lock);

	if (desc->used) {
		desc->scb_as_is = as_is;
		desc->scb_value = scb;

		if (desc->algo == CA_ALGO_CSA2 || desc->algo == CA_ALGO_CSA3) {
			t5d_hwdmx_desc_channel_set_scb(desc->hw_desc_chan, as_is, scb);
			r = 0;
		} else if (desc->sw_desc_chan) {
			swdmx_desc_channel_set_scb(desc->sw_desc_chan, as_is, scb);
			r = 0;
		} else {
			print_err("descrambler %d is not allocated", chan_id);
			r = -1;
		}
	} else {
		r = -1;
		print_err("descrambler %d is not used", chan_id);
	}

	mutex_unlock(&dvbdev.lock);

	return r;
}

/**
 * Clear the descramblers in a CA device.
 * @param ca_id The CA device index.
 * @retval 0 On success.
 * @retval -1 On error.
 */
int
t5d_clear_ca(int ca_id)
{
	struct t5d_demux *dmx;
	int i;

	t5d_param_check((ca_id >= 0) && (ca_id < T5D_DEMUX_PER_DEVICE));

	dmx = &dvbdev.demuxes[ca_id];

	mutex_lock(&dvbdev.lock);

	for (i = 0; i < T5D_DESC_PER_DEMUX; i ++) {
		struct t5d_descrambler *desc;

		desc = &dmx->descs[i];

		if (desc->used) {
			desc_clear(desc);
			desc->used = false;
		}
	}

	mutex_unlock(&dvbdev.lock);

	return 0;
}

/**
 * Allocate a new key entry.
 * @param is_iv The key is an IV data.
 * @return The new key entry's index.
 * @retval -1 On error.
 */
int
t5d_alloc_key(int is_iv)
{
	int i, key_id = -1;

	mutex_lock(&dvbdev.lock);

	for (i = 0; i < T5D_KEY_PER_DEVICE; i ++) {
		struct t5d_key *key = &dvbdev.keys[i];

		if (!key->used) {
			key_id = i;

			key->used = true;
			key->sw_key.is_iv = is_iv;
			break;
		}
	}

	mutex_unlock(&dvbdev.lock);

	if (key_id == -1)
		print_err("cannot allocate a new key");

	return key_id;
}

/**
 * Free a key entry.
 * @param key_id The key entry's index.
 * @retval 0 On success.
 * @retval -1 On error.
 */
int
t5d_free_key(
	int key_id)
{
	struct t5d_key *key;
	int r;

	t5d_param_check((key_id >= 0) && (key_id < T5D_KEY_PER_DEVICE));

	key = &dvbdev.keys[key_id];

	mutex_lock(&dvbdev.lock);

	if (key->used) {
		if (key->algo == KEY_ALGO_CSA2 || key->algo == KEY_ALGO_CSA3)
			t5d_hwdmx_key_clear(&key->hw_key);
		else
			swdmx_key_clear(&key->sw_key);

		key->used = false;

		r = 0;
	} else {
		print_err("key %d is not used", key_id);
		r = -1;
	}

	mutex_unlock(&dvbdev.lock);

	return r;
}

/**
 * Configure the key entry.
 * @param key_id The key entry's index.
 * @param user The module user index.
 * @param algo The algorithm.
 * @retval 0 On success.
 * @retval -1 On error.
 */
int
t5d_config_key(
	int key_id,
	enum user_id user,
	enum key_algo algo)
{
	struct t5d_key *key;
	int r;

	t5d_param_check((key_id >= 0) && (key_id < T5D_KEY_PER_DEVICE));

	key = &dvbdev.keys[key_id];

	mutex_lock(&dvbdev.lock);

	if (key->used) {
		key->user = user;
		key->algo = algo;

		if ((user == DSC_LOC_DEC) || (user == DSC_NETWORK)) {
			if (algo == KEY_ALGO_AES) {
				swdmx_aes_dec_key_init(&key->sw_key);
			}
		}

		r = 0;
	} else {
		print_err("key %d is not used", key_id);
		r = -1;
	}

	mutex_unlock(&dvbdev.lock);

	return r;
}

/**
 * Set the key.
 * @param key_id The key entry's index.
 * @param key The key value.
 * @param len The key's length.
 * @retval 0 On success.
 * @retval -1 On error.
 */
int
t5d_set_key(
	int key_id,
	const u8 *key,
	int len)
{
	struct t5d_key *kent;
	int r;

	t5d_param_check((key_id >= 0) && (key_id < T5D_KEY_PER_DEVICE));

	kent = &dvbdev.keys[key_id];

	mutex_lock(&dvbdev.lock);

	if (kent->used) {
		if (kent->algo == KEY_ALGO_CSA2 || kent->algo == KEY_ALGO_CSA3)
			t5d_hwdmx_key_set(&kent->hw_key, key, len);
		else
			swdmx_key_set(&kent->sw_key, key, len);

		r = 0;
	} else {
		print_err("key %d is not used", key_id);
		r = -1;
	}

	mutex_unlock(&dvbdev.lock);

	return r;
}

/**
 * Clear the key table.
 * @retval 0 On success.
 * @retval -1 On error.
 */
int
t5d_clear_keys(void)
{
	int i;

	mutex_lock(&dvbdev.lock);

	for (i = 0; i < T5D_KEY_PER_DEVICE; i ++) {
		struct t5d_key *key = &dvbdev.keys[i];

		if (key->used) {
			if (key->algo == KEY_ALGO_CSA2 || key->algo == KEY_ALGO_CSA3)
				t5d_hwdmx_key_clear(&key->hw_key);
			else
				swdmx_key_clear(&key->sw_key);

			key->used = false;
		}
	}

	mutex_unlock(&dvbdev.lock);

	return 0;
}
