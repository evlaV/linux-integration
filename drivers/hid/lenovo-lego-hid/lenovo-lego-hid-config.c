// SPDX-License-Identifier: GPL-2.0-or-later
/*
 *  HID driver for Lenovo Legion Go devices.
 *
 *  Copyright (c) 2025 Derek J. Clark <derekjohn.clark@gmail.com>
 */
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/array_size.h>
#include <linux/cleanup.h>
#include <linux/completion.h>
#include <linux/delay.h>
#include <linux/dev_printk.h>
#include <linux/device.h>
#include <linux/device.h>
#include <linux/hid.h>
#include <linux/jiffies.h>
#include <linux/kstrtox.h>
#include <linux/led-class-multicolor.h>
#include <linux/mutex.h>
#include <linux/printk.h>
#include <linux/string.h>
#include <linux/sysfs.h>
#include <linux/types.h>
#include <linux/usb.h>
#include <linux/workqueue.h>
#include <linux/workqueue_types.h>

#include "lenovo-lego-hid-core.h"
#include "lenovo-lego-hid-config.h"

struct lego_cfg {
	struct delayed_work lego_cfg_setup;
	struct completion send_cmd_complete;
	struct led_classdev *led_cdev;
	struct hid_device *hdev;
	struct mutex cfg_mutex;
	int last_cmd_ret;
	u8 last_cmd_val;
	u8 rgb_profile;
	u8 rgb_effect;
	u8 rgb_speed;
	u8 rgb_mode;
} drvdata;

enum MCU_COMMAND {
	VIBRATION = 0x67, // MCU_SUBCMD
	MOUSE_DPI = 0x68, // MOUSE_SUBCMD
	GAMEPAD = 0x69, // MCU_SUBCMD
	GYRO = 0x6a, // GYRO_SUBCMD
	TOUCHPAD = 0x6b, // MCU_SUBCMD
	FPS_KEYMAP = 0x6c, // FPS_KEYMAP_SUBCMD
	FPS_MODE = 0x6e, // MCU_SUBCMD
	LIGHTING_EFFECT_EN = 0x70, // LIGHTING_SUBCMD
	LIGHTING_PROFILE = 0x72, // MCU_SUBCMD
	LIGHTING_EFFECT = 0x73, // MCU_SUBCMD
	TRACKPAD = 0x76, // MCU_SUBCMD

};

enum MCU_SUBCMD {
	NONE = 0x00,
	GET_PROPERTY = 0x01,
	SET_PROPERTY = 0x02,
};

enum MOUSE_SUBCMD {
	GET_MOUSE_DPI_LIST = 0x02,
	SET_MOUSE_DPI = 0x03,
	GET_MOUSE_DPI_PROFILE = 0x04,
	SET_MOUSE_DPI_PROFILE = 0x05,
};

enum GYRO_SUBCMD {
	GET_GYRO_STATE = 0x01,
	SET_GYRO_STATE = 0x02,
	GET_GYRO_MODE = 0x04,
	GET_GYRO_MODE_STATUS = 0x05,
	SET_GYRO_MODE_STATUS = 0x06,
};

enum GAMEPAD_MODE {
	XINPUT = 0x01,
	DINPUT,
};

enum GYRO_MODE {
	GYRO_DISABLE,
	GYRO_ATTACHED,
	GYRO_DETACHED,
};

enum GYRO_STATUS {
	GYRO_MODE_DISABLE,
	GYRO_AS_LS,
	GYRO_AS_RS,
};

enum CONTROLLER_DEVICE {
	RECIEVER = 0x01,
	DONGLE,
	GAMEPAD_LEFT,
	GAMEPAD_RIGHT,

};

enum FPS_KEYMAP_SUBCMD {
	GET_FPS_KEYMAP_BUTTON = 0x03,
	SET_FPS_KEYMAP = 0x04,
	GET_FPS_KEYMAP = 0x05,
};

enum LIGHTING_SUBCMD {
	SET_LIGHTING_ENABLED = 0x03,
	GET_LIGHTING_ENABLED = 0x04,
};

struct mcu_version {
	u8 ver1;
	u8 ver2;
	u8 ver3;
	u8 ver4;
} __packed;

struct command_report {
	u8 cmd;
	u8 sub_cmd;
	u8 data[63];
} __packed;

struct lego_cfg_rw_attr {
	u8 mcu_cmd;
	u8 sub_cmd;
};

int lego_cfg_raw_event(u8 *data, int size)
{
	//struct command_report *cmd_rep;

	pr_debug("Got raw event of length: %u, [%*ph]\n", size, size, data);

	if (size != GO_PACKET_SIZE)
		return -EINVAL;

	return 0;
}

//static int lego_cfg_send_cmd(struct hid_device *hdev, u8 *buf, int ep)
//{
//	unsigned char *dmabuf __free(kfree) = NULL;
//	size_t size = GO_PACKET_SIZE;
//	int ret;
//
//	pr_debug("Send data as raw output report: [%*ph]\n", GO_PACKET_SIZE,
//		 buf);
//
//	dmabuf = kmemdup(buf, size, GFP_KERNEL);
//	if (!dmabuf)
//		return -ENOMEM;
//
//	ret = hid_hw_output_report(hdev, dmabuf, size);
//
//	if (ret != size)
//		return -EINVAL;
//
//	return 0;
//}

//static int mcu_property_out(struct hid_device *hdev, enum MCU_COMMAND command,
//			    u8 index, u8 *val, size_t size)
//{
//	u8 outbuf[GO_PACKET_SIZE] = { command, index };
//	int ep = get_endpoint_address(hdev);
//	unsigned int i;
//	int ret;
//
//	if (ep != LEGION_GO_CFG_INTF_IN)
//		return -ENODEV;
//
//	for (i = 0; i < size; i++)
//		outbuf[i + 2] = val[i];
//
//	mutex_lock(&drvdata.cfg_mutex);
//	drvdata.last_cmd_ret = 0;
//	drvdata.last_cmd_val = 0;
//	ret = lego_cfg_send_cmd(hdev, outbuf, ep);
//	if (ret) {
//		mutex_unlock(&drvdata.cfg_mutex);
//		return ret;
//	}
//
//	ret = wait_for_completion_interruptible_timeout(
//		&drvdata.send_cmd_complete, msecs_to_jiffies(5));
//
//	if (ret == 0) /* timeout occured */
//		ret = -EBUSY;
//	if (ret > 0) /* timeout/interrupt didn't occur */
//		ret = 0;
//
//	reinit_completion(&drvdata.send_cmd_complete);
//	mutex_unlock(&drvdata.cfg_mutex);
//	return ret;
//}

void cfg_setup(struct work_struct *work)
{
	//int ret;
}

int lego_cfg_probe(struct hid_device *hdev, const struct hid_device_id *_id)
{
	//int ret;

	mutex_init(&drvdata.cfg_mutex);

	hid_set_drvdata(hdev, &drvdata);

	drvdata.hdev = hdev;

	init_completion(&drvdata.send_cmd_complete);

	/* Executing calls prior to returning from probe will lock the MCU. Schedule
	 * initial data call after probe has completed and MCU can accept calls.
	 */
	INIT_DELAYED_WORK(&drvdata.lego_cfg_setup, &cfg_setup);
	schedule_delayed_work(&drvdata.lego_cfg_setup, msecs_to_jiffies(2));

	pr_info("Initialized config interfce\n");
	return 0;
}

void lego_cfg_remove(struct hid_device *hdev)
{
	cancel_delayed_work_sync(&drvdata.lego_cfg_setup);

	hid_hw_close(hdev);
	hid_hw_stop(hdev);
}
