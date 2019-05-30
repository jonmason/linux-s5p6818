/*
 * linux/drivers/input/touchscreen/ts-if.c
 *
 * Copyright (c) 2011 FriendlyARM (www.arm9.net)
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/input.h>
#include <linux/init.h>
#include <linux/errno.h>
#include <linux/serio.h>
#include <linux/delay.h>
#include <linux/miscdevice.h>

#if defined(CONFIG_DRM_PANEL_FRIENDLYELEC)
extern void panel_get_display_size(int *w, int *h);
#endif

extern void register_ts_if_dev(struct input_dev *dev);

#define S3C_TSVERSION	0x0101
#define DEBUG_LVL		KERN_DEBUG

static struct input_dev *input_dev;
static int abs_x[2] = { 0, 4095 };
static int abs_y[2] = { 0, 4095 };
static char phys[] = "input(ts)";

#define DEVICE_NAME		"ts-if"

#ifdef CONFIG_AUTO_REPORT_1WIRE_INPUT
void onewire_input_report(int x, int y, int pressed)
{
	if (pressed) {
		input_report_key(input_dev, BTN_TOUCH, 1);
		input_report_abs(input_dev, ABS_X, x);
		input_report_abs(input_dev, ABS_Y, y);
		input_report_abs(input_dev, ABS_PRESSURE, 1);
		input_sync(input_dev);
	} else {
		input_report_key(input_dev, BTN_TOUCH, 0);
		input_report_abs(input_dev, ABS_PRESSURE, 0);
		input_sync(input_dev);
	}
}

void onewire_input_set_params(int min_x, int max_x, int min_y, int max_y)
{
	abs_x[0] = min_x;
	abs_x[1] = max_x;

	abs_y[0] = min_y;
	abs_y[1] = max_y;

	if (input_dev) {
		input_set_abs_params(input_dev, ABS_X, min_x, max_x, 0, 0);
		input_set_abs_params(input_dev, ABS_Y, min_y, max_y, 0, 0);
	}
}
#endif

static long _ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	unsigned is_down;

	is_down = (((unsigned)(arg)) >> 31);
	if (is_down) {
		unsigned x, y;

		x = (arg >> 16) & 0x7FFF;
		y = arg & 0x7FFF;
		input_report_abs(input_dev, ABS_X, x);
		input_report_abs(input_dev, ABS_Y, y);

		input_report_key(input_dev, BTN_TOUCH, 1);
		input_report_abs(input_dev, ABS_PRESSURE, 1);
		input_sync(input_dev);
	} else {
		input_report_key(input_dev, BTN_TOUCH, 0);
		input_report_abs(input_dev, ABS_PRESSURE, 0);
		input_sync(input_dev);
	}

	return 0;
}

static struct file_operations dev_fops = {
	.owner	= THIS_MODULE,
	.unlocked_ioctl	= _ioctl,
};

static struct miscdevice misc = {
	.minor	= 185,
	.name	= DEVICE_NAME,
	.fops	= &dev_fops,
};

static int __init dev_init(void)
{
	int width = 1280, height = 720;
	int ret;

	input_dev = input_allocate_device();
	if (!input_dev) {
		ret = -ENOMEM;
		return ret;
	}

#if defined(CONFIG_DRM_PANEL_FRIENDLYELEC)
	panel_get_display_size(&width, &height);
#endif
#ifndef CONFIG_AUTO_REPORT_1WIRE_INPUT
	abs_x[1] = width;
	abs_y[1] = height;
#endif

	input_dev->evbit[0] = BIT_MASK(EV_SYN) | BIT_MASK(EV_KEY) | BIT_MASK(EV_ABS);
	input_dev->keybit[BIT_WORD(BTN_TOUCH)] = BIT_MASK(BTN_TOUCH);

	input_set_abs_params(input_dev, ABS_X, abs_x[0], abs_x[1], 0, 0);
	input_set_abs_params(input_dev, ABS_Y, abs_y[0], abs_y[1], 0, 0);
	input_set_abs_params(input_dev, ABS_PRESSURE, 0, 1, 0, 0);

	input_dev->name = "fa_ts_input";
	input_dev->phys = phys;
	input_dev->id.bustype = BUS_RS232;
	input_dev->id.vendor = 0xDEAD;
	input_dev->id.product = 0xBEEF;
	input_dev->id.version = S3C_TSVERSION;

	/* All went ok, so register to the input system */
	ret = input_register_device(input_dev);
	if (ret) {
		printk("ts-if: Could not register input device(touchscreen)!\n");
		input_free_device(input_dev);
		return ret;
	}

	ret = misc_register(&misc);
	if (ret) {
		input_unregister_device(input_dev);
		input_free_device(input_dev);
		return ret;
	}

	register_ts_if_dev(input_dev);

	printk (DEVICE_NAME"\tinitialized\n");
	return ret;
}

static void __exit dev_exit(void)
{
	input_unregister_device(input_dev);
	misc_deregister(&misc);
}

module_init(dev_init);
module_exit(dev_exit);

MODULE_AUTHOR("FriendlyARM Inc.");
MODULE_DESCRIPTION("OneWire Touch Screen Interface Driver");
MODULE_LICENSE("GPL");

