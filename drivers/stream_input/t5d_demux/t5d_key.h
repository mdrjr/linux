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

#ifndef _T5D_KEY_H_
#define _T5D_KEY_H_
/**
 * Init key device
 * @retval 0 On success.
 * @retval 1 On error.
 */
extern int
t5d_key_init(void);

/**
 * DeInit key device
 * @retval 0 On success.
 * @retval 1 On error.
 */
extern void
t5d_key_deinit(void);
#endif /*_T5D_KEY_H_*/
