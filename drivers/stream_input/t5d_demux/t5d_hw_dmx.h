/* SPDX-License-Identifier: LGPL-2.1+ WITH Linux-syscall-note */
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

#ifndef _T5D_HW_DMX_H_
#define _T5D_HW_DMX_H_

#define T5D_TS_IN_COUNT			3
#define T5D_S2P_COUNT			3
#define T5D_ASYNC_FIFO_COUNT	2
#define T5D_HW_DMX_COUNT		3
#define T5D_DSC_PER_DEMUX		8

/*extend enum uses flags[16:23] in dmx.h*/
/*private define for dump data*/
enum {
	DMX_DUMP_DVR_TYPE = 0x80,
	DMX_DUMP_PES_TYPE,
	DMX_DUMP_ES_TYPE,
	DMX_DUMP_ES_VIDEO_TYPE,
	DMX_DUMP_ES_AUDIO_TYPE,
	DMX_DUMP_SECTION_TYPE,
	DMX_DUMP_TS_TYPE,
	DMX_DUMP_INPUT_TYPE
};

enum {
	T5D_TS_DISABLE,
	T5D_TS_PARALLEL,
	T5D_TS_SERIAL
};

struct t5d_ts_in {
	int id;
	int mode;
	struct pinctrl *pinctrl;
	int control;
	int s2p_id;
	int csa_enable;
};

struct t5d_s2p {
	int invert;
};

struct t5d_dvr_block {
	u32 addr;
	u32 len;
};

struct t5d_asyncfifo {
	int id;
	int hw_dmx_id;
	int source; /*t5d_demux_source*/
	int init;
	unsigned long pages;
	unsigned long pages_map;
	int buf_len;
	int w_offset;
	int r_offset;
	int flush_size;
	struct t5d_dvr_block blk;
	unsigned long stored_pages;
	struct device *dev;
};

/**
 * Initialize the hardware demux device.
 * @retval 0  On success.
 * @retval -1 On error.
 */
extern int
t5d_hw_dmx_init(void);

/**
 * Initialize the hardware demux device.
 * @param hw_dmx_id Hardware demux device id
 * @param ts_inputs Hardware TS inputs array
 * @param s2ps Hardware s2p array
 * @retval 0  On success.
 * @retval -1 On error.
 */
extern int
t5d_hw_dmx_id_init(int hw_dmx_id,
		   struct t5d_ts_in *ts_inputs,
		   struct t5d_s2p *s2ps);

/**
 * Set the hardware demux ts output.
 * @param value The ts out value
 * @retval 0  On success.
 * @retval -1 On error.
 */
extern int
t5d_hw_dmx_set_tso(unsigned int value);

/**
 * Get the hardware demux ts output.
 * @retval ts output value.
 */
extern unsigned int
t5d_hw_dmx_get_tso(void);

/**
 * Release the hardware demux device.
 * @retval 0  On success.
 * @retval -1 On error.
 */
extern int
t5d_hw_dmx_deinit(void);

/**
 * Add the dump stream id
 * @param source The input source type
 * @param feed The dvb_demux_feed
 * @param dump_type The dump type
 * @retval 0 On success
 * @retval -1 On error
 */
extern int
t5d_dump_add_sid(int source, struct dvb_demux_feed *feed, int dump_type);

/**
 * Get the dump input feed
 * @param source The input source type
 * @retval feed On success
 * @retval NULL On error
 */
extern struct dvb_demux_feed *
t5d_dump_get_input_feed(int source);

/**
 * Remove the dump stream id
 * @param source The input source type
 * @param cb The ts feed cb
 * @retval 0 On success
 * @retval -1 On error
 */
extern int
t5d_dump_remove_sid(int source, struct dvb_demux_feed *feed);

/**
 * Initialize the hardware asyncfifo device.
 * @retval 0  On success.
 * @param pdev Platform device
 * @param id Asyncfifo index
 * @param source Asyncfifo source
 * @retval -1 On error.
 */
extern int
t5d_asyncfifo_init(struct platform_device *pdev, int id, int source);
/**
 * Release the hardware asyncfifo device.
 * @retval 0  On success.
 * @retval -1 On error.
 */
extern int
t5d_asyncfifo_deinit(void);
#endif /*_T5D_HW_DMX_H_*/
