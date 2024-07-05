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

#include <linux/version.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>

#include <linux/wait.h>
#include <linux/string.h>
#include <linux/interrupt.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/fcntl.h>
#include <linux/uaccess.h>
#include <linux/poll.h>
#include <linux/delay.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/dvb/dmx.h>
#include <media/dvb_demux.h>
#include <media/dvb_frontend.h>
#include <media/dmxdev.h>
#include <uapi/linux/dvb/dmx.h>
#include <cpu_version.h>
#include <linux/pinctrl/consumer.h>
#include <linux/amlogic/media/registers/cpu_version.h>
#include <linux/amlogic/media/codec_mm/codec_mm.h>
#include <linux/amlogic/media/codec_mm/configs.h>
#include <linux/dvb/aml_dmx_ext.h>
#include <linux/dvb/aml_ca_ext.h>

#include "aml_demux_ext.h"
#include "t5d_dvb_reg.h"
#include "t5d_hw_dsc.h"
#include "t5d_demux.h"
#include "t5d_hw_dmx.h"
#include "t5d_dsc.h"
#include "t5d_key.h"

struct t5d_pcr {
	int valid;
	u64 last_pcr;
	u64 last_jiffies;
};

struct t5d_video {
	int used;
	int dmx_id;
	int fid;

	u8 *buf;
	unsigned long phys;
	u32 len;
	u32 w_offset;
	u32 r_offset;
	u64 pts;
	u64 apts;
	int pes_type;
	int v_passthrough;
};

struct t5d_dump_node {
	int sid;
	int pid;
	int dump_type;
	struct dvb_demux_feed *feed;
	struct list_head node;
};

#define print_err(fmt, args...)   \
	dprintk(LOG_ERROR, debug_dvb, fmt, ## args)
#define print_dbg(fmt, args...)   \
	dprintk(LOG_DBG, debug_dvb, fmt, ## args)
#define print_ver(fmt, args...)   \
	dprintk(LOG_VER, debug_dvb, fmt, ## args)

MODULE_PARM_DESC(debug_dvb, "\n\t\t Enable demux debug information");
static int debug_dvb;
module_param(debug_dvb, int, 0644);

MODULE_PARM_DESC(flow_control, "\n\t\t flow control percentage");
static int flow_control = 80;
module_param(flow_control, int, 0644);

#define DVB_VERSION "V0.10"
#define T5D_PCR_PER_DEMUX 4
#define T5D_VIDEO_PER_DEMUX 4
#define T5D_VIDEO_BUFFER_LEN (15*1024*1024)

struct t5d_sw_demux_feed {
	struct dvb_demux_feed *feed;
	struct list_head node;
};

struct t5d_sw_demux {
	struct dmx_demux_ext	demux;
	struct dmxdev			dmxdev;
	struct t5d_pcr			pcrs[T5D_PCR_PER_DEMUX];
	struct t5d_video		videos[T5D_VIDEO_PER_DEMUX];
	struct dmx_frontend		hw_fes[T5D_HW_STREAM_NUM];
	struct dmx_frontend		mem_fe;
	void 					*buf;
	struct t5d_sw_dsc		dsc;
};

LIST_HEAD(t5d_dump_es_head);
static struct mutex 		t5d_mutex;
static struct t5d_sw_demux 	t5d_sw_demuxes[T5D_DEMUX_PER_DEVICE];
static struct t5d_s2p		t5d_s2p[T5D_S2P_COUNT];
static struct t5d_ts_in		t5d_ts_input[T5D_TS_IN_COUNT];

/*PES type to ID*/
static int
pes_type2id(int pes_type)
{
	int id = -1;

	switch (pes_type) {
	case DMX_PES_VIDEO0:
	case DMX_PES_AUDIO0:
	case DMX_PES_PCR0:
		id = 0;
		break;
	case DMX_PES_VIDEO1:
	case DMX_PES_AUDIO1:
	case DMX_PES_PCR1:
		id = 1;
		break;
	case DMX_PES_VIDEO2:
	case DMX_PES_AUDIO2:
	case DMX_PES_PCR2:
		id = 2;
		break;
	case DMX_PES_VIDEO3:
	case DMX_PES_AUDIO3:
	case DMX_PES_PCR3:
		id = 3;
		break;
	}

	return id;
}

/*PCR callback function.*/
static void
pcr_callback(int dmx_id, int fid, const u8 *data, int len, void *user_data)
{
	struct dvb_demux_feed *feed = user_data;
	u64 pcr = *(u64 *)data;
	int pcr_id = -1;

	if (feed) {
		struct t5d_pcr *slot;

		mutex_lock(&t5d_mutex);
		pcr_id = pes_type2id(feed->pes_type);
		if (pcr_id < 0)
			return;
		slot = &t5d_sw_demuxes[dmx_id].pcrs[pcr_id];

		slot->valid		   = true;
		slot->last_pcr	   = pcr;
		slot->last_jiffies = jiffies_to_msecs(get_jiffies_64()) * 90;
		mutex_unlock(&t5d_mutex);
	}
}

/*Get PCR.*/
int
demux_get_pcr(int dmx_id, int pcr_id, u64 *pcr)
{
	struct t5d_pcr *slot;
	u64 v, diff;

	if ((dmx_id < 0) || (dmx_id >= T5D_DEMUX_PER_DEVICE))
		return -1;

	if ((pcr_id < 0) || (pcr_id >= T5D_PCR_PER_DEMUX))
		return -1;

	slot  = &t5d_sw_demuxes[dmx_id].pcrs[pcr_id];

	if (!slot->valid)
		return -1;

	diff = jiffies_to_msecs(get_jiffies_64()) * 90 - slot->last_jiffies;
	v	 = (slot->last_pcr + diff) & 0x1ffffffffull;

	*pcr = v;
	return 0;
}
EXPORT_SYMBOL(demux_get_pcr);

/*PES callback function.*/
static void
pes_callback(int dmx_id, int fid, const u8 *data, int len, void *user_data)
{
	int sid;
	struct dvb_demux_feed *feed = user_data;
	struct t5d_dump_node *d_entry = NULL;
	struct t5d_dump_node *d_tmp = NULL;
	struct dvb_demux_feed *sw_feed = NULL;

	if (feed && feed->cb.ts)
		feed->cb.ts(data, len, NULL, 0, &feed->feed.ts, 0);

	sid = t5d_get_demux_source_unlock(dmx_id);
	list_for_each_entry_safe(d_entry, d_tmp, &t5d_dump_es_head, node) {
		if (d_entry->dump_type == DMX_DUMP_PES_TYPE
		    && d_entry->sid == sid
		    && (d_entry->pid == feed->pid
			|| d_entry->pid == 0x1fff)) {
			sw_feed = d_entry->feed;
			if (sw_feed && sw_feed->cb.ts) {
				print_ver("dump pes %#x bytes.\n", len);
				sw_feed->cb.ts(data, len, NULL, 0, &sw_feed->feed.ts, 0);
			}
		}
	}
}

/*Video callback function.*/
static void
video_callback(int dmx_id, int fid, const u8 *data, int len, void *user_data)
{
	int space, video_id = -1;
	struct t5d_video *dmx_video = NULL;
	struct dvb_demux_feed *feed = user_data;
	struct t5d_es_data *es = (struct t5d_es_data *)data;
	struct dmx_sec_es_data hdr;
	struct dmx_non_sec_es_header non_sec_hdr;
	u32 last_offset;
	struct t5d_dump_node *d_entry = NULL;
	struct t5d_dump_node *d_tmp = NULL;
	struct dvb_demux_feed *sw_feed = NULL;
	int sid;

	mutex_lock(&t5d_mutex);
	if (feed) {
		video_id = pes_type2id(feed->pes_type);
	}
	if (video_id < 0) {
		mutex_unlock(&t5d_mutex);
		return;
	}

	dmx_video = &t5d_sw_demuxes[dmx_id].videos[video_id];
	if (dmx_video->buf == NULL) {
		mutex_unlock(&t5d_mutex);
		return;
	}

	last_offset = dmx_video->w_offset;
	if (es->valid) {
		space = dmx_video->len - dmx_video->w_offset;
		if (es->len <= space) {
			memcpy(dmx_video->buf + dmx_video->w_offset, es->data, es->len);
			codec_mm_dma_flush(dmx_video->buf + dmx_video->w_offset,
					   es->len, DMA_TO_DEVICE);
			dmx_video->w_offset += es->len;
		} else {
			memcpy((void *)(dmx_video->buf + dmx_video->w_offset), (void *)es->data, space);
			codec_mm_dma_flush(dmx_video->buf + dmx_video->w_offset, space, DMA_TO_DEVICE);
			memcpy((void *)dmx_video->buf, (void *)(es->data + space), es->len - space);
			codec_mm_dma_flush(dmx_video->buf, es->len - space, DMA_TO_DEVICE);
			dmx_video->w_offset = es->len - space;
			if (dmx_video->w_offset > dmx_video->r_offset)
				print_err("%s overflow\n", __func__);
		}
	}

	dmx_video->pts = es->pts;
	hdr.pts_dts_flag = 0;
	non_sec_hdr.pts_dts_flag = 0;

	if (es->has_pts) {
		hdr.pts_dts_flag |= 2;
		non_sec_hdr.pts_dts_flag |= 2;
	}
	if (!es->valid) {
		hdr.pts_dts_flag |= 3;
		non_sec_hdr.pts_dts_flag |= 3;
	}
	if (es->scrambled) {
		hdr.pts_dts_flag |= 4;
		non_sec_hdr.pts_dts_flag |= 4;
	}

	hdr.pts = es->pts;
	hdr.dts = 0;
	hdr.buf_start  = (u32)dmx_video->phys;
	hdr.buf_end    = (u32)(dmx_video->phys + dmx_video->len);
	hdr.data_start = (u32)(dmx_video->phys + last_offset);
	hdr.data_end   = (u32)(dmx_video->phys + dmx_video->w_offset);

	if (feed && feed->cb.ts) {
		if (dmx_video->v_passthrough) {
			feed->cb.ts((const u8 *)&hdr, sizeof(hdr), NULL, 0, &feed->feed.ts, 0);
			print_ver("video pts: %llx, start: %#x, end: %#x, data start :%#x, data end: %#x\n",
				  hdr.pts, hdr.buf_start, hdr.buf_end, hdr.data_start, hdr.data_end);
		} else {
			non_sec_hdr.pts = es->pts;
			non_sec_hdr.dts = 0;
			non_sec_hdr.len = es->len;
			feed->cb.ts((const u8 *)&non_sec_hdr, sizeof(non_sec_hdr), NULL, 0, &feed->feed.ts, 0);
			feed->cb.ts(es->data, es->len, NULL, 0, &feed->feed.ts, 0);
			print_ver("video pts: %llx, len:%#x,\n", non_sec_hdr.pts, non_sec_hdr.len);
		}
	}

	sid = t5d_get_demux_source_unlock(dmx_id);
	list_for_each_entry_safe(d_entry, d_tmp, &t5d_dump_es_head, node) {
		print_ver("dump cb, sid:%#x, pid:%#x, dump_type:%#x\n",
			  d_entry->sid, d_entry->pid, d_entry->dump_type);
		if (d_entry->dump_type == DMX_DUMP_ES_VIDEO_TYPE
		    && d_entry->sid == sid
		    && (d_entry->pid == feed->pid
			|| d_entry->pid == 0x1fff)) {
			sw_feed = d_entry->feed;
			if (sw_feed && sw_feed->cb.ts)
				print_ver("dump video %#x bytes\n", es->len);
			sw_feed->cb.ts(es->data, es->len, NULL, 0, &sw_feed->feed.ts, 0);
		}
	}
	mutex_unlock(&t5d_mutex);
}

/*Audio callback function.*/
static void
audio_callback(int dmx_id, int fid, const u8 *data, int len, void *user_data)
{
	int id;
	int sid;
	struct t5d_video *dmx_video = NULL;
	struct dvb_demux_feed *feed = user_data;
	struct t5d_es_data *es = (struct t5d_es_data *)data;
	struct dmx_non_sec_es_header hdr;
	struct t5d_dump_node *d_entry = NULL;
	struct t5d_dump_node *d_tmp = NULL;
	struct dvb_demux_feed *sw_feed = NULL;

	id = pes_type2id(feed->pes_type);
	dmx_video = &t5d_sw_demuxes[dmx_id].videos[id];
	hdr.pts_dts_flag = 0;

	if (es->has_pts)
		hdr.pts_dts_flag |= 2;
	if (!es->valid)
		hdr.pts_dts_flag |= 3;
	if (es->scrambled)
		hdr.pts_dts_flag |= 4;

	dmx_video->apts = es->pts;
	hdr.pts = es->pts;
	hdr.dts = 0;
	hdr.len = es->len;

	if (feed && feed->cb.ts) {
		print_ver("audio pts: %llx, len:%#x\n", hdr.pts, hdr.len);
		feed->cb.ts((const u8 *)&hdr, sizeof(hdr), NULL, 0, &feed->feed.ts, 0);

		if (hdr.len)
			feed->cb.ts(es->data, es->len, NULL, 0, &feed->feed.ts, 0);
	}

	sid = t5d_get_demux_source_unlock(dmx_id);
	list_for_each_entry_safe(d_entry, d_tmp, &t5d_dump_es_head, node) {
		if (d_entry->dump_type == DMX_DUMP_ES_AUDIO_TYPE
		    && d_entry->sid == sid
		    && (d_entry->pid == feed->pid
			|| d_entry->pid == 0x1fff)) {
			sw_feed = d_entry->feed;
			if (sw_feed && sw_feed->cb.ts) {
				print_ver("dump audio %#x bytes\n", es->len);
				sw_feed->cb.ts(es->data, es->len, NULL, 0, &sw_feed->feed.ts, 0);
			}
		}
	}
}

/*DVR callback function.*/
static void
dvr_callback(int dmx_id, int fid, const u8 *data, int len, void *user_data)
{
	struct dvb_demux_feed *feed = user_data;

	if (feed && feed->cb.ts)
		feed->cb.ts(data, len, NULL, 0, &feed->feed.ts, 0);
}

/*Section callback function.*/
static void
sec_callback(int dmx_id, int fid, const u8 *data, int len, void *user_data)
{
	struct dvb_demux_filter *filter = user_data;

	if (filter) {
		struct dvb_demux_feed *feed = filter->feed;

		if (feed && feed->cb.sec) {
			feed->cb.sec(data, len, NULL, 0, &filter->filter, 0);
		}
	}
}

/*Start a feed.*/
static int
dmx_start_feed(struct dvb_demux_feed *feed)
{
	struct t5d_sw_demux *dmx = (struct t5d_sw_demux *)feed->demux;
	int dmx_id = dmx - t5d_sw_demuxes;
	int r = -1;
	struct t5d_dump_node *d_node = NULL;
	struct dmxdev_filter *filter = NULL;
	int dump_type;
	int dump_sid = 0;

	print_dbg("feed type: %d, dmx_id: %d\n", feed->type, dmx_id);
	filter = feed->feed.ts.priv;
	if (feed->type == DMX_TYPE_TS) {
		struct dmx_pes_filter_params p;
		t5d_filter_cb cb = NULL;
		int fid;

		if (feed->pes_type == DMX_PES_OTHER) {
			dump_type = ((filter->params.pes.flags >> 16) & 0xff);
			if (dump_type >= DMX_DUMP_DVR_TYPE
			    && dump_type <= DMX_DUMP_INPUT_TYPE) {
				dump_sid = ((filter->params.pes.flags >> 24) & 0xff);
				//filter->params.pes.flags &= 0x0000ffff;

				print_dbg("filter: %#x dump type %#x, sid: %#x\n",
					  filter, dump_type, dump_sid);
				if (dump_type == DMX_DUMP_TS_TYPE
				    || dump_type == DMX_DUMP_INPUT_TYPE) {
					t5d_dump_add_sid(dump_sid, feed, dump_type);
					return 0;
				} else {
					d_node = kmalloc(sizeof(*d_node), GFP_KERNEL);
					if (!d_node)
						return -ENOMEM;;
					memset(d_node, 0, sizeof(struct t5d_dump_node));
					INIT_LIST_HEAD(&d_node->node);

					d_node->feed = feed;
					d_node->sid = dump_sid;
					d_node->pid = feed->pid & 0x1fff;
					d_node->dump_type = dump_type;
					feed->priv = d_node;
					if (d_node->dump_type == DMX_DUMP_ES_TYPE
					    || d_node->dump_type == DMX_DUMP_ES_VIDEO_TYPE
					    || d_node->dump_type == DMX_DUMP_ES_AUDIO_TYPE
					    || d_node->dump_type == DMX_DUMP_PES_TYPE) {
						list_add(&d_node->node, &t5d_dump_es_head);
						print_dbg("add %#x to dump es list\n", feed->pid);
					}
				}
				return 0;
			}
		}

		if ((fid = t5d_alloc_filter(dmx_id)) == -1)
			goto end;

		memset(&p, 0, sizeof(p));

		p.pid	   = feed->pid;
		p.pes_type = feed->pes_type;

		print_dbg("feed pes_type: %d, flags: %d\n",
			  feed->pes_type,
			  filter->params.pes.flags);
		if ((feed->pes_type == DMX_PES_PCR0)
		    || (feed->pes_type == DMX_PES_PCR1)
		    || (feed->pes_type == DMX_PES_PCR2)
		    || (feed->pes_type == DMX_PES_PCR3)) {
			cb = pcr_callback;
		} else if ((feed->ts_type & (TS_PACKET | TS_DEMUX)) == TS_PACKET) {
			p.output = DMX_OUT_TS_TAP;
			cb = dvr_callback;
		} else if (((feed->pes_type == DMX_PES_VIDEO0)
			    || (feed->pes_type == DMX_PES_VIDEO1)
			    || (feed->pes_type == DMX_PES_VIDEO2)
			    || (feed->pes_type == DMX_PES_VIDEO3))
			   && (filter->params.pes.flags & DMX_ES_OUTPUT)) {
			int video_id = 0;
			struct t5d_video *video = NULL;
			int buf_page_num = 0;
			int flags = 0;

			p.output = DMX_OUT_DECODER;
			cb = video_callback;
			video_id = pes_type2id(feed->pes_type);

			mutex_lock(&t5d_mutex);
			video = &t5d_sw_demuxes[dmx_id].videos[video_id];
			buf_page_num = PAGE_ALIGN(T5D_VIDEO_BUFFER_LEN) / PAGE_SIZE;
			flags = CODEC_MM_FLAGS_DMA_CPU | CODEC_MM_FLAGS_FOR_VDECODER;
			video->dmx_id = dmx_id;
			video->pes_type = feed->pes_type;
			video->fid = fid;
			video->phys =
				codec_mm_alloc_for_dma("dmx", buf_page_num, 4 + PAGE_SHIFT, flags);
			video->buf = codec_mm_phys_to_virt(video->phys);
			video->len = T5D_VIDEO_BUFFER_LEN;
			video->w_offset = 0;
			video->r_offset = 0;
			if (filter->params.pes.flags & DMX_OUTPUT_RAW_MODE)
				video->v_passthrough = 1;
			else
				video->v_passthrough = 0;
			mutex_unlock(&t5d_mutex);
		} else if (((feed->pes_type == DMX_PES_AUDIO0)
			    || (feed->pes_type == DMX_PES_AUDIO1)
			    || (feed->pes_type == DMX_PES_AUDIO2)
			    || (feed->pes_type == DMX_PES_AUDIO3))
			   && (filter->params.pes.flags & DMX_ES_OUTPUT)) {
			p.output = DMX_OUT_DECODER;
			cb = audio_callback;
		} else {
			p.output = DMX_OUT_TAP;
			cb = pes_callback;
		}

		p.flags = filter->params.pes.flags;
		t5d_set_pes_filter(dmx_id, fid, &p);
		t5d_set_filter_callback(dmx_id, fid, cb, feed);
		t5d_start_filter(dmx_id, fid);

		feed->filter->hw_handle = fid;
	} else if (feed->type == DMX_TYPE_SEC) {
		struct dvb_demux_filter *filter;

		for (filter = feed->filter; filter; filter = filter->next) {
			struct dmx_sct_filter_params p;
			int fid;
			int i;

			if ((fid = t5d_alloc_filter(dmx_id)) == -1)
				goto end;

			memset(&p, 0, sizeof(p));

			p.pid = feed->pid;

			p.filter.filter[0] = filter->filter.filter_value[0];
			p.filter.mask[0]   = filter->filter.filter_mask[0];
			p.filter.mode[0]   = filter->filter.filter_mode[0];

			p.filter.mode[0] ^= 0xff;

			for (i = 1; i < 16; i ++) {
				p.filter.filter[i] = filter->filter.filter_value[i + 2];
				p.filter.mask[i]   = filter->filter.filter_mask[i + 2];
				p.filter.mode[i]   = filter->filter.filter_mode[i + 2];
				p.filter.mode[i]  ^= 0xff;
			}

			if (feed->feed.sec.check_crc)
				p.flags |= DMX_CHECK_CRC;

			print_dbg("dmx_id: %d, filter %#x\n", dmx_id, filter);
			t5d_set_sec_filter(dmx_id, fid, &p);
			t5d_set_filter_callback(dmx_id, fid, sec_callback, filter);
			t5d_start_filter(dmx_id, fid);

			filter->hw_handle = fid;
		}
	} else {
		print_err("illegal feed type: %d", feed->type);
		goto end;
	}
	r = 0;
end:
	return r;
}

/*Stop a feed.*/
static int
dmx_stop_feed(struct dvb_demux_feed *feed)
{
	struct t5d_sw_demux *dmx = (struct t5d_sw_demux *)feed->demux;
	int dmx_id = dmx - t5d_sw_demuxes;
	int r = -1;
	int dump_type;
	int dump_sid = 0;
	struct dmxdev_filter *filter = NULL;

	if (feed->type == DMX_TYPE_TS) {
		if (feed->pes_type == DMX_PES_OTHER) {
			filter = feed->feed.ts.priv;
			dump_type = ((filter->params.pes.flags >> 16) & 0xff);
			print_dbg("filter: %#x dump type %#x, sid: %#x\n",
				  filter, dump_type, dump_sid);
			if (dump_type >= DMX_DUMP_DVR_TYPE
			    && dump_type <= DMX_DUMP_INPUT_TYPE) {
				dump_sid = ((filter->params.pes.flags >> 24) & 0xff);
				filter->params.pes.flags &= 0x0000ffff;

				if (dump_type == DMX_DUMP_TS_TYPE
				    || dump_type == DMX_DUMP_INPUT_TYPE) {
					t5d_dump_remove_sid(dump_sid, feed);
					return 0;
				} else if (dump_type == DMX_DUMP_ES_TYPE
					   || dump_type == DMX_DUMP_ES_VIDEO_TYPE
					   || dump_type == DMX_DUMP_ES_AUDIO_TYPE
					   || dump_type == DMX_DUMP_PES_TYPE) {
					list_del(&((struct t5d_dump_node *)feed->priv)->node);
					print_dbg("remove %#x from dump es list\n", feed->pid);
				}
			}
		}
	}

	if (feed->type == DMX_TYPE_TS) {
		int video_id = 0;
		int fid;
		int pcr_id = -1;
		struct t5d_video *video = NULL;

		fid = feed->filter->hw_handle;

		t5d_stop_filter(dmx_id, fid);
		t5d_free_filter(dmx_id, fid);

		mutex_lock(&t5d_mutex);
		if ((feed->pes_type == DMX_PES_VIDEO0)
		    || (feed->pes_type == DMX_PES_VIDEO1)
		    || (feed->pes_type == DMX_PES_VIDEO2)
		    || (feed->pes_type == DMX_PES_VIDEO3)) {
			video_id = pes_type2id(feed->pes_type);

			video = &t5d_sw_demuxes[dmx_id].videos[video_id];
			codec_mm_free_for_dma("dmx", video->phys);
			memset((void *)video, 0, sizeof(struct t5d_video));
		}

		pcr_id = pes_type2id(feed->pes_type);
		if (pcr_id != -1) {
			struct t5d_pcr *slot = &t5d_sw_demuxes[dmx_id].pcrs[pcr_id];

			slot->valid = false;
		}
		mutex_unlock(&t5d_mutex);
	} else if (feed->type == DMX_TYPE_SEC) {
		struct dvb_demux_filter *filter;

		for (filter = feed->filter; filter; filter = filter->next) {
			int fid = filter->hw_handle;

			t5d_stop_filter(dmx_id, fid);
			t5d_free_filter(dmx_id, fid);
		}
	} else {
		print_err("illegal feed type: %d", feed->type);
		goto end;
	}
	r = 0;
end:
	return r;
}

int
dmx_get_stc(struct dmx_demux *dmx, unsigned int num,
	    u64 *stc, unsigned int *base)
{
	print_err("%s called. TODO:\n", __func__);
	return 0;
}

static int
dmx_set_input(struct dmx_demux *demux, int source)
{
	struct t5d_sw_demux *pdmx = (struct t5d_sw_demux *)demux;
	int dmx_id = pdmx - t5d_sw_demuxes;

	print_err("%s dmx_id: %d, source: %d\n", __func__, dmx_id, source);

	return 0;
}

static int
dmx_set_hw_source(struct dmx_demux *dmx, int hw_source)
{
	struct t5d_sw_demux *pdmx = (struct t5d_sw_demux *)dmx;
	int dmx_id = pdmx - t5d_sw_demuxes;

	if (hw_source >= FRONTEND_TS0_1)
		hw_source = FRONTEND_TS0 + (hw_source - FRONTEND_TS0_1);
	else if (hw_source >= DMA_0_1)
		hw_source = DMA_0 + (hw_source - DMA_0_1);

	print_dbg("%s dmx_id: %d, hw source: %d\n", __func__, dmx_id, hw_source);
	return t5d_set_demux_source(dmx_id, hw_source);
}

static int
dmx_get_hw_source(struct dmx_demux *dmx, int *hw_source)
{
	struct t5d_sw_demux *pdmx = (struct t5d_sw_demux *)dmx;
	int dmx_id = pdmx - t5d_sw_demuxes;

	*hw_source = t5d_get_demux_source(dmx_id);
	print_dbg("%s dmx_id: %d, hw source: %d\n", __func__, dmx_id, *hw_source);

	return 0;
}

static int
get_sec_mem_info(struct dmx_demux *dmx, void *v_feed, void *v_info)
{
	struct t5d_sw_demux *pdmx = (struct t5d_sw_demux *)dmx;
	int dmx_id = pdmx - t5d_sw_demuxes;
	struct t5d_video *dmx_video = NULL;
	struct dmx_mem_info *info = v_info;
	struct dvb_demux_feed *feed = (struct dvb_demux_feed *)v_feed;
	int video_id = -1;

	mutex_lock(&t5d_mutex);
	memset(info, 0, sizeof(struct dmx_mem_info));
	if (feed) {
		video_id = pes_type2id(feed->pes_type);
	}
	if (video_id < 0) {
		mutex_unlock(&t5d_mutex);
		return -1;
	}
	dmx_video = &t5d_sw_demuxes[dmx_id].videos[video_id];

	info->dmx_total_size = dmx_video->len;
	info->dmx_buf_phy_start = dmx_video->phys;
	if (dmx_video->w_offset >= dmx_video->r_offset) {
		info->dmx_free_size =
			dmx_video->len -
			dmx_video->w_offset +
			dmx_video->r_offset;
	} else {
		info->dmx_free_size = dmx_video->r_offset - dmx_video->w_offset;
	}
	info->wp_offset = dmx_video->w_offset;
	info->newest_pts = dmx_video->pts;

	print_dbg("phy start: %#x, total_size: %#x, wp offset: %#x\n",
		  info->dmx_buf_phy_start, info->dmx_total_size, info->wp_offset);

	mutex_unlock(&t5d_mutex);
	return 0;
}

static int
get_ts_mem_info(struct dmx_demux *dmx, void *v_feed, void *v_info)
{
	struct t5d_sw_demux *pdmx = (struct t5d_sw_demux *)dmx;
	int dmx_id = pdmx - t5d_sw_demuxes;
	struct t5d_video *dmx_video = NULL;
	struct dmx_mem_info *info = v_info;
	struct dvb_demux_feed *feed = (struct dvb_demux_feed *)v_feed;
	int video_id = -1;

	mutex_lock(&t5d_mutex);
	memset(info, 0, sizeof(struct dmx_mem_info));
	if (feed) {
		video_id = pes_type2id(feed->pes_type);
	}
	if (video_id < 0) {
		mutex_unlock(&t5d_mutex);
		return -1;
	}
	dmx_video = &t5d_sw_demuxes[dmx_id].videos[video_id];

	info->dmx_total_size = dmx_video->len;
	info->dmx_buf_phy_start = dmx_video->phys;
	if (dmx_video->w_offset >= dmx_video->r_offset) {
		info->dmx_free_size =
			dmx_video->len -
			dmx_video->w_offset +
			dmx_video->r_offset;
	} else {
		info->dmx_free_size = dmx_video->r_offset - dmx_video->w_offset;
	}
	info->wp_offset = dmx_video->w_offset;
	info->newest_pts = dmx_video->pts;

	print_dbg("dmx[%d] video[%d] phy start: %#x, dmx_total_size: %#x, wp offset: %#x, rp offset: %#x\n",
		  dmx_id, video_id,
		  info->dmx_buf_phy_start,
		  info->dmx_total_size,
		  info->wp_offset,
		  dmx_video->r_offset);

	mutex_unlock(&t5d_mutex);

	return 0;
}

static int
get_dmx_mem_info(struct dmx_demux *dmx, void *v_info)
{
	int id;
	int filter_num = 0;
	struct t5d_sw_demux *pdmx = (struct t5d_sw_demux *)dmx;
	int dmx_id = pdmx - t5d_sw_demuxes;
	struct dmx_filter_mem_info *info = v_info;
	struct dmxdev_filter *filter;
	struct filter_mem_info *pinfo;
	int free_mem = 0;
	int total_mem = 0;
	unsigned int buf_phy_start;
	unsigned int wp_offset, rp_offset, len;
	u64 newest_pts = 0;
	struct dvb_demux_feed *feed;
	struct dvb_demux *demux = (struct dvb_demux *)dmx;

	//mutex_lock(&demux->mutex);
	mutex_lock(&t5d_mutex);

	list_for_each_entry(feed, &demux->feed_list, list_head) {
		print_dbg("feed->type: %d, pes_type: %d\n", feed->type, feed->pes_type);
		if (feed->state != DMX_STATE_GO)
			continue;

		if (feed->type == DMX_TYPE_TS) {
			filter = feed->feed.ts.priv;
			pinfo = &info->info[filter_num];
			id = pes_type2id(feed->pes_type);
			if ((feed->pes_type == DMX_PES_VIDEO0)
			    || (feed->pes_type == DMX_PES_VIDEO1)
			    || (feed->pes_type == DMX_PES_VIDEO2)
			    || (feed->pes_type == DMX_PES_VIDEO3)) {
				pinfo->type = DMX_VIDEO_TYPE;
				buf_phy_start = t5d_sw_demuxes[dmx_id].videos[id].phys;
				wp_offset = t5d_sw_demuxes[dmx_id].videos[id].w_offset;
				rp_offset = t5d_sw_demuxes[dmx_id].videos[id].r_offset;
				len = t5d_sw_demuxes[dmx_id].videos[id].len;
				newest_pts = t5d_sw_demuxes[dmx_id].videos[id].pts;
				if (wp_offset >= rp_offset) {
					free_mem = len - (wp_offset - rp_offset);
				} else {
					free_mem = rp_offset - wp_offset;
				}

				pinfo->filter_info.dmx_buf_phy_start = buf_phy_start;
				pinfo->filter_info.dmx_free_size = free_mem;
				pinfo->filter_info.dmx_total_size = len;
				pinfo->filter_info.wp_offset = wp_offset;
				pinfo->filter_info.newest_pts = newest_pts;
				print_dbg("video dmx_total_size:%#x, free mem: %#x, wp_offset: %#x, newest_pts: %llx",
					  pinfo->filter_info.dmx_total_size,
					  free_mem, wp_offset, newest_pts);
			} else if ((feed->pes_type == DMX_PES_AUDIO0)
				   || (feed->pes_type == DMX_PES_AUDIO1)
				   || (feed->pes_type == DMX_PES_AUDIO2)
				   || (feed->pes_type == DMX_PES_AUDIO3)) {
				newest_pts = t5d_sw_demuxes[dmx_id].videos[id].apts;
				free_mem = dvb_ringbuffer_free(&filter->buffer);
				total_mem = filter->buffer.size;

				pinfo->type = DMX_AUDIO_TYPE;
				pinfo->filter_info.dvb_core_free_size = free_mem;
				pinfo->filter_info.dvb_core_total_size = total_mem;
				pinfo->filter_info.newest_pts = newest_pts;
				pinfo->filter_info.dmx_total_size = total_mem;
				print_dbg("audio dmx_total_size:%#x, free mem: %#x, newest_pts: %llx",
					  pinfo->filter_info.dmx_total_size,
					  free_mem, newest_pts);
			} else {
				print_dbg("ignore pes_type: %d\n", feed->pes_type);
				continue;
			}
		} else if (feed->type == DMX_TYPE_SEC) {
			filter = feed->filter->filter.priv;
			free_mem = dvb_ringbuffer_free(&filter->buffer);
			total_mem = filter->buffer.size;

			pinfo = &info->info[filter_num];
			pinfo->type = DMX_SECTION_TYPE;
			pinfo->filter_info.dvb_core_free_size = free_mem;
			pinfo->filter_info.dvb_core_total_size = total_mem;
		} else
			continue;

		pinfo->pid = feed->pid;
		filter_num++;
	}

	info->filter_num = filter_num;
	print_dbg("filter_num: %d\n", filter_num);
	//mutex_unlock(&demux->mutex);
	mutex_unlock(&t5d_mutex);
	return 0;
}

static int
dmx_decode_info(struct dmx_demux *dmx, void *v_info)
{
	struct t5d_sw_demux *pdmx = (struct t5d_sw_demux *)dmx;
	int dmx_id = pdmx - t5d_sw_demuxes;
	struct decoder_mem_info *info = v_info;

	mutex_lock(&t5d_mutex);
	t5d_sw_demuxes[dmx_id].videos[0].r_offset = info->rp_phy -
			t5d_sw_demuxes[dmx_id].videos[0].phys;
	print_dbg("rp_phy: %#x\n", info->rp_phy);
	mutex_unlock(&t5d_mutex);

	return 0;
}

static int
t5d_ts_input_parse(struct device *dev)
{
	int i;
	int ret;
	u32 value;
	char buf[32];
	const char *str;
	int s2p_id = 0;

	for (i = 0; i < T5D_TS_IN_COUNT; i++) {
		struct t5d_ts_in *ts = &t5d_ts_input[i];
		ts->mode = T5D_TS_DISABLE;
		ts->s2p_id = -1;
		ts->pinctrl = NULL;
		memset(buf, 0, sizeof(buf));
		snprintf(buf, sizeof(buf), "ts%d", i);
		ret = of_property_read_string(dev->of_node, buf, &str);
		if (!ret) {
			if (!strcmp(str, "serial")) {
				print_dbg("%s: serial\n", buf);
				if (s2p_id >= T5D_S2P_COUNT)
					print_err("no free s2p\n");
				else {
					memset(buf, 0, sizeof(buf));
					snprintf((char *)buf, sizeof(buf), "s_ts%d", i);
					ts->id = i;
					ts->mode = T5D_TS_SERIAL;
					ts->pinctrl = devm_pinctrl_get_select(dev, buf);
					ts->s2p_id = s2p_id++;
				}
			} else if (!strcmp(str, "parallel")) {
				print_dbg("%s: parallel\n", buf);
				memset(buf, 0, sizeof(buf));
				snprintf((char *)buf, sizeof(buf), "p_ts%d", i);
				ts->id = i;
				ts->mode = T5D_TS_PARALLEL;
				ts->pinctrl = devm_pinctrl_get_select(dev, buf);

			} else {
				ts->mode = T5D_TS_DISABLE;
				ts->pinctrl = NULL;
			}
		}
		memset(buf, 0, sizeof(buf));
		snprintf((char *)buf, sizeof(buf), "ts%d_control", i);
		ret = of_property_read_u32(dev->of_node, buf, &value);
		if (!ret) {
			print_err("%s: %#x\n", buf, value);
			ts->control = value;
		} else {
			print_err("read error: %s: %#x\n", buf, value);
		}

		memset(buf, 0, sizeof(buf));
		snprintf((char *)buf, sizeof(buf), "ts%d_csa_enable", i);
		ret = of_property_read_u32(dev->of_node, buf, &value);
		if (!ret) {
			print_err("%s: %#x\n", buf, value);
			ts->csa_enable = value;
		} else {
			print_err("read error: %s: %#x\n", buf, value);
		}

		if (ts->s2p_id != -1) {
			memset(buf, 0, sizeof(buf));
			snprintf(buf, sizeof(buf), "ts%d_invert", i);
			ret = of_property_read_u32(dev->of_node, buf, &value);
			if (!ret) {
				print_err("%s: %#x\n", buf, value);
				t5d_s2p[ts->s2p_id].invert = value;
			}
		}
	}
#if 0
	//memcpy(&t5d_ts_input[1], &t5d_ts_input[2], sizeof(struct t5d_ts_in));
	//t5d_ts_input[1].id = 1;
	t5d_ts_input[1].csa_enable = 0;
	t5d_ts_input[2].csa_enable = 1;
#endif

	return 0;
}

static int
t5d_write_check_flow_control(int dmx_id, int percentage)
{
	int id;
	int used_space, threshold;
	struct t5d_video *dmx_video = NULL;
	struct dvb_demux_feed *feed;
	struct dmxdev_filter *filter;
	struct dvb_demux *demux = (struct dvb_demux *)&t5d_sw_demuxes[dmx_id].demux;

	mutex_lock(&t5d_mutex);
	list_for_each_entry(feed, &demux->feed_list, list_head) {
		print_dbg("feed->type: %d, pes_type: %d\n", feed->type, feed->pes_type);
		if (feed->state != DMX_STATE_GO)
			continue;

		if (feed->type == DMX_TYPE_TS) {
			filter = feed->feed.ts.priv;
			if ((feed->pes_type == DMX_PES_VIDEO0)
			    || (feed->pes_type == DMX_PES_VIDEO1)
			    || (feed->pes_type == DMX_PES_VIDEO2)
			    || (feed->pes_type == DMX_PES_VIDEO3)) {
				/* check video phy buffer status*/
				id = pes_type2id(feed->pes_type);
				dmx_video = &t5d_sw_demuxes[dmx_id].videos[id];
				threshold = dmx_video->len * percentage / 100;
				if (dmx_video->w_offset >= dmx_video->r_offset) {
					used_space = dmx_video->w_offset - dmx_video->r_offset;
				} else {
					used_space = dmx_video->len - dmx_video->r_offset + dmx_video->w_offset;
				}

			} else if ((feed->pes_type == DMX_PES_AUDIO0)
				   || (feed->pes_type == DMX_PES_AUDIO1)
				   || (feed->pes_type == DMX_PES_AUDIO2)
				   || (feed->pes_type == DMX_PES_AUDIO3)) {
				/* check audio ringbuffer status*/
				used_space = filter->buffer.size -
					     dvb_ringbuffer_free(&filter->buffer);
				threshold = filter->buffer.size * percentage / 100;
			} else
				continue;

			if (used_space >= threshold) {
				print_err("trigger flowcontrol, used_space:%#x, threshold:%#x\n",
					  used_space, threshold);
				mutex_unlock(&t5d_mutex);
				return -1;
			}
		}
	}

	mutex_unlock(&t5d_mutex);
	return 0;
}

/*Write TS to decoder.*/
static int
dmx_write(struct dmx_demux *demux, const char __user *buf, size_t count)
{
	struct t5d_sw_demux *dvbdemux = (struct t5d_sw_demux *)demux;
	int dmx_id = dvbdemux - t5d_sw_demuxes;
	enum t5d_demux_source source = t5d_get_demux_source(dmx_id);
	int write_each_len = 188 * 6 * 20;
	size_t left = count;
	char *p = (char *)buf;
	void *write_buf = NULL;
	struct dvb_demux_feed *feed = NULL;

	print_ver("called. buf: %#x, count: %#x\n", buf, count);

	if (t5d_sw_demuxes[dmx_id].buf == NULL) {
		t5d_sw_demuxes[dmx_id].buf = vmalloc(write_each_len);
	}
	write_buf = t5d_sw_demuxes[dmx_id].buf;
	if (write_buf == NULL) {
		print_err("vmalloc failed\n");
		return -ENOMEM;
	}

	while (left > 0) {
		if (t5d_write_check_flow_control(dmx_id, flow_control)) {
			print_err("it triggered demux flow control\n");
			return count - left;
		}

		if (left < write_each_len)
			write_each_len = left;

		if (copy_from_user(write_buf, p, write_each_len))
			return -EFAULT;

		feed = t5d_dump_get_input_feed(source);
		if (feed)
			feed->cb.ts(write_buf, write_each_len, NULL, 0, &feed->feed.ts, NULL);

		t5d_process_stream(source, write_buf, write_each_len);
		p += write_each_len;
		left -= write_each_len;
	}

	return count;
}

static ssize_t ts_setting_show(struct class *class,
			       struct class_attribute *attr, char *buf)
{
	int i;
	int r, total = 0;
	struct t5d_ts_in *ts;

	r = sprintf(buf, "tsin:\n");
	for (i = 0; i < T5D_TS_IN_COUNT; i++) {
		ts = &t5d_ts_input[i];
		r = sprintf(buf, "tsin%d %s control: %#x\n",
			    i, (ts->mode == T5D_TS_DISABLE) ? "disable" :
			    (ts->mode == T5D_TS_PARALLEL ? "parallel" :
			     "serial"), ts->control);
		buf += r;
		total += r;
	}

	return total;
}

static ssize_t ts_setting_store(struct class *class,
				struct class_attribute *attr,
				const char *buf, size_t count)
{
	print_dbg("%s. TODO:\n", __func__);
	return count;
}

static ssize_t get_pcr_show(struct class *class,
			    struct class_attribute *attr, char *buf)
{
	print_dbg("%s. TODO:\n", __func__);
	return 0;
}

static ssize_t get_pcr_store(struct class *class,
			     struct class_attribute *attr,
			     const char *buf, size_t count)
{
	print_dbg("%s. TODO:\n", __func__);
	return 0;
}

static ssize_t dmx_setting_show(struct class *class,
				struct class_attribute *attr, char *buf)
{
	int i;
	int r, total = 0;
	struct dvb_demux *demux = NULL;
	enum t5d_demux_source source;

	for (i = 0; i < T5D_DEMUX_PER_DEVICE; i++) {
		demux = (struct dvb_demux *)&t5d_sw_demuxes[i];

		//if (list_empty(&demux->feed_list))
		//	continue;
		r = sprintf(buf, "dmx%d input: ", i);
		buf += r;
		total += r;

		source = t5d_get_demux_source(i);
		if (source >= T5D_DMA_0 && source <= T5D_DMA_7)
			r = sprintf(buf, "T5D_DMA_%d\n", source);
		else if (source >= T5D_FRONTEND_TS0 && source <= T5D_FRONTEND_TS7)
			r = sprintf(buf, "T5D_FRONTEND_TS%d\n", source - T5D_FRONTEND_TS0);

		buf += r;
		total += r;
	}

	return total;
}

static ssize_t dsc_setting_show(struct class *class,
				struct class_attribute *attr, char *buf)
{
	int total = 0;

	total = t5d_dump_ca_info(buf);

	return total;
}

static ssize_t dmx_ver_show(struct class *class,
			    struct class_attribute *attr, char *buf)
{
	print_dbg("%s. todo:\n", __func__);
	return 0;
}

static ssize_t tso_source_show(struct class *class,
			       struct class_attribute *attr, char *buf)
{
	int r, total = 0;
	u32 val = 0;

	val = t5d_hw_dmx_get_tso();
	r = sprintf(buf, "tso source:%d\n", val);
	buf += r;
	total += r;

	return total;
}

static ssize_t tso_source_store(struct class *class,
				struct class_attribute *attr,
				const char *buf, size_t count)
{
	unsigned int value = 0;

	if (buf[0] == '0') {
		value = 0;
		print_dbg("value:%#x\n", value);
	} else if (buf[0] == '1') {
		value = 1;
		print_dbg("value:%#x\n", value);
	}

	t5d_hw_dmx_set_tso(value);
	return count;
}

static CLASS_ATTR_RW(ts_setting);
static CLASS_ATTR_RW(get_pcr);
static CLASS_ATTR_RO(dmx_setting);
static CLASS_ATTR_RO(dsc_setting);
static CLASS_ATTR_RO(dmx_ver);
static CLASS_ATTR_RW(tso_source);

static struct attribute *t5d_stb_class_attrs[] = {
	&class_attr_ts_setting.attr,
	&class_attr_get_pcr.attr,
	&class_attr_dmx_setting.attr,
	&class_attr_dsc_setting.attr,
	&class_attr_dmx_ver.attr,
	&class_attr_tso_source.attr,
	NULL
};

ATTRIBUTE_GROUPS(t5d_stb_class);

static struct class t5d_stb_class = {
		.name = "stb",
		.class_groups = t5d_stb_class_groups
	};


static int reg_addr;
static ssize_t register_addr_show(struct class *class,
				  struct class_attribute *attr, char *buf)
{
	int ret;

	ret = sprintf(buf, "%x\n", reg_addr);
	return ret;
}

static ssize_t register_addr_store(struct class *class,
				   struct class_attribute *attr,
				   const char *buf, size_t size)
{
	int addr = 0;
	long value = 0;

	if (kstrtol(buf, 0, &value) == 0)
		addr = (int)value;
	reg_addr = addr;
	return size;
}

static ssize_t register_value_show(struct class *class,
				   struct class_attribute *attr, char *buf)
{
	int ret, value;

	value = READ_MPEG_REG(reg_addr);
	ret = sprintf(buf, "%#x\n", value);

	return ret;
}

static ssize_t register_value_store(struct class *class,
				    struct class_attribute *attr,
				    const char *buf, size_t size)
{
	u32 value = 0;
	long long val = 0;

	if (kstrtoll(buf, 0, &val) == 0)
		value = (u32)val;
	WRITE_MPEG_REG(reg_addr, value);

	return size;
}

static ssize_t dump_filter_show(struct class *class,
				struct class_attribute *attr, char *buf)
{
	int i;
	int r;
	int idx = 0;
	int id = -1;
	ssize_t size = 0;
	struct dvb_demux_feed *feed;
	struct dmxdev_filter *filter;
	struct t5d_video *dmx_video = NULL;
	struct dvb_demux *demux = NULL;

	mutex_lock(&t5d_mutex);
	r = sprintf(buf, "********dvr********\n");
	buf += r;
	size += r;

	for (i = 0; i < T5D_DEMUX_PER_DEVICE; i++) {
		demux = (struct dvb_demux *)&t5d_sw_demuxes[i];

		if (list_empty(&demux->feed_list))
			continue;
		r = sprintf(buf, "dmx_id %d\n", i);
		buf += r;
		size += r;

		list_for_each_entry(feed, &demux->feed_list, list_head) {
			print_dbg(buf, "feed->type: %d, ts_type: %d\n", feed->type, feed->ts_type);
			if (feed->state != DMX_STATE_GO)
				continue;

			if (feed->type == DMX_TYPE_TS &&
			    (feed->ts_type & (TS_PACKET | TS_DEMUX)) == TS_PACKET) {
				r = sprintf(buf, "%d pid:%#x\n", idx++, feed->pid);
				buf += r;
				size += r;
			}
		}
	}

	r = sprintf(buf, "********PES********\n");
	buf += r;
	size += r;

	idx = 0;
	for (i = 0; i < T5D_DEMUX_PER_DEVICE; i++) {
		demux = (struct dvb_demux *)&t5d_sw_demuxes[i];

		if (list_empty(&demux->feed_list))
			continue;
		r = sprintf(buf, "dmx_id %d\n", i);
		buf += r;
		size += r;

		list_for_each_entry(feed, &demux->feed_list, list_head) {
			print_dbg("feed->type: %d, pes_type: %d\n", feed->type, feed->pes_type);
			if (feed->state != DMX_STATE_GO || feed->type != DMX_TYPE_TS)
				continue;

			if (feed->pes_type != DMX_PES_AUDIO0 &&
			    feed->pes_type != DMX_PES_AUDIO1 &&
			    feed->pes_type != DMX_PES_AUDIO2 &&
			    feed->pes_type != DMX_PES_AUDIO3 &&
			    feed->pes_type != DMX_PES_VIDEO0 &&
			    feed->pes_type != DMX_PES_VIDEO1 &&
			    feed->pes_type != DMX_PES_VIDEO2 &&
			    feed->pes_type != DMX_PES_VIDEO3 &&
			    feed->pes_type != DMX_PES_PCR0 &&
			    feed->pes_type != DMX_PES_PCR1 &&
			    feed->pes_type != DMX_PES_PCR2 &&
			    feed->pes_type != DMX_PES_PCR3) {
				r = sprintf(buf, "%d pid:%#x\n", idx++, feed->pid);
				buf += r;
				size += r;
			}
		}
	}

	r = sprintf(buf, "********ES********\n");
	buf += r;
	size += r;
	idx = 0;
	for (i = 0; i < T5D_DEMUX_PER_DEVICE; i++) {
		demux = (struct dvb_demux *)&t5d_sw_demuxes[i];

		if (list_empty(&demux->feed_list))
			continue;
		r = sprintf(buf, "dmx_id %d\n", i);
		buf += r;
		size += r;

		list_for_each_entry(feed, &demux->feed_list, list_head) {
			print_dbg(buf, "feed->type: %d, pes_type: %d\n", feed->type, feed->pes_type);
			if (feed->state != DMX_STATE_GO || feed->type != DMX_TYPE_TS)
				continue;

			id = pes_type2id(feed->pes_type);
			if (id < 0)
				continue;

			filter = feed->feed.ts.priv;
			if (!(filter->params.pes.flags & DMX_ES_OUTPUT))
				continue;

			print_dbg("i %d, id %d, pes_type %d\n", i, id, feed->pes_type);
			if (feed->pes_type == DMX_PES_VIDEO0
			    || feed->pes_type == DMX_PES_VIDEO1
			    || feed->pes_type == DMX_PES_VIDEO2
			    || feed->pes_type == DMX_PES_VIDEO3) {
				dmx_video = &t5d_sw_demuxes[i].videos[id];
				r = sprintf(buf, "pid:%#x type:video addr:%#x len:%#x rp:%#x wp:%#x\n",
					    feed->pid, dmx_video->phys, dmx_video->len,
					    dmx_video->r_offset, dmx_video->w_offset);
			} else if (feed->pes_type == DMX_PES_PCR0
				   || feed->pes_type == DMX_PES_PCR1
				   || feed->pes_type == DMX_PES_PCR2
				   || feed->pes_type == DMX_PES_PCR3) {
				struct t5d_pcr *slot;
				int pcr_id;

				pcr_id = pes_type2id(feed->pes_type);
				if (pcr_id < 0)
					continue;

				slot = &t5d_sw_demuxes[i].pcrs[pcr_id];
				r = sprintf(buf, "pid:%#x type:pcr value:%#x\n",
					    feed->pid, slot->last_pcr);
			} else {
				r = sprintf(buf, "pid:%#x type:audio addr:%#x len:%#x rp:%#x wp:%#x\n",
					    feed->pid,
					    filter->buffer.data, filter->buffer.size,
					    filter->buffer.pread, filter->buffer.pwrite);
			}
			buf += r;
			size += r;
		}
	}

	r = sprintf(buf, "********section********\n");
	buf += r;
	size += r;
	idx = 0;
	for (i = 0; i < T5D_DEMUX_PER_DEVICE; i++) {
		demux = (struct dvb_demux *)&t5d_sw_demuxes[i];

		if (list_empty(&demux->feed_list))
			continue;
		r = sprintf(buf, "dmx_id %d\n", i);
		buf += r;
		size += r;

		list_for_each_entry(feed, &demux->feed_list, list_head) {
			print_dbg(buf, "feed->type: %d\n", feed->type);
			if (feed->state != DMX_STATE_GO || feed->type != DMX_TYPE_SEC)
				continue;

			filter = feed->filter->filter.priv;
			r = sprintf(buf, "pid:%#x addr:%#x len:%#x rp:%#x wp:%#x\n",
				    feed->pid, filter->buffer.data, filter->buffer.size,
				    filter->buffer.pread, filter->buffer.pwrite);
			buf += r;
			size += r;
		}
	}
	mutex_unlock(&t5d_mutex);
	return size;
}

static ssize_t dump_filter_store(struct class *class,
				 struct class_attribute *attr,
				 const char *buf, size_t size)
{
	print_dbg("%s. todo:\n", __func__);
	return size;
}

static ssize_t dmx_source_show(struct class *class,
			       struct class_attribute *attr, char *buf)
{
	print_dbg("%s. todo:\n", __func__);
	return 0;
}

static ssize_t dmx_source_store(struct class *class,
				struct class_attribute *attr,
				const char *buf, size_t count)
{
	print_dbg("%s. todo:\n", __func__);
	return 0;
}

static CLASS_ATTR_RW(register_addr);
static CLASS_ATTR_RW(register_value);
static CLASS_ATTR_RW(dump_filter);
static CLASS_ATTR_RW(dmx_source);

static struct attribute *t5d_dmx_class_attrs[] = {
	&class_attr_register_addr.attr,
	&class_attr_register_value.attr,
	&class_attr_dump_filter.attr,
	&class_attr_dmx_source.attr,
	NULL
};

ATTRIBUTE_GROUPS(t5d_dmx_class);

static struct class t5d_dmx_class = {
		.name = "dmx",
		.class_groups = t5d_dmx_class_groups
	};

/*Destroy the DVB adaptor.*/
static int
t5d_dvb_remove(struct platform_device *pdev)
{
	int i;
	struct dvb_adapter *padapter;
	print_dbg("%s enter\n", __func__);
	padapter = aml_dvb_get_adapter(&pdev->dev);

	t5d_asyncfifo_deinit();

	for (i = 0; i < T5D_DEMUX_PER_DEVICE; i++) {
		struct t5d_sw_demux *sw_dmx = &t5d_sw_demuxes[i];
		//struct dvb_device *dvbdev = (struct dvb_device *)&sw_dmx->dmxdev;
		t5d_dsc_deinit(&sw_dmx->dsc);
	}

	t5d_hw_dmx_deinit();
	t5d_deinit_device();
	t5d_key_deinit();

	class_unregister(&t5d_stb_class);
	class_unregister(&t5d_dmx_class);

	return 0;
}

static int
t5d_dvb_probe(struct platform_device *pdev)
{
	int i, j = 0, r;
	struct dvb_adapter *padapter;
	struct dmx_demux_ext *dmx = NULL;
	struct dmxdev *dmxdev = NULL;

	print_dbg("probe t5d dvb driver [%s].\n", DVB_VERSION);
	padapter = aml_dvb_get_adapter(&pdev->dev);

	mutex_init(&t5d_mutex);
	/*Create demux devices.*/
	for (i = 0; i < T5D_DEMUX_PER_DEVICE; i ++) {
		dmx    = &t5d_sw_demuxes[i].demux;
		dmxdev = &t5d_sw_demuxes[i].dmxdev;

		dmx->dmx.capabilities = (DMX_TS_FILTERING
					 | DMX_SECTION_FILTERING
					 | DMX_MEMORY_BASED_FILTERING);
		dmx->dvbdmx.filternum = T5D_FILTER_PER_DEMUX;
		dmx->dvbdmx.feednum   = T5D_FILTER_PER_DEMUX;
		dmx->dvbdmx.start_feed = dmx_start_feed;
		dmx->dvbdmx.stop_feed  = dmx_stop_feed;
		dmx->dmx.get_stc = dmx_get_stc;

		if ((r = dvb_dmx_init(&dmx->dvbdmx)) < 0)
			goto ERR;

		dmx->set_input = dmx_set_input;
		dmx->set_hw_source = dmx_set_hw_source;
		dmx->get_hw_source = dmx_get_hw_source;
		dmx->get_sec_mem_info = get_sec_mem_info;
		dmx->get_ts_mem_info = get_ts_mem_info;
		dmx->get_dmx_mem_info = get_dmx_mem_info;
		dmx->decode_info = dmx_decode_info;
		dmx->dmx.write = dmx_write;

		dmxdev->filternum = dmx->dvbdmx.feednum;
		dmxdev->demux	  = &dmx->dmx;
		dmxdev->capabilities = 0;
		if ((r = dvb_dmxdev_init(dmxdev, padapter)) < 0)
			goto ERR;
	}

	/*Init the hardware demux*/
	t5d_hw_dmx_init();
	t5d_hwdmx_desc_init();

	/*Init the software demux*/
	t5d_init_device();

	/*Parse ts input parameters*/
	t5d_ts_input_parse(&pdev->dev);

	for (i = 0; i < T5D_TS_IN_COUNT && j < T5D_ASYNC_FIFO_COUNT; i++) {
		if (t5d_ts_input[i].mode != T5D_TS_DISABLE) {
			/*Init the hardware demux*/
			t5d_hw_dmx_id_init(j, &t5d_ts_input[i], &t5d_s2p[0]);

			/*Init the hardware asyncfifo*/
			r = t5d_asyncfifo_init(pdev, j, T5D_FRONTEND_TS0 + t5d_ts_input[i].id);
			if (r < 0)
				goto ERR;
			j++;
		}
	}

	for (i = 0; i < T5D_DEMUX_PER_DEVICE; i++) {
		struct t5d_sw_demux *sw_dmx = &t5d_sw_demuxes[i];
		dmx = &t5d_sw_demuxes[i].demux;

		memset(&sw_dmx->dsc, 0, sizeof(struct t5d_sw_dsc));
		sw_dmx->dsc.id = i;
		t5d_dsc_init(pdev, &sw_dmx->dsc);

		for (j = 0; j < T5D_HW_STREAM_NUM; j++) {
			sw_dmx->hw_fes[j].source = DMX_FRONTEND_0 + j;
			r = dmx->dmx.add_frontend(&sw_dmx->demux.dmx, &sw_dmx->hw_fes[j]);
			if (r)
				print_err("adding hw frontend to dmx failed %d\n", r);
		}

		sw_dmx->mem_fe.source = DMX_MEMORY_FE;
		r = dmx->dmx.add_frontend(&dmx->dmx, &sw_dmx->mem_fe);
		if (r)
			print_err("adding mem frontend to dmx failed %d\n", r);

		r = dmx->dmx.connect_frontend(&dmx->dmx, &sw_dmx->hw_fes[0]);
		if (r)
			print_err("connect frontend failed %d\n", r);
	}

	t5d_key_init();

	class_register(&t5d_stb_class);
	class_register(&t5d_dmx_class);
	return 0;

ERR:
	t5d_dvb_remove(pdev);
	return r;
}

#ifdef CONFIG_OF
static const struct of_device_id t5d_dvb_dt_match[] = {
	{
		.compatible = "amlogic, dvb-demux",
	},
	{},
};
#endif

struct platform_driver t5d_dvb_driver = {
	.probe = t5d_dvb_probe,
	.remove = t5d_dvb_remove,
	.suspend = NULL,
	.resume = NULL,
	.driver = {
		.name = "t5d-dvb-demux",
		.owner = THIS_MODULE,
#ifdef CONFIG_OF
		.of_match_table = t5d_dvb_dt_match,
#endif
	}
};

static int __init
dvb_init(void)
{
	print_dbg("T5D DVB initialize");
	return platform_driver_register(&t5d_dvb_driver);
}

static void __exit
dvb_exit(void)
{
	print_dbg("T5D DVB release\n");
	platform_driver_unregister(&t5d_dvb_driver);
}

module_init(dvb_init);
module_exit(dvb_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("T5D demux module");
MODULE_ALIAS("T5D demux module");
