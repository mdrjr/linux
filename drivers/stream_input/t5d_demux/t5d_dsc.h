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
#ifndef _T5D_DSC_H_
#define _T5D_DSC_H_

#define T5D_DSC_PER_DEMUX 8

struct t5d_sw_dsc {
	int id;
	struct dvb_device *dev;
	int ca_chan[T5D_DSC_PER_DEMUX];
};

/**
 * Initialize the descrambler.
 * @param pdev the platform device handle
 * @param dsc the descrambler handle
 * @retval 0 On success.
 * @retval -1 On error.
 */
extern int
t5d_dsc_init(
	struct platform_device *pdev,
	struct t5d_sw_dsc *dsc);

/**
 * Release the descrambler.
 * @param dsc the descrambler handle
 * @retval 0 On success.
 * @retval -1 On error.
 */
extern void
t5d_dsc_deinit(struct t5d_sw_dsc *dsc);
#endif /*_T5D_DSC_H_*/
