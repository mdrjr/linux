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
#include <linux/debugfs.h>
#include <linux/cdev.h>
#include "t5d_hw_dsc.h"
#include "t5d_demux.h"

#define print_err(fmt, args...)   \
	dprintk(LOG_ERROR, debug_key, fmt, ## args)
#define print_dbg(fmt, args...)   \
	dprintk(LOG_DBG, debug_key, fmt, ## args)

MODULE_PARM_DESC(debug_key, "\n\t\t Enable key debug information");
static int debug_key;
module_param(debug_key, int, 0644);

#define T5D_KEY_DEVICE_NAME "key"
#define DEVICE_INSTANCES 1

struct t5d_key_device {
	struct cdev cdev;
};

static struct t5d_key_device t5d_key_dev;
static dev_t t5d_key_devt;
static struct class *t5d_key_class;
static struct dentry *t5d_key_debug_dent;
u32 old_kt_log_level = 3;

int
t5d_key_open(struct inode *inode, struct file *filep)
{
	struct t5d_key_device *dev;

	dev = container_of(inode->i_cdev, struct t5d_key_device, cdev);
	filep->private_data = dev;

	return 0;
}

int
t5d_key_release(struct inode *inode, struct file *filep)
{
	if (!filep->private_data)
		return 0;

	filep->private_data = NULL;
	t5d_clear_keys();

	return 0;
}

static long
t5d_key_ioctl(struct file *filep, unsigned int cmd, unsigned long arg)
{
	struct key_alloc alloc_param;
	struct key_config cfg_param;
	struct key_descr key_param;
	int ret = 0;
	int i;

	switch (cmd) {
	case KEY_ALLOC:
		memset(&alloc_param, 0, sizeof(alloc_param));
		if (copy_from_user(&alloc_param, (void __user *)arg,
				   sizeof(alloc_param))) {
			return -EFAULT;
		}

		alloc_param.key_index = t5d_alloc_key(alloc_param.is_iv);
		if (alloc_param.key_index == -1) {
			print_err("key alloc failed\n");
			return -EFAULT;
		}

		ret = copy_to_user((void __user *)arg, &alloc_param,
				   sizeof(alloc_param));
		if (unlikely(ret)) {
			return -EFAULT;
		}
		print_dbg("alloc key success, kte:%#x\n", alloc_param.key_index);
		break;
	case KEY_CONFIG:
		memset(&cfg_param, 0, sizeof(cfg_param));
		if (copy_from_user(&cfg_param, (void __user *)arg,
				   sizeof(cfg_param))) {
			return -EFAULT;
		}

		ret = t5d_config_key(cfg_param.key_index,
				     cfg_param.key_userid,
				     cfg_param.key_algo);
		if (ret != 0) {
			print_err("key config failed\n");
			return -EFAULT;
		}
		print_dbg("config key success, kte:%#x, userid:%d, algo:%d\n",
			  cfg_param.key_index,
			  cfg_param.key_userid,
			  cfg_param.key_algo);
		break;
	case KEY_SET:
		memset(&key_param, 0, sizeof(key_param));
		if (copy_from_user(&key_param, (void __user *)arg,
				   sizeof(key_param))) {
			return -EFAULT;
		}

		if (key_param.key_len > 32) {
			print_err("key len error\n");
			return -EFAULT;
		}

		ret = t5d_set_key(key_param.key_index,
				  &key_param.key[0],
				  key_param.key_len);
		if (ret != 0) {
			print_err("set key failed retval=%#x\n", ret);
			return -EFAULT;
		}
		print_dbg("set key success, kte:%#x, key len:%d, ",
			  key_param.key_index,
			  key_param.key_len);
		for (i = 0; i < key_param.key_len; i++)
			print_dbg("0x%02x ", key_param.key[i]);
		break;
	case KEY_FREE:
		ret = t5d_free_key((int)arg);
		if (ret != 0) {
			print_err("free key failed retval=%#x\n", ret);
			return -EFAULT;
		}
		print_dbg("free key success, kte:%#x\n", (int)arg);
		break;
	default:
		print_err("Unknown cmd: %d\n", cmd);
		return -EFAULT;
	}

	return 0;
}

static const struct file_operations t5d_key_fops = {
	.owner = THIS_MODULE,
	.open = t5d_key_open,
	.release = t5d_key_release,
	.unlocked_ioctl = t5d_key_ioctl,
	.compat_ioctl = t5d_key_ioctl
};

/**
 * Init key device
 * @retval 0 On success.
 * @retval 1 On error.
 */
int
t5d_key_init(void)
{
	int ret = 0;
	struct device *device;

	t5d_key_class = class_create(THIS_MODULE, T5D_KEY_DEVICE_NAME);
	if (IS_ERR(t5d_key_class)) {
		print_err("key class_create failed\n");
		ret = PTR_ERR(t5d_key_class);
		return ret;
	}

	if (alloc_chrdev_region(&t5d_key_devt, 0, DEVICE_INSTANCES,
				T5D_KEY_DEVICE_NAME) < 0) {
		print_err("%s device can't be allocated.\n", T5D_KEY_DEVICE_NAME);
		goto destroy;
	}

	cdev_init(&t5d_key_dev.cdev, &t5d_key_fops);
	t5d_key_dev.cdev.owner = THIS_MODULE;
	ret = cdev_add(&t5d_key_dev.cdev,
		       MKDEV(MAJOR(t5d_key_devt), MINOR(t5d_key_devt)), 1);
	if (unlikely(ret < 0)) {
		unregister_chrdev_region(t5d_key_devt, DEVICE_INSTANCES);
		goto destroy;
	}

	device = device_create(t5d_key_class, NULL, t5d_key_devt, NULL,
			       T5D_KEY_DEVICE_NAME);
	if (IS_ERR(device)) {
		print_err("device_create failed\n");
		ret = PTR_ERR(device);
		cdev_del(&t5d_key_dev.cdev);
		goto destroy;
	}

	t5d_key_debug_dent = debugfs_create_dir("t5d_key", NULL);
	if (!t5d_key_debug_dent) {
		print_err("can't create debugfs directory\n");
	}
	debugfs_create_u32("log_level", 0644, t5d_key_debug_dent, &old_kt_log_level);

destroy:
	class_destroy(t5d_key_class);

	return ret;
}

/**
 * DeInit key device
 * @retval 0 On success.
 * @retval 1 On error.
 */
void
t5d_key_deinit(void)
{
	device_destroy(t5d_key_class, t5d_key_devt);
	cdev_del(&t5d_key_dev.cdev);
	unregister_chrdev_region(MKDEV(MAJOR(t5d_key_devt),
				       MINOR(t5d_key_devt)),
				 DEVICE_INSTANCES);
}
