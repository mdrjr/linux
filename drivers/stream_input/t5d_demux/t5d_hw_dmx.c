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
#include <linux/wait.h>
#include <linux/string.h>
#include <linux/delay.h>
#include <linux/vmalloc.h>
#include <linux/kthread.h>
#include <linux/platform_device.h>
#include <linux/dma-mapping.h>
#include <linux/amlogic/media/registers/cpu_version.h>
#include <linux/dvb/aml_ca_ext.h>
#include <media/dvb_demux.h>
#include "../../../drivers/common/media_clock/switch/amports_gate.h"

#include "c_stb_define.h"
#include "c_stb_regs_define.h"
#include "t5d_dvb_reg.h"
#include "t5d_hw_dsc.h"
#include "t5d_demux.h"
#include "t5d_hw_dmx.h"

#define print_err(fmt, args...)   \
	dprintk(LOG_ERROR, debug_hw_dmx, fmt, ## args)
#define print_dbg(fmt, args...)   \
	dprintk(LOG_DBG, debug_hw_dmx, fmt, ## args)
#define print_ver(fmt, args...)   \
	dprintk(LOG_VER, debug_hw_dmx, fmt, ## args)

MODULE_PARM_DESC(debug_hw_dmx, "\n\t\t Enable hardware demux debug information");
static int debug_hw_dmx;
module_param(debug_hw_dmx, int, 0644);

#define ASYNCFIFO_TIMER	(5)
#define ASYNCFIFO_BUFFER_SIZE_DEFAULT (512 * 1024)

struct t5d_dump_ts {
	int source; /*t5d_demux_source type*/
	int type;	/* dump type*/
	struct dvb_demux_feed *feed;
	struct list_head node;
};

LIST_HEAD(t5d_dump_ts_list);

static struct t5d_hwdmx_descchannel
	t5d_descchannels[T5D_HW_DMX_COUNT][T5D_DSC_PER_DEMUX];
static struct t5d_asyncfifo t5d_asyncfifos[T5D_ASYNC_FIFO_COUNT];
struct task_struct *afifo_thread[T5D_ASYNC_FIFO_COUNT] = {NULL, NULL};

//#undef WRITE_MPEG_REG(r, v)	dmx_write_reg(r, v)
#define DMX_READ_REG(i, r)\
	((i)?((i == 1)?READ_MPEG_REG(r##_2) :\
	READ_MPEG_REG(r##_3)) : READ_MPEG_REG(r))

#define DMX_WRITE_REG(i, r, d)\
	do {\
		if (i == 1) {\
			WRITE_MPEG_REG(r##_2, d);\
		} else if (i == 2) {\
			WRITE_MPEG_REG(r##_3, d);\
		} else {\
			WRITE_MPEG_REG(r, d);\
		}\
	} while (0)

#define READ_PERI_REG		READ_CBUS_REG
#define WRITE_PERI_REG		WRITE_CBUS_REG
#define READ_ASYNC_FIFO_REG(i, r)\
	((i) ? ((i-1)?READ_PERI_REG(ASYNC_FIFO1_##r):\
	READ_PERI_REG(ASYNC_FIFO2_##r)) : READ_PERI_REG(ASYNC_FIFO_##r))

#define WRITE_ASYNC_FIFO_REG(i, r, d)\
	do {\
		if (i == 2) {\
			WRITE_CBUS_REG(ASYNC_FIFO1_##r, d);\
		} else if (i == 0) {\
			WRITE_CBUS_REG(ASYNC_FIFO_##r, d);\
		} else {\
			WRITE_CBUS_REG(ASYNC_FIFO2_##r, d);\
		}\
	} while (0)

#define CLEAR_ASYNC_FIFO_REG_MASK(i, reg, mask)\
	WRITE_ASYNC_FIFO_REG(i, reg,\
	(READ_ASYNC_FIFO_REG(i, reg) & (~(mask))))
#define DEMUX_INT_MASK\
			((0<<(AUDIO_SPLICING_POINT))    |\
			(0<<(VIDEO_SPLICING_POINT))     |\
			(1<<(OTHER_PES_READY))          |\
			(1<<(PCR_READY))                |\
			(1<<(SUB_PES_READY))            |\
			(1<<(SECTION_BUFFER_READY))     |\
			(0<<(OM_CMD_READ_PENDING))      |\
			(1<<(TS_ERROR_PIN))             |\
			(1<<(NEW_PDTS_READY))           |\
			(0<<(DUPLICATED_PACKET))        |\
			(0<<(DIS_CONTINUITY_PACKET)))
long
aml_stb_get_base(int id)
{
	int newbase = 0;
	if (MESON_CPU_MAJOR_ID_TXL < get_cpu_type()
	    && MESON_CPU_MAJOR_ID_GXLX != get_cpu_type()) {
		newbase = 1;
	}

	switch (id) {
	case ID_STB_CBUS_BASE:
		return (newbase) ? 0x1800 : 0x1600;
	case ID_SMARTCARD_REG_BASE:
		return (newbase) ? 0x9400 : 0x2110;
	case ID_ASYNC_FIFO_REG_BASE:
		return (newbase) ? 0x2800 : 0x2310;
	case ID_ASYNC_FIFO1_REG_BASE:
		return 0x9800;
	case ID_ASYNC_FIFO2_REG_BASE:
		return (newbase) ? 0x2400 : 0x2314;
	case ID_RESET_BASE:
		return (newbase) ? 0x0400 : 0x1100;
	case ID_PARSER_SUB_START_PTR_BASE:
		return (newbase) ? 0x3800 : 0x2900;
	default:
		return 0;
	}

	return 0;
}
int
t5d_set_chan_regs(struct t5d_asyncfifo *afifo, int pid)
{
	u32 data, addr, advance;
	int cid = 5;

	while (DMX_READ_REG(afifo->hw_dmx_id, FM_WR_ADDR) & 0x8000)
		udelay(1);

	advance = 0;
	addr = cid >> 1;
	data = (((RECORDER_STREAM << PID_TYPE) | pid) << 16) |
	       ((0x7 << PID_TYPE) | 0x1FFE);

	DMX_WRITE_REG(afifo->hw_dmx_id, FM_WR_DATA, data);
	DMX_WRITE_REG(afifo->hw_dmx_id, FM_WR_ADDR, (advance << 16) | 0x8000 | addr);

	data = DMX_READ_REG(afifo->hw_dmx_id, MAX_FM_COMP_ADDR) & 0xF0;
	DMX_WRITE_REG(afifo->hw_dmx_id, MAX_FM_COMP_ADDR, data | (0 >> 1));

	if (DMX_READ_REG(afifo->hw_dmx_id, OM_CMD_STATUS) & 0x8e00) {
		print_err("warning: send cmd %x\n", DMX_READ_REG(afifo->hw_dmx_id, OM_CMD_STATUS));
	}

	return 0;
}

/**
 * Initialize the hardware demux device.
 * @retval 0  On success.
 * @retval -1 On error.
 */
int
t5d_hw_dmx_init(void)
{
	int i;
	int times = 0;
	u32 data;

	amports_switch_gate("demux", 1);
	amports_switch_gate("ahbarb0", 1);
	amports_switch_gate("parser_top", 1);

	WRITE_MPEG_REG(RESET1_REGISTER, RESET_DEMUXSTB);
	for (i = 0; i < T5D_HW_DMX_COUNT; i++) {
		while (times++ < 1000000) {
			if (!(DMX_READ_REG(i, OM_CMD_STATUS) & 0x01))
				break;
		}
	}

	WRITE_MPEG_REG(STB_TOP_CONFIG, 0);
	WRITE_MPEG_REG(STB_S2P2_CONFIG, 0);

#if 1 // ci module needs ts_out_invert?
	data = READ_MPEG_REG(TS_TOP_CONFIG);
	data |= 1 << TS_OUT_CLK_INVERT;
	WRITE_MPEG_REG(TS_TOP_CONFIG, data);
#endif

	/*invert ts out clk end*/
	WRITE_MPEG_REG(TS_FILE_CONFIG,
		       (0 << 16) |
		       (6 << DES_OUT_DLY) |
		       (3 << TRANSPORT_SCRAMBLING_CONTROL_ODD) |
		       (3 << TRANSPORT_SCRAMBLING_CONTROL_ODD_2) |
		       (0 << TS_HIU_ENABLE) |
		       (5 << FEC_FILE_CLK_DIV));

	/*Reset asyncfifo*/
	WRITE_MPEG_REG(RESET6_REGISTER, (1 << 11) | (1 << 12));

	return 0;
}

/**
 * Add the dump stream id
 * @param source The input source type
 * @param feed The dvb_demux_feed
 * @param dump_type The dump type
 * @retval 0 On success
 * @retval -1 On error
 */
int
t5d_dump_add_sid(int source, struct dvb_demux_feed *feed, int dump_type)
{
	struct t5d_dump_ts *d_node = NULL;

	d_node = kmalloc(sizeof(*d_node), GFP_KERNEL);
	if (!d_node) {
		print_err("error, no memory\n");
		return -1;
	}

	memset(d_node, 0, sizeof(struct t5d_dump_ts));
	INIT_LIST_HEAD(&d_node->node);

	d_node->source = source;
	d_node->type = dump_type;
	d_node->feed = feed;
	list_add(&d_node->node, &t5d_dump_ts_list);
	print_dbg("add dump ts, feed:%#x, source:%#x, type:%#x\n",
		  feed, source, dump_type);

	return 0;
}

/**
 * Remove the dump stream id
 * @param source The input source type
 * @param cb The ts feed cb
 * @retval 0 On success
 * @retval -1 On error
 */
int
t5d_dump_remove_sid(int source, struct dvb_demux_feed *feed)
{
	struct t5d_dump_ts *d_entry = NULL;
	struct t5d_dump_ts *d_tmp = NULL;

	list_for_each_entry_safe(d_entry, d_tmp, &t5d_dump_ts_list, node) {
		if (d_entry->source == source
		    && d_entry->feed == feed) {
			list_del(&d_entry->node);
			print_dbg("remove dump ts feed:%#x\n", feed);
		}
	}

	return 0;
}

/**
 * Get the dump input feed
 * @param source The input source type
 * @retval feed On success
 * @retval NULL On error
 */
struct dvb_demux_feed *
t5d_dump_get_input_feed(int source)
{
	struct t5d_dump_ts *d_entry = NULL;
	struct t5d_dump_ts *d_tmp = NULL;

	list_for_each_entry_safe(d_entry, d_tmp, &t5d_dump_ts_list, node) {
		print_dbg("dump entry source:%#x, type:%#x, feed:%#x\n",
			  d_entry->source, d_entry->type, d_entry->feed);
		if (d_entry->source == source
		    && d_entry->type == DMX_DUMP_INPUT_TYPE
		    && d_entry->feed != NULL) {
			return d_entry->feed;
		}
	}

	return 0;
}

/**
 * Initialize the hardware demux device.
 * @param hw_dmx_id Hardware demux device id
 * @param ts_inputs Hardware TS inputs array
 * @param s2ps Hardware s2p array
 * @retval 0  On success.
 * @retval -1 On error.
 */
int
t5d_hw_dmx_id_init(int hw_dmx_id,
		   struct t5d_ts_in *ts_input,
		   struct t5d_s2p *s2ps)
{
	u32 version, data;

	int fec_core_sel = 0;
	int dec_clk_en = 0, des_in = 0, en_des = 0, des_out = 0;
	int out_src, fec_clk, fec_sel;
	int i;
	u32 fec_s0, fec_s1, fec_s2;
	u32 invert0, invert1, invert2;
	static int tsfile_clkdiv = 5;

	int dump_ts_select = 1;
	int keep_duplicate_packet = 1;

	DMX_WRITE_REG(hw_dmx_id, DEMUX_CONTROL, 0x0000);

	version = DMX_READ_REG(hw_dmx_id, STB_VERSION);
	DMX_WRITE_REG(hw_dmx_id, STB_TEST_REG, version);
	print_dbg("STB %d hardware version: %d\n", hw_dmx_id, version);
	DMX_WRITE_REG(hw_dmx_id, STB_TEST_REG, 0x5550);
	data = DMX_READ_REG(hw_dmx_id, STB_TEST_REG);
	if (data != 0x5550)
		print_err("STB %d register access failed\n", hw_dmx_id);

	DMX_WRITE_REG(hw_dmx_id, STB_TEST_REG, 0xaaa0);
	data = DMX_READ_REG(hw_dmx_id, STB_TEST_REG);
	if (data != 0xaaa0)
		print_err("STB %d register access failed\n", hw_dmx_id);
	DMX_WRITE_REG(hw_dmx_id, MAX_FM_COMP_ADDR, 0x0000);
	DMX_WRITE_REG(hw_dmx_id, STB_INT_MASK, 0);
	DMX_WRITE_REG(hw_dmx_id, STB_INT_STATUS, 0xffff);

	fec_sel = ts_input->id;
	/* config dsc0 by STB_TOP_CONFIG*/
	if (hw_dmx_id == 0) {
		des_in = 0;
		en_des = 1;
		dec_clk_en = 1;
		if (ts_input->csa_enable)
			fec_core_sel = 1;
	} else if (hw_dmx_id == 1) {
		des_in = 1;
		en_des = 1;
		des_out = 2;
		if (ts_input->csa_enable)
			fec_core_sel = 1;
		WRITE_MPEG_REG(COMM_DESC_2_CTL,
			       (6 << 8) | /*des_out_dly_2*/
			       ((!!en_des) << 6) | /*des_pl_clk_2*/
			       ((!!en_des) << 5) | /*des_pl_2*/
			       (des_out << 2) | /*use_des_2*/
			       (des_in)/*des_i_sel_2*/
			      );
	}
	DMX_WRITE_REG(hw_dmx_id, FEC_INPUT_CONTROL,
		      (fec_core_sel << FEC_CORE_SEL) |
		      (fec_sel << FEC_SEL));

	/*stb enable*/
	fec_clk = tsfile_clkdiv;
	out_src = ts_input->id;

	fec_s0 = 0;
	fec_s1 = 0;
	fec_s2 = 0;
	invert0 = 0;
	invert1 = 0;
	invert2 = 0;

	for (i = 0; i < T5D_TS_IN_COUNT; i++) {
		if (ts_input->s2p_id == 0)
			fec_s0 = i;
		else if (ts_input->s2p_id == 1)
			fec_s1 = i;
		else if (ts_input->s2p_id == 2)
			fec_s2 = i;
	}
	invert0 = s2ps[0].invert;
	invert1 = s2ps[1].invert;

	WRITE_MPEG_REG(STB_TOP_CONFIG,
		       (invert1 << INVERT_S2P1_FEC_CLK) |
		       (fec_s1 << S2P1_FEC_SERIAL_SEL) |
		       (out_src << TS_OUTPUT_SOURCE) |
		       (des_in << DES_INPUT_SEL) |
		       (en_des << ENABLE_DES_PL) |
		       (dec_clk_en << ENABLE_DES_PL_CLK) |
		       (invert0 << INVERT_S2P0_FEC_CLK) |
		       (fec_s0 << S2P0_FEC_SERIAL_SEL));
	invert2 = s2ps[2].invert;
	WRITE_MPEG_REG(STB_S2P2_CONFIG,
		       (invert2 << INVERT_S2P2_FEC_CLK) |
		       (fec_s2 << S2P2_FEC_SERIAL_SEL));

	/*dmx enable*/
	/*Initialize the registers */
	DMX_WRITE_REG(hw_dmx_id, STB_INT_MASK, DEMUX_INT_MASK);
	DMX_WRITE_REG(hw_dmx_id, DEMUX_MEM_REQ_EN,
		      (1 << SECTION_AHB_DMA_EN) |
		      (0 << SUB_AHB_DMA_EN) |
		      (1 << OTHER_PES_AHB_DMA_EN) |
		      (1 << SECTION_PACKET) |
		      (1 << VIDEO_PACKET) |
		      (1 << AUDIO_PACKET) |
		      (1 << SUB_PACKET) |
		      (1 << SCR_ONLY_PACKET) |
		      (1 << OTHER_PES_PACKET));
	DMX_WRITE_REG(hw_dmx_id, PES_STRONG_SYNC, 0x1234);
	DMX_WRITE_REG(hw_dmx_id, DEMUX_ENDIAN,
		      (1 << SEPARATE_ENDIAN) |
		      (0 << OTHER_PES_ENDIAN) |
		      (7 << SCR_ENDIAN) |
		      (7 << SUB_ENDIAN) |
		      (7 << AUDIO_ENDIAN) |
		      (7 << VIDEO_ENDIAN) |
		      (7 << OTHER_ENDIAN) |
		      (7 << BYPASS_ENDIAN) | (0 << SECTION_ENDIAN));

	DMX_WRITE_REG(hw_dmx_id, STB_OM_CTL,
		      (0x40 << MAX_OM_DMA_COUNT) |
		      (0x7f << LAST_OM_ADDR));

	DMX_WRITE_REG(hw_dmx_id, DEMUX_CONTROL,
		      (0 << BYPASS_USE_RECODER_PATH) |
		      (0 << INSERT_AUDIO_PES_STRONG_SYNC) |
		      (0 << INSERT_VIDEO_PES_STRONG_SYNC) |
		      (0 << OTHER_INT_AT_PES_BEGINNING) |
		      (0 << DISCARD_AV_PACKAGE) |
		      (dump_ts_select << TS_RECORDER_SELECT) |
		      (keep_duplicate_packet << KEEP_DUPLICATE_PACKAGE) |
		      (1 << SECTION_END_WITH_TABLE_ID) |
		      (1 << ENABLE_FREE_CLK_FEC_DATA_VALID) |
		      (1 << ENABLE_FREE_CLK_STB_REG) |
		      (1 << STB_DEMUX_ENABLE) |
		      (0 << NOT_USE_OF_SOP_INPUT));

	return 0;
}

/**
 * Set the hardware demux ts output.
 * @param value The ts out value
 * @retval 0  On success.
 * @retval -1 On error.
 */
int
t5d_hw_dmx_set_tso(unsigned int value)
{
	u32 data;
	int out_src = value;

	data = READ_MPEG_REG(STB_TOP_CONFIG);
	data &= ~(7 << TS_OUTPUT_SOURCE);

	data |= out_src << TS_OUTPUT_SOURCE;

	WRITE_MPEG_REG(STB_TOP_CONFIG, data);

	return 0;
}

/**
 * Get the hardware demux ts output.
 * @retval ts output value.
 */
unsigned int
t5d_hw_dmx_get_tso(void)
{
	u32 data;
	u32 value;

	data = READ_MPEG_REG(STB_TOP_CONFIG);
	value = (data >> TS_OUTPUT_SOURCE);

	return value;
}

/**
 * Release the hardware demux device.
 * @retval 0  On success.
 * @retval -1 On error.
 */
int
t5d_hw_dmx_deinit(void)
{
	print_err("TODO: %s\n", __func__);
	return 0;
}

static int asyncfifo_thread_func(void *data)
{
	u32 start_addr;
	int reg_val, cnt, offset;
	struct t5d_asyncfifo *afifo = (struct t5d_asyncfifo *)data;
	struct t5d_dump_ts *d_entry = NULL;
	struct t5d_dump_ts *d_tmp = NULL;

	while (afifo->init) {
		print_ver("asyncfifo[%d] hw_dmx%d, source:%d\n",
			  afifo->id, afifo->hw_dmx_id, afifo->source);

		reg_val = READ_ASYNC_FIFO_REG(afifo->id, REG0);
		start_addr = virt_to_phys((void *)afifo->pages);
		afifo->w_offset = (reg_val - start_addr);
		if (afifo->w_offset == afifo->r_offset) {
			print_ver("asyncfifo[%d] no new data, wp: %#x\n",
				  afifo->id, afifo->w_offset);
			msleep(ASYNCFIFO_TIMER);
			continue;
		}

		if (afifo->r_offset > afifo->w_offset) {
			cnt = afifo->buf_len - afifo->r_offset;
			offset = afifo->r_offset;
			afifo->r_offset = 0;
		} else if (afifo->w_offset > afifo->r_offset) {
			cnt = afifo->w_offset - afifo->r_offset;
			offset = afifo->r_offset;
			afifo->r_offset = afifo->w_offset;
		}

		print_ver("asyncfifo[%d] wp: %#x, rp: %#x, cnt: %#x\n",
			  afifo->id,
			  afifo->w_offset,
			  afifo->r_offset,
			  cnt);

		dma_sync_single_for_cpu(afifo->dev,
					afifo->pages_map + offset,
					cnt,
					DMA_FROM_DEVICE);

		list_for_each_entry_safe(d_entry, d_tmp, &t5d_dump_ts_list, node) {
			print_ver("dump thread, feed:%#x, source:%#x, type:%#x\n",
				  d_entry->feed, d_entry->source, d_entry->type);
			if (d_entry->source == afifo->source
			    && d_entry->type == DMX_DUMP_TS_TYPE
			    && d_entry->feed != NULL) {
				print_dbg("dump data len: %#x\n", cnt);
				d_entry->feed->cb.ts((u8 *)(afifo->pages + offset), cnt, NULL, 0, &d_entry->feed->feed.ts, NULL);
			}
		}

		t5d_process_stream(afifo->source,
				   (u8 *)(afifo->pages + offset),
				   cnt);
		msleep(ASYNCFIFO_TIMER);
	}

	print_dbg("asyncfifo thread exiting\n");
	return 0;
}

int
async_fifo_set_regs(struct t5d_asyncfifo *afifo, int source_val)
{
	u32 factor = 1;
	int len = afifo->buf_len;

	print_dbg("pages: %#x, phys: %#x\n", afifo->pages, virt_to_phys((void *)afifo->pages));
	/*Destination address*/
	WRITE_ASYNC_FIFO_REG(afifo->id, REG0, virt_to_phys((void *)afifo->pages));

	/*Setup flush parameters*/
	WRITE_ASYNC_FIFO_REG(afifo->id, REG1,
			     (0 << ASYNC_FIFO_TO_HIU) |
			     (0 << ASYNC_FIFO_FLUSH) |
			     /*don't flush the path*/
			     (1 << ASYNC_FIFO_RESET) |
			     /*reset the path*/
			     (1 << ASYNC_FIFO_WRAP_EN) |
			     /*wrap enable*/
			     (0 << ASYNC_FIFO_FLUSH_EN) |
			     (((len >> 7) & 0x7fff) << ASYNC_FIFO_FLUSH_CNT_LSB));
	/*number of 128-byte blocks to flush*/

	/*clear the reset signal*/
	WRITE_ASYNC_FIFO_REG(afifo->id, REG1,
			     READ_ASYNC_FIFO_REG(afifo->id,
					     REG1) & ~(1 << ASYNC_FIFO_RESET));

	/*Enable flush*/
	WRITE_ASYNC_FIFO_REG(afifo->id, REG1,
			     READ_ASYNC_FIFO_REG(afifo->id,
					     REG1) | (1 << ASYNC_FIFO_FLUSH_EN));

	/*Setup fill parameters*/
	WRITE_ASYNC_FIFO_REG(afifo->id, REG2,
			     (1 << ASYNC_FIFO_ENDIAN_LSB) |
			     (0 << ASYNC_FIFO_FILL_EN) |
			     (0 << ASYNC_FIFO_FILL_CNT_LSB));

	/*Enable fill path*/
	WRITE_ASYNC_FIFO_REG(afifo->id, REG2,
			     READ_ASYNC_FIFO_REG(afifo->id, REG2) |
			     (1 << ASYNC_FIFO_FILL_EN));

	/*generate flush interrupt*/
	WRITE_ASYNC_FIFO_REG(afifo->id, REG3,
			     (READ_ASYNC_FIFO_REG(afifo->id, REG3) & 0xffff0000) |
			     ((((len >> (factor + 7)) - 1) & 0x7fff) <<
			      ASYNC_FLUSH_SIZE_IRQ_LSB));

	/*Connect the STB DEMUX to ASYNCFIFO*/
	WRITE_ASYNC_FIFO_REG(afifo->id, REG2,
			     READ_ASYNC_FIFO_REG(afifo->id, REG2) |
			     (source_val << ASYNC_FIFO_SOURCE_LSB));

	DMX_WRITE_REG(afifo->hw_dmx_id, DEMUX_CONTROL,
		      DMX_READ_REG(afifo->hw_dmx_id, DEMUX_CONTROL) |
		      (1 << TS_RECORDER_ENABLE));

	print_dbg("afifo%d, source value: %d\n", afifo->id, source_val);
	return 0;
}

/**
 * Initialize the hardware asyncfifo device.
 * @retval 0  On success.
 * @param pdev Platform device
 * @param id Asyncfifo index
 * @param source Asyncfifo source
 * @retval -1 On error.
 */
int
t5d_asyncfifo_init(struct platform_device *pdev, int id, int source)
{
	int source_val;
	int len = ASYNCFIFO_BUFFER_SIZE_DEFAULT;
	struct t5d_asyncfifo *afifo = &t5d_asyncfifos[id];

	if (afifo->init)
		return -1;

	afifo->init = 0;
	afifo->flush_size = (len >> 1);
	afifo->blk.addr = 0;
	afifo->blk.len = 0;

	afifo->id = id;
	afifo->hw_dmx_id = id;
	afifo->source = source;

	if (!afifo->pages) {
		afifo->pages = __get_free_pages(GFP_KERNEL, get_order(len));
	}

	if (!afifo->pages) {
		return -1;
	}

	afifo->pages_map = dma_map_single(&pdev->dev,
					  (void *)afifo->pages, len, DMA_FROM_DEVICE);
	afifo->r_offset = 0;
	afifo->w_offset = 0;
	afifo->buf_len = ASYNCFIFO_BUFFER_SIZE_DEFAULT;
	afifo->dev = &pdev->dev;

	if (afifo->hw_dmx_id == 0)
		source_val = 3;
	else if (afifo->hw_dmx_id == 1)
		source_val = 2;
	else {
		print_err("invalid hw dmx id: %d\n", afifo->hw_dmx_id);
		return -1;
	}

	async_fifo_set_regs(afifo, source_val);

	/*create hw channel , pid == 0*/
	t5d_set_chan_regs(afifo, 0);

	if (!afifo_thread[afifo->id])
		afifo_thread[afifo->id] = kthread_run(asyncfifo_thread_func, afifo, "asyncfifo");

	afifo->init = 1;
	print_dbg("afifo%d , source:%d, init done\n", afifo->id, afifo->source);

	return 0;
}

/**
 * Deinit the hardware asyncfifo device.
 * @retval 0  On success.
 * @retval -1 On error.
 */
int
t5d_asyncfifo_deinit(void)
{
	int i;
	struct t5d_asyncfifo *afifo = NULL;

	for (i = 0; i < T5D_ASYNC_FIFO_COUNT; i ++) {
		afifo = &t5d_asyncfifos[i];
		if (!afifo->init)
			continue;

		CLEAR_ASYNC_FIFO_REG_MASK(afifo->id, REG1, 1 << ASYNC_FIFO_FLUSH_EN);
		CLEAR_ASYNC_FIFO_REG_MASK(afifo->id, REG2, 1 << ASYNC_FIFO_FILL_EN);

		if (afifo->pages) {
			dma_unmap_single(afifo->dev,
					 afifo->pages_map, ASYNCFIFO_BUFFER_SIZE_DEFAULT, DMA_FROM_DEVICE);
			/* Don't really free memory here*/
			afifo->pages_map = 0;
		}

		afifo->r_offset = 0;
		afifo->w_offset = 0;
		afifo->buf_len = 0;
		afifo->init = 0;
		/*TODO: If the destruction operation is considered, then the mutual
		 * exclusion between this and the asyncfifo thread is required.
		 */
		if (afifo_thread[afifo->id]) {
			kthread_stop(afifo_thread[afifo->id]);
			afifo_thread[afifo->id] = NULL;
		}
	}

	return 0;
}

int
t5d_desc_set_pid(int dev_id, int chan_id, int pid)
{
	int offset = (dev_id == 1) ? 4 : 0;
	u32 data;

	WRITE_MPEG_REG(TS_PL_PID_INDEX, ((chan_id & 0x0f) >> 1) + offset);
	data = READ_MPEG_REG(TS_PL_PID_DATA);
	if (chan_id & 1) {
		data &= 0xFFFF0000;
		data |= pid & 0x1fff;
		if (pid == 0x1fff)
			data |= 1 << PID_MATCH_DISABLE_LOW;
	} else {
		data &= 0xFFFF;
		data |= (pid & 0x1fff) << 16;
		if (pid == 0x1fff)
			data |= 1 << PID_MATCH_DISABLE_HIGH;
	}
	WRITE_MPEG_REG(TS_PL_PID_INDEX, ((chan_id & 0x0f) >> 1) + offset);
	WRITE_MPEG_REG(TS_PL_PID_DATA, data);
	WRITE_MPEG_REG(TS_PL_PID_INDEX, 0);

	print_dbg("desc%d set ch%d pid: %#x\n", dev_id, chan_id, pid);

	return 0;
}

int
t5d_desc_set_key(int dev_id, int chan_id, u8 *key, enum t5d_hwdmx_keyparity parity)
{
	u16 k0, k1, k2, k3;
	u32 key0, key1;
	int reg;
	int type;

#if 0	//for test
	key[3] = key[0] + key[1] + key[2];
	key[7] = key[4] + key[5] + key[6];
#endif

	k0 = (key[0] << 8) | key[1];
	k1 = (key[2] << 8) | key[3];
	k2 = (key[4] << 8) | key[5];
	k3 = (key[6] << 8) | key[7];

	key0 = (k0 << 16) | k1;
	key1 = (k2 << 16) | k3;
	WRITE_MPEG_REG(COMM_DESC_KEY0, key0);
	WRITE_MPEG_REG(COMM_DESC_KEY1, key1);

	if (parity == HWDMX_DESC_ODD_KEY)
		type = 1;
	else if (parity == HWDMX_DESC_EVEN_KEY)
		type = 0;
	else {
		print_err("wrong parity %d, chan_id:%d\n", parity, chan_id);
		return -1;
	}

	reg = (chan_id + type * T5D_DSC_PER_DEMUX) + ((dev_id == 1) ? 16 : 0);
	WRITE_MPEG_REG(COMM_DESC_KEY_RW, reg);
	print_dbg("set %08x%08x, chan:%d type:%d, dev_id:%d\n",
		  key0, key1,
		  chan_id, type, dev_id);

	return 0;
}

void
t5d_hwdmx_desc_init(void)
{
	memset(t5d_descchannels, 0,
	       sizeof(struct t5d_hwdmx_descchannel) *
	       T5D_HW_DMX_COUNT * T5D_DSC_PER_DEMUX);

	return;
}

struct t5d_hwdmx_descchannel *
t5d_hwdmx_desc_alloc_channel(int source)
{
	int i;
	int dev_id = -1;
	struct t5d_hwdmx_descchannel *chan = NULL;

	for (i = 0; i < T5D_ASYNC_FIFO_COUNT; i++) {
		if (t5d_asyncfifos[i].source == source) {
			dev_id = t5d_asyncfifos[i].hw_dmx_id;
			break;
		}
	}

	if (dev_id == -1) return chan;

	for (i = 0; i < T5D_DSC_PER_DEMUX; i++) {
		chan = &t5d_descchannels[dev_id][i];
		if (!chan->used) {
			memset(chan, 0, sizeof(struct t5d_hwdmx_descchannel));
			chan->dev_id = dev_id;
			chan->chan_id = i;
			chan->pid    = 0x1fff;
			chan->used = 1;
			print_dbg("alloc desc channel %p on dsc%d\n", chan, dev_id);
			return chan;
		}
	}

	return chan;
}

int
t5d_hwdmx_desc_channel_set_pid(
	struct t5d_hwdmx_descchannel *chan,
	u16       pid)
{
	if (!chan) return -1;

	if (pid >= 0x1fff) {
		print_err("illegal PID 0x%04x", pid);
		return -1;
	}

	chan->pid = pid;

	// set pid to hw register according to the dev_id and chan_id
	t5d_desc_set_pid(chan->dev_id, chan->chan_id, pid);

	return 0;
}

int
t5d_hwdmx_desc_channel_set_key(
	struct t5d_hwdmx_descchannel *chan,
	enum t5d_hwdmx_keyparity parity,
	struct t5d_hwdmx_key *key)
{
	if (!chan || !key) {
		print_err("wrong set key param");
		return -1;
	}

	switch (parity) {
	case HWDMX_DESC_ODD_KEY:
		chan->odd_key = key;
		break;
	case HWDMX_DESC_EVEN_KEY:
		chan->even_key = key;
		break;
	default:
		print_err("wrong parity:%d\n", parity);
		break;
	}

	t5d_desc_set_key(chan->dev_id, chan->chan_id, key->data, parity);

	return 0;
}

int
t5d_hwdmx_desc_channel_set_scb(
	struct t5d_hwdmx_descchannel *chan,
	int as_is,
	int scb)
{
	if (!chan) return -1;

	chan->scb_as_is = as_is;
	chan->scb = scb;

	return 0;
}

int
t5d_hwdmx_desc_channel_enable(struct t5d_hwdmx_descchannel *chan)
{
	if (!chan || !chan->used) {
		return -1;
	}

	return 0;
}

int
t5d_hwdmx_desc_channel_disable(struct t5d_hwdmx_descchannel *chan)
{
	if (!chan) return -1;

	chan->enable = 0;

	return 0;
}

void
t5d_hwdmx_desc_channel_free(struct t5d_hwdmx_descchannel *chan)
{
	if (!chan) return;

	memset(chan, 0, sizeof(struct t5d_hwdmx_descchannel));
}

/**
 * Clear the key entry.
 * \param key The key entry.
 * \retval 0 On success.
 * \retval -1 On error.
 */
int
t5d_hwdmx_key_clear(struct t5d_hwdmx_key *key)
{
	if (!key) return -1;

	if (key->used) {
		key->used = 0;
		key->algo = HWDMX_DESC_ALGO_NONE;
	}

	return 0;
}

/**
 * Set the key value.
 * \param key The key entry.
 * \param v The key value.
 * \param len The key's length.
 * \retval 0 On success.
 * \retval -1 On error.
 */
int
t5d_hwdmx_key_set(struct t5d_hwdmx_key *key, const u8 *v, int len)
{
	int i, j;
	struct t5d_hwdmx_descchannel *chan = NULL;
	enum t5d_hwdmx_keyparity parity;

	if (!key || !v) return -1;

	memcpy(&key->data[0], v, len);

	for (i = 0; i < T5D_HW_DMX_COUNT; i++) {
		for (j = 0; j < T5D_DSC_PER_DEMUX; j++) {
			chan = &t5d_descchannels[i][j];
			if (!chan->used) continue;

			if (chan->odd_key == key) {
				parity = HWDMX_DESC_ODD_KEY;
			} else if (chan->even_key == key) {
				parity = HWDMX_DESC_EVEN_KEY;
			} else
				continue;

			t5d_desc_set_key(chan->dev_id, chan->chan_id, key->data, parity);
			key->used = 1;
		}
	}
	return 0;
}
