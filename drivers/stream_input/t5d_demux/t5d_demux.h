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

#ifndef _T5D_DEMUX_H_
#define _T5D_DEMUX_H_

#ifdef USERSPACE
#include <inttypes.h>
#include <pthread.h>
#include <string.h>
#include <assert.h>
#include <stdio.h>
#include <stdarg.h>

typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef uint8_t  bool;

#define TRUE  1
#define FALSE 0
#define true  1
#define false 0

#define CONFIG_AMLOGIC_DVB_COMPAT

struct mutex {
	pthread_mutex_t lock;
};

#define mutex_init(m) pthread_mutex_init(&(m)->lock, NULL)
#define mutex_destroy(m) pthread_mutex_destroy(&(m)->lock)
#define mutex_lock_interruptible(m) pthread_mutex_lock(&(m)->lock)
#define mutex_lock(m) pthread_mutex_lock(&(m)->lock)
#define mutex_unlock(m) pthread_mutex_unlock(&(m)->lock)

#define t5d_assert(a) assert(a)

static inline void
t5d_log(const char *fmt, ...)
{
	va_list ap;

	fprintf(stderr, "SWDMX: ");

	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);

	fprintf(stderr, "\n");
}

#else /*!defined USERSPACE*/

#include <linux/slab.h>
#include <linux/stddef.h>
#include <linux/module.h>
#include <linux/mutex.h>

//#define CONFIG_AMLOGIC_DVB_COMPAT

#define t5d_assert(a) \
		do {\
			if (!(a)) printk("t5d_demux: assert %s\n", #a);\
		} while (0)

#define t5d_param_check(expr) \
		do {\
			if (!(expr)) {\
				printk("t5d_demux: invalid param %s\n", #expr);\
				return -1;\
			}\
		} while (0)

#define LOG_ERROR 0
#define LOG_DBG	1
#define LOG_VER 2

#define dprintk(level, debug, x...)\
	do {\
		if ((level) <= (debug)) \
			printk(x);\
	} while (0)
#endif /*USERSPACE*/

#include "linux/amlogic/aml_key.h"
#include "linux/dvb/ca.h"
#include "linux/dvb/dmx.h"
#include <linux/dvb/aml_dmx_ext.h>
#include "linux/dvb/aml_ca_ext.h"
#include <linux/list.h>
#include "sw_demux/swdemux.h"

/**Filters number in one demux device.*/
#define T5D_FILTER_PER_DEMUX  32
/**Descramblers number in one demux device.*/
#define T5D_DESC_PER_DEMUX    32
/**Demux devices number.*/
#define T5D_DEMUX_PER_DEVICE  8
/**Key entries number.*/
#define T5D_KEY_PER_DEVICE    256
/**Hardware stream number.*/
#define T5D_HW_STREAM_NUM     3
/**DMA input stream number.*/
#define T5D_DMA_STREAM_NUM    8
/**Stream TS buffer size.*/
#define T5D_STREAM_TS_BUFFER_SIZE 4096

/**Filter type.*/
enum t5d_filter_type {
	T5D_FILTER_PES, /**< PES filter.*/
	T5D_FILTER_SEC  /**< Section filter.*/
};

/**Demux input source.*/
enum t5d_demux_source {
	T5D_DMA_0 = 0,
	T5D_DMA_1,
	T5D_DMA_2,
	T5D_DMA_3,
	T5D_DMA_4,
	T5D_DMA_5,
	T5D_DMA_6,
	T5D_DMA_7,
	T5D_FRONTEND_TS0 = 32,
	T5D_FRONTEND_TS1,
	T5D_FRONTEND_TS2,
	T5D_FRONTEND_TS3,
	T5D_FRONTEND_TS4,
	T5D_FRONTEND_TS5,
	T5D_FRONTEND_TS6,
	T5D_FRONTEND_TS7
};

/**Filter data callback.*/
typedef void (*t5d_filter_cb)(int dmx_id, int filter_id, const u8 *data, int len, void *user_data);

/**ES data.*/
struct t5d_es_data {
	bool valid;     /**< Is a valid PES.*/
	bool has_pts;   /**< Has PTS.*/
	bool scrambled; /**< Stream is scrambled.*/
	u64  pts;       /**< PTS value.*/
	int  len;       /**< Data length.*/
	u8  *data;      /**< Data pointer.*/
};

/**Filter.*/
struct t5d_filter {
	int  dmx_id;  /**< The demux device's index.*/
	int  id;      /**< Index.*/
	bool used;    /**< Is used?*/
	bool started; /**< Is started?*/
	enum t5d_filter_type type; /**< Filter type: PES/section.*/
	struct dmx_sct_filter_params sec_params; /**< Section filter's parameters.*/
	struct dmx_pes_filter_params pes_params; /**< PES filter's parameters.*/
	struct swdmx_secfilter *sw_sec_filter;   /**< Software section filter.*/
	struct swdmx_tsfilter  *sw_ts_filter;    /**< Software TS filter.*/
	struct swdmx_pesparser *sw_pes_parser;   /**< Software PES parser.*/
	void *user_data; /**< User defined data used in callback.*/
	t5d_filter_cb cb; /**< Callback function.*/
};

/**Descrambler.*/
struct t5d_descrambler {
	int  id;   /**< Index.*/
	bool used; /**< Is used?*/
	bool scb_as_is; /**< Key SCB flags.*/
	u16  pid;  /**< PID.*/
	int  scb_value; /**< New SCB flags.*/
	enum ca_sc2_algo_type algo; /**< Algorithm.*/
	int  even_key_id; /**< Even key's index.*/
	int  even_iv_id;  /**< Event IV data index.*/
	int  odd_key_id;  /**< Odd key's index.*/
	int  odd_iv_id;   /**< Odd IV data index.*/
	int  zero_key_id; /**< 00 key's index.*/
	int  zero_iv_id;  /**< 00 key's IV data index.*/
	struct swdmx_descchannel *sw_desc_chan; /**< Software descrambler channel.*/
	struct t5d_hwdmx_descchannel *hw_desc_chan; /**< Hardware descrambler channel.*/
};

/**Demux.*/
struct t5d_demux {
	enum t5d_demux_source source; /**< Data input source.*/
	struct list_head lh; /**< List node.*/
	int id; /**< Index.*/
	struct t5d_filter      filters[T5D_FILTER_PER_DEMUX]; /**< Filters in this demux.*/
	struct t5d_descrambler descs[T5D_DESC_PER_DEMUX];     /**< Descramblers in this demux.*/
};

/**Key entry.*/
struct t5d_key {
	int  id;   /**< Index.*/
	bool used; /**< Is used?*/
	enum user_id  user; /**< User module ID.*/
	enum key_algo algo; /**< Algorithm.*/
	u8   key[16]; /**< Key value.*/
	struct swdmx_key sw_key; /**< Software key entry.*/
	struct t5d_hwdmx_key hw_key; /**< Hardware key entry.*/
};

/**Stream type.*/
enum t5d_stream_type {
	T5D_STREAM_HW,  /**< Hardware TS input.*/
	T5D_STREAM_DMA  /**< DDR input TS.*/
};

/**Stream.*/
struct t5d_stream {
	enum t5d_stream_type    type;    /**< Stream type.*/
	enum t5d_demux_source   source;  /**< Source.*/
	struct list_head        demuxes; /**< Demuxes list in this source.*/
	struct swdmx_descrambler *sw_desc;    /**< Software descrambler.*/
	struct swdmx_ts_parser *sw_ts_parser; /**< Software TS parser.*/
	struct swdmx_demux     *sw_demux;     /**< Software demux.*/
	u8                      cache_buffer[188]; /**< Cached TS buffer.*/
	int                     cache_len;    /**< Cached TS data length.*/
	u8                     *ts_buffer;    /**< TS buffer used for process.*/
};

/**DVB device.*/
struct t5d_dvb {
	struct mutex      lock; /**< Mutex.*/
	struct t5d_demux  demuxes[T5D_DEMUX_PER_DEVICE];   /**< Demux devices.*/
	struct t5d_key    keys[T5D_KEY_PER_DEVICE];        /**< Key entries.*/
	struct t5d_stream hw_streams[T5D_HW_STREAM_NUM];   /**< Hardware input stream.*/
	struct t5d_stream dma_streams[T5D_DMA_STREAM_NUM]; /**< DDR input stream.*/
};

/**
 * Initialize the DVB device.
 * @retval 0 On success.
 * @retval -1 On error.
 */
extern int
t5d_init_device(void);

/**
 * Release the DVB device.
 * @retval 0 On success.
 * @retval -1 On error.
 */
extern int
t5d_deinit_device(void);

/**
 * Set the demux device's input source.
 * @param dmx_id Demux device's index.
 * @param source The input source.
 * @retval 0 On success.
 * @retval -1 On error.
 */
extern int
t5d_set_demux_source(
	int dmx_id,
	enum t5d_demux_source source);

/**
 * Get the demux's source.
 * @param dmx_id Demux device's index.
 * @return The demux's source.
 */
extern enum t5d_demux_source
t5d_get_demux_source(int dmx_id);

/**
 * Get the demux's source without lock.
 * @param dmx_id Demux device's index.
 * @return The demux's source.
 */
extern enum t5d_demux_source
t5d_get_demux_source_unlock(int dmx_id);

/**
 * Allocate a filter.
 * @param dmx_id Demux device's index.
 * @return The filter's index.
 * @retval -1 On error.
 */
extern int
t5d_alloc_filter(
	int dmx_id);

/**
 * Free a filter.
 * @param dmx_id Demux device's index.
 * @param filter_id The filter's index to be freed.
 * @return The filter's index.
 * @retval -1 On error.
 */
extern int
t5d_free_filter(
	int dmx_id,
	int filter_id);

/**
 * Set the PES filter's parameters.
 * @param dmx_id Demux device's index.
 * @param filter_id The filter's index.
 * @param params The filter's parameters.
 * @retval 0 On success.
 * @retval -1 On error.
 */
extern int
t5d_set_pes_filter(
	int dmx_id,
	int filter_id,
	struct dmx_pes_filter_params *params);

/**
 * Set the section filter's parameters.
 * @param dmx_id Demux device's index.
 * @param filter_id The filter's index.
 * @param params The filter's parameters.
 * @retval 0 On success.
 * @retval -1 On error.
 */
extern int
t5d_set_sec_filter(
	int dmx_id,
	int filter_id,
	struct dmx_sct_filter_params *params);

/**
 * Set the filter's callback functions.
 * @param dmx_id Demux device's index.
 * @param filter_id The filter's index.
 * @param cb The callback function.
 * @param user_data The user defined data used in the callback function.
 * @retval 0 On success.
 * @retval -1 On error.
 */
extern int
t5d_set_filter_callback(
	int dmx_id,
	int filter_id,
	t5d_filter_cb cb,
	void *user_data);

/**
 * Start a filter.
 * @param dmx_id Demux device's index.
 * @param filter_id The filter's index.
 * @retval 0 On success.
 * @retval -1 On error.
 */
extern int
t5d_start_filter(
	int dmx_id,
	int filter_id);

/**
 * Stop a filter.
 * @param dmx_id Demux device's index.
 * @param filter_id The filter's index.
 * @retval 0 On success.
 * @retval -1 On error.
 */
extern int
t5d_stop_filter(
	int dmx_id,
	int filter_id);

/**
 * Process the TS stream.
 * @param source The input source type.
 * @param data The TS data buffer.
 * @param len The data length.
 * @retval 0 On success.
 * @retval -1 On error.
 */
extern int
t5d_process_stream(
	enum t5d_demux_source source,
	const u8 *data,
	int len);

/**
 * Dump all CA channel infos.
 * @param buf The output CA channel infos.
 * @retval 0 On success.
 * @retval -1 On error.
 */
extern int
t5d_dump_ca_info(char *buf);

/**
 * Allocate a descrambler channel.
 * @param ca_id The CA device index.
 * @param pid PID.
 * @param algo Descrambling algorithm.
 * @param type Module type.
 * @retval 0 On success.
 * @retval -1 On error.
 */
extern int
t5d_alloc_ca_chan(
	int ca_id,
	u16 pid,
	enum ca_sc2_algo_type algo,
	enum ca_sc2_dsc_type type);

/**
 * Free a descrambler channel.
 * @param ca_id The CA device index.
 * @param chan_id The descrambler channel index.
 * @retval 0 On success.
 * @retval -1 On error.
 */
extern int
t5d_free_ca_chan(
	int ca_id,
	int chan_id);

/**
 * Set the descrambler's key.
 * @param ca_id The CA device index.
 * @param chan_id The descrambler channel index.
 * @param type The key's parity.
 * @param key_id The ke entry's index.
 * @retval 0 On success.
 * @retval -1 On error.
 */
extern int
t5d_set_ca_key(
	int ca_id,
	int chan_id,
	enum ca_sc2_key_type type,
	int key_id);

/**
 * Reset the descrambler's algorithm.
 * @param ca_id The CA device index.
 * @param chan_id The descrambler channel index.
 * @param algo The algorithm.
 * @retval 0 On success.
 * @retval -1 On error.
 */
extern int
t5d_set_ca_algo(
	int ca_id,
	int chan_id,
	enum ca_sc2_algo_type algo);

/**
 * Set the output scrambling control flag.
 * @param ca_id The CA device index.
 * @param chan_id The descrambler channel index.
 * @param as_is Keep the origin scrambling control.
 * @param scb The new scrambling control.
 * @retval 0 On success.
 * @retval -1 On error.
 */
extern int
t5d_set_ca_scb(
	int ca_id,
	int chan_id,
	bool as_is,
	int scb);

/**
 * Clear the descramblers in a CA device.
 * @param ca_id The CA device index.
 * @retval 0 On success.
 * @retval -1 On error.
 */
extern int
t5d_clear_ca(int ca_id);

/**
 * Allocate a new key entry.
 * @param is_iv The key is an IV data.
 * @return The new key entry's index.
 * @retval -1 On error.
 */
extern int
t5d_alloc_key(int is_iv);

/**
 * Free a key entry.
 * @param key_id The key entry's index.
 * @retval 0 On success.
 * @retval -1 On error.
 */
extern int
t5d_free_key(
	int key_id);

/**
 * Configure the key entry.
 * @param key_id The key entry's index.
 * @param user The module user index.
 * @param algo The algorithm.
 * @retval 0 On success.
 * @retval -1 On error.
 */
extern int
t5d_config_key(
	int key_id,
	enum user_id user,
	enum key_algo algo);

/**
 * Set the key.
 * @param key_id The key entry's index.
 * @param key The key value.
 * @param len The key's length.
 * @retval 0 On success.
 * @retval -1 On error.
 */
extern int
t5d_set_key(
	int key_id,
	const u8 *key,
	int len);

/**
 * Clear the key table.
 * @retval 0 On success.
 * @retval -1 On error.
 */
extern int
t5d_clear_keys(void);

#endif /*_T5D_DEMUX_H_*/
