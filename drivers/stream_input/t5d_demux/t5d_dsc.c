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
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/dvb/aml_ca_ext.h>
#include <linux/dvb/dmx.h>
#include <media/dvb_frontend.h>
#include <media/dvb_demux.h>
#include <media/dmxdev.h>

#include "t5d_hw_dsc.h"
#include "t5d_demux.h"
#include "t5d_dsc.h"

#define print_err(fmt, args...)   \
	dprintk(LOG_ERROR, debug_dsc, fmt, ## args)
#define print_dbg(fmt, args...)   \
	dprintk(LOG_DBG, debug_dsc, fmt, ## args)

#define T5D_DSC_CHANNEL_NUM 8

MODULE_PARM_DESC(debug_dsc, "\n\t\t Enable descrambler debug information");
static int debug_dsc;
module_param(debug_dsc, int, 0644);

static int t5d_dvbdsc_open(struct inode *inode, struct file *file)
{
	int err;

	err = dvb_generic_open(inode, file);
	if (err < 0)
		return err;

	return 0;
}

static int t5d_dvbdsc_release(struct inode *inode, struct file *file)
{
	struct dvb_device *dvbdev = file->private_data;
	struct t5d_sw_dsc *dsc = dvbdev->priv;

	t5d_clear_ca(dsc->id);
	dvb_generic_release(inode, file);

	return 0;
}

static int t5d_handle_desc_ext(
	struct t5d_sw_dsc *dsc,
	struct ca_sc2_descr_ex *d)
{
	int ret = -EINVAL;

	switch (d->cmd) {
	case CA_ALLOC:
		if (d->params.alloc_params.algo > CA_ALGO_UNKNOWN)
			break;
		if (d->params.alloc_params.loop != 1
		    && d->params.alloc_params.loop != 0) {
			print_err("alloc params loop: %d, error\n",
				  d->params.alloc_params.loop);
			break;
		}
		if (d->params.alloc_params.dsc_type == CA_DSC_TSD_TYPE
		    || d->params.alloc_params.dsc_type == CA_DSC_TSE_TYPE) {
			print_err("not support TSE/TSD\n");
			break;
		}

		d->params.alloc_params.ca_index =
			t5d_alloc_ca_chan(dsc->id,
					  d->params.alloc_params.pid & 0x1FFF,
					  d->params.alloc_params.algo,
					  d->params.alloc_params.dsc_type);
		print_dbg("CA_ALLOC dsc%d ca_index: %d, pid: %#x, loop: %d\n",
			  dsc->id,
			  d->params.alloc_params.ca_index,
			  d->params.alloc_params.pid,
			  d->params.alloc_params.loop);
		if (d->params.alloc_params.ca_index >= 0)
			ret = 0;

		ret = t5d_set_ca_algo(
			      dsc->id,
			      d->params.alloc_params.ca_index,
			      d->params.alloc_params.algo);
		print_dbg("CA_SET_ALGO dsc%d ca_index: %d, algo: %d\n",
			  dsc->id,
			  d->params.alloc_params.ca_index,
			  d->params.alloc_params.algo);
		break;
	case CA_FREE:
		ret = t5d_free_ca_chan(dsc->id,
				       d->params.free_params.ca_index);
		print_dbg("CA_FREE dsc%d, ca_index: %d\n",
			  dsc->id,
			  d->params.free_params.ca_index);
		break;
	case CA_KEY:
		ret = t5d_set_ca_key(dsc->id,
				     d->params.key_params.ca_index,
				     d->params.key_params.parity,
				     d->params.key_params.key_index);
		print_dbg("CA_KEY dsc%d, ca_index:%d, kte: %d, parity: %d\n",
			  dsc->id,
			  d->params.key_params.ca_index,
			  d->params.key_params.key_index,
			  d->params.key_params.parity);
		break;
	case CA_GET_STATUS:
		print_err("CA_GET_STATUS: not support\n");
		ret = 0;
		break;
	case CA_SET_SCB:
		ret = t5d_set_ca_scb(
			      dsc->id,
			      d->params.scb_params.ca_index,
			      d->params.scb_params.ca_scb_as_is,
			      d->params.scb_params.ca_scb);
		print_dbg("dsc%d CA_SET_SCB: %#x, ca_index: %d\n",
			  dsc->id,
			  d->params.scb_params.ca_scb,
			  d->params.scb_params.ca_index);
		break;
	case CA_SET_ALGO:
		ret = t5d_set_ca_algo(
			      dsc->id,
			      d->params.algo_params.ca_index,
			      d->params.algo_params.algo + 1);
		print_dbg("CA_SET_ALGO dsc%d ca_index: %d, algo: %d\n",
			  dsc->id,
			  d->params.algo_params.ca_index,
			  d->params.algo_params.algo);
		break;
	default:
		break;
	}

	return ret;
}

static int t5d_dvbdsc_do_ioctl(struct file *file, unsigned int cmd, void *parg)
{
	struct dvb_device *dvbdev = file->private_data;
	struct t5d_sw_dsc *dsc = dvbdev->priv;
	int ret = -EINVAL;

	switch (cmd) {
	case CA_RESET:
		break;
	case CA_GET_CAP: {
		struct ca_caps *cap = parg;
		cap->slot_num = 1;
		cap->slot_type = CA_DESCR;
		cap->descr_num = T5D_DSC_CHANNEL_NUM;
		cap->descr_type = 0;
		break;
	}
	case CA_GET_SLOT_INFO: {
		struct ca_slot_info *slot = parg;
		slot->num = 1;
		slot->type = CA_DESCR;
		slot->flags = 0;
		break;
	}
	case CA_GET_DESCR_INFO: {
		struct ca_descr_info *descr = parg;
		descr->num = T5D_DSC_CHANNEL_NUM;
		descr->type = 0;
		break;
	}
	case CA_SC2_SET_DESCR_EX: {
		ret = t5d_handle_desc_ext(dsc, (struct ca_sc2_descr_ex *)parg);
		break;
	}
	}

	return ret;
}

static int t5d_dvbdsc_usercopy(struct file *file,
			       unsigned int cmd, unsigned long arg,
			       int (*func)(struct file *file,
					       unsigned int cmd, void *arg))
{
	char sbuf[128];
	void *mbuf = NULL;
	void *parg = NULL;
	int err = -EINVAL;

	/* Copy arguments into tmp kernel buffer */
	switch (_IOC_DIR(cmd)) {
	case _IOC_NONE:
		/*
		 * For this command, the pointer is actually an integer
		 * argument.
		 */
		parg = (void *)arg;
		break;
	case _IOC_READ: /* some v4l ioctls are marked wrong ... */
	case _IOC_WRITE:
	case (_IOC_WRITE | _IOC_READ):
		if (_IOC_SIZE(cmd) <= sizeof(sbuf))
			parg = sbuf;
		else {
			/* too big to allocate from stack */
			mbuf = kmalloc(_IOC_SIZE(cmd), GFP_KERNEL);
			if (!mbuf)
				return -ENOMEM;
			parg = mbuf;
		}
		err = -EFAULT;
		if (copy_from_user(parg, (void __user *)arg, _IOC_SIZE(cmd)))
			goto out;
		break;
	}

	/* call driver */
	err = func(file, cmd, parg);
	if (err == -ENOIOCTLCMD)
		err = -ENOTTY;

	if (err < 0)
		goto out;

	/* Copy results into user buffer */
	switch (_IOC_DIR(cmd)) {
	case _IOC_READ:
	case (_IOC_WRITE | _IOC_READ):
		if (copy_to_user((void __user *)arg, parg, _IOC_SIZE(cmd)))
			err = -EFAULT;
		break;
	}

out:
	kfree(mbuf);
	return err;
}

static long t5d_dvbdsc_ioctl(struct file *file, unsigned int cmd,
			     unsigned long arg)
{
	return t5d_dvbdsc_usercopy(file, cmd, arg, t5d_dvbdsc_do_ioctl);
}

#ifdef CONFIG_COMPAT
static long t5d_dvbdsc_compat_ioctl(struct file *file,
				    unsigned int cmd, unsigned long args)
{
	unsigned long ret;

	args = (unsigned long)compat_ptr(args);
	ret = t5d_dvbdsc_ioctl(file, cmd, args);

	return ret;
}
#endif

static const struct file_operations t5d_dvbdsc_fops = {
	.owner = THIS_MODULE,
	.read = NULL,
	.write = NULL,
	.unlocked_ioctl = t5d_dvbdsc_ioctl,
	.open = t5d_dvbdsc_open,
	.release = t5d_dvbdsc_release,
	.poll = NULL,
#ifdef CONFIG_COMPAT
	.compat_ioctl = t5d_dvbdsc_compat_ioctl,
#endif
};

static struct dvb_device t5d_dvbdev_dsc = {
	.priv = NULL,
	.users = 1,
	.readers = 1,
	.writers = 1,
	.fops = &t5d_dvbdsc_fops,
};

/**
 * Initialize the descrambler.
 * @param pdev the platform device handle
 * @param dsc the descrambler handle
 * @retval 0 On success.
 * @retval -1 On error.
 */
int
t5d_dsc_init(
	struct platform_device *pdev,
	struct t5d_sw_dsc *dsc)
{
	struct dvb_adapter *padapter;

	padapter = aml_dvb_get_adapter(&pdev->dev);
	dvb_register_device(padapter, &dsc->dev,
			    &t5d_dvbdev_dsc, dsc, DVB_DEVICE_CA, 0);

	print_dbg("init dsc %#x\n", dsc);

	return 0;
}

/**
 * Release the descrambler.
 * @param dsc the descrambler handle
 * @retval 0 On success.
 * @retval -1 On error.
 */
void
t5d_dsc_deinit(struct t5d_sw_dsc *dsc)
{
	dvb_unregister_device(dsc->dev);
}
