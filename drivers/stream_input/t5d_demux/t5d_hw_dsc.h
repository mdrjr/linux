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

#ifndef _T5D_HW_DESC_H_
#define _T5D_HW_DESC_H_

#include <linux/kernel.h>

/**Descrambler key's parity.*/
enum t5d_hwdmx_keyparity {
	HWDMX_DESC_ODD_KEY,
	HWDMX_DESC_EVEN_KEY,
	HWDMX_DESC_ODD_IV,
	HWDMX_DESC_EVEN_IV
};

/**Key's algorithm.*/
enum t5d_hwdmx_keyalgo {
	HWDMX_DESC_ALGO_NONE,
	HWDMX_DESC_ALGO_CSA_DEC,
	HWDMX_DESC_ALGO_AES_DEC
};

/**Key entry.*/
struct t5d_hwdmx_key {
	int used;
	int is_iv;
	enum t5d_hwdmx_keyalgo algo;
	u8 data[16];	/**< Key data.*/
};

/**Descrambler channel*/
struct t5d_hwdmx_descchannel {
	int dev_id;
	int chan_id;
	int used;
	int enable;
	u16 pid;
	int scb_as_is;
	int scb;
	struct t5d_hwdmx_key *odd_key;
	struct t5d_hwdmx_key *even_key;
};

extern void
t5d_hwdmx_desc_init(void);

extern struct t5d_hwdmx_descchannel *
t5d_hwdmx_desc_alloc_channel(int source);

extern int
t5d_hwdmx_desc_channel_set_pid(
	struct t5d_hwdmx_descchannel *chan,
	u16       pid);

extern int
t5d_hwdmx_desc_channel_set_key(
	struct t5d_hwdmx_descchannel *chan,
	enum t5d_hwdmx_keyparity parity,
	struct t5d_hwdmx_key *key);

extern int
t5d_hwdmx_desc_channel_set_scb(
	struct t5d_hwdmx_descchannel *chan,
	int as_is,
	int scb);

extern int
t5d_hwdmx_desc_channel_enable(struct t5d_hwdmx_descchannel *chan);

extern int
t5d_hwdmx_desc_channel_disable(struct t5d_hwdmx_descchannel *chan);

extern void
t5d_hwdmx_desc_channel_free(struct t5d_hwdmx_descchannel *chan);

extern int
t5d_hwdmx_key_clear(struct t5d_hwdmx_key *key);

extern int
t5d_hwdmx_key_set(struct t5d_hwdmx_key *key, const u8 *v, int len);
#endif /*_T5D_HW_DESC_H_*/
