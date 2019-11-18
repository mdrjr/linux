// SPDX-License-Identifier: GPL-2.0+
/*
 * FB driver for the Hardkernel 3.5 inch TFT LCD
 * that uses the ILI9488 LCD Controller
 * for Odroid-N2/C4 using kernel 4.9
 *
 * Copyright (C) 2020 Deokgyu Yang
 *
 * Based on fb_ili9340.c by Noralf Tronnes
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/vmalloc.h>
#include <linux/gpio/consumer.h>
#include <linux/backlight.h>
#include <linux/delay.h>

#include "fbtft.h"

#define DRVNAME			"fb_hktft35"
#define WIDTH			320
#define HEIGHT			480

#define ODROID_TFT35_MACTL_MV	0x20
#define ODROID_TFT35_MACTL_MX	0x40
#define ODROID_TFT35_MACTL_MY	0x80

/* this init sequence matches Hardkernel 3.5 inch TFT LCD */
static int default_init_sequence[] = {
	-1, 0xB0,0x00,
	-1, 0x11,
	-2, 120,
	-1, 0x3A,0x55,
	-1, 0xC2,0x33,
	-1, 0xC5,0x00,0x1E,0x80,
	-1, 0x36,0x28,
	-1, 0xB1,0xB0,
	-1, 0xE0,0x00,0x04,0x0E,0x08,0x17,0x0A,0x40,0x79,0x4D,0x07,0x0E,0x0A,0x1A,0x1D,0x0F,
	-1, 0xE1,0x00,0x1B,0x1F,0x02,0x10,0x05,0x32,0x34,0x43,0x02,0x0A,0x09,0x33,0x37,0x0F,
	-1, 0x11,
	-1, 0x29,
	-3
};

static void set_addr_win(struct fbtft_par *par, int xs, int ys, int xe, int ye)
{
	fbtft_par_dbg(DEBUG_SET_ADDR_WIN, par,
		"%s(xs=%d, ys=%d, xe=%d, ye=%d)\n", __func__, xs, ys, xe, ye);

	/* Column address */
	write_reg(par, 0x2A, xs >> 8, xs & 0xFF, xe >> 8, xe & 0xFF);

	/* Row adress */
	write_reg(par, 0x2B, ys >> 8, ys & 0xFF, ye >> 8, ye & 0xFF);

	/* Memory write */
	write_reg(par, 0x2C);
}

static int set_var(struct fbtft_par *par)
{
	u8 val;

	switch (par->info->var.rotate) {
	case 270:
		val = ODROID_TFT35_MACTL_MV;
		break;
	case 180:
		val = ODROID_TFT35_MACTL_MY;
		break;
	case 90:
		val = ODROID_TFT35_MACTL_MV | ODROID_TFT35_MACTL_MX | ODROID_TFT35_MACTL_MY;
		break;
	default:
		val = ODROID_TFT35_MACTL_MX;
		break;
	}
	/* Memory Access Control  */
	write_reg(par, 0x36, val | (par->bgr << 3));
	return 0;
}

static struct fbtft_display display = {
	.regwidth = 8,
	.buswidth = 8,
	.width = WIDTH,
	.height = HEIGHT,
	.init_sequence = default_init_sequence,
	.fbtftops = {
		.set_addr_win = set_addr_win,
		.set_var = set_var,
	},
};
FBTFT_REGISTER_DRIVER(DRVNAME, "odroid,hktft35", &display);

MODULE_ALIAS("platform:" DRVNAME);
MODULE_ALIAS("platform:hktft35");

MODULE_DESCRIPTION("FB driver for the Hardkernel 3.5 inch TFT LCD uses the ILI9488 LCD Controller");
MODULE_AUTHOR("Deokgyu Yang");
MODULE_LICENSE("GPL");
