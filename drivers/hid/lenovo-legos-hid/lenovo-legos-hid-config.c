// SPDX-License-Identifier: GPL-2.0-or-later
/*
 *  HID driver for Lenovo Legion Go S devices.
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

#include "lenovo-legos-hid-core.h"
#include "lenovo-legos-hid-config.h"

struct legos_cfg {
	struct delayed_work legos_cfg_setup;
	struct completion send_cmd_complete;
	struct led_classdev *led_cdev;
	struct hid_device *hdev;
	struct mutex cfg_mutex;
	int last_cmd_ret;
	u8 last_cmd_val;
	u8 mcu_id[12];
	u8 mcu_ver[4];
	u8 rgb_profile;
	u8 rgb_effect;
	u8 rgb_speed;
	u8 rgb_mode;
} drvdata;

/* GET/SET_GAMEPAD_CFG */
enum GAMEPAD_MODE {
	XINPUT,
	DINPUT,
};

static const char *const GAMEPAD_MODE_TEXT[] = {
	[XINPUT] = "xinput",
	[DINPUT] = "dinput",
};

enum FEATURE_ENABLE_STATUS {
	FEATURE_DISABLED,
	FEATURE_ENABLED,
};

static const char *const FEATURE_ENABLE_STATUS_TEXT[] = {
	[FEATURE_DISABLED] = "false",
	[FEATURE_ENABLED] = "true",
};

enum IMU_ENABLED {
	IMU_OFF,
	IMU_ON,
	IMU_OFF_2S,
};

static const char *const IMU_ENABLED_TEXT[] = {
	[IMU_OFF] = "off",
	[IMU_ON] = "on",
	[IMU_OFF_2S] = "off-2sec",
};

enum OS_TYPE {
	WINDOWS,
	LINUX,
};

static const char *const OS_TYPE_TEXT[] = {
	[WINDOWS] = "windows",
	[LINUX] = "linux",
};

enum POLL_RATE {
	HZ125,
	HZ250,
	HZ500,
	HZ1000,
};

static const char *const POLL_RATE_TEXT[] = {
	[HZ125] = "125",
	[HZ250] = "250",
	[HZ500] = "500",
	[HZ1000] = "1000",
};

enum DPAD_MODE {
	DIR8,
	DIR4,
};

static const char *const DPAD_MODE_TEXT[] = {
	[DIR8] = "8-way",
	[DIR4] = "4-way",
};

enum GAMEPAD_CFG_INDEX {
	NONE = 0x00,
	CFG_GAMEPAD_MODE, // GAMEPAD_MODE
	CFG_AUTO_SLP_TIME = 0x04, // 1-255
	CFG_PASS_ENABLE, // FEATURE_ENABLED
	CFG_LIGHT_ENABLE, // FEATURE_ENABLED
	CFG_IMU_ENABLE, // FEATURE_ENABLED
	CFG_TPAD_ENABLE, // FEATURE_ENABLED
	CFG_OS_TYPE = 0x0A, // OS_TYPE
	CFG_POLL_RATE = 0x10, // POLL_RATE
	CFG_DPAD_MODE, // DPAD_MODE
	CFG_MS_WHEEL_STEP, // 1-127
};

/* GET/SET_TP_PARAM */
enum TOUCHPAD_MODE {
	TP_REL,
	TP_ABS,
};

static const char *const TOUCHPAD_MODE_TEXT[] = {
	[TP_REL] = "relative",
	[TP_ABS] = "absolute",
};

enum TOUCHPAD_CFG_INDEX {
	CFG_WINDOWS_MODE = 0x03, // TOUCHPAD_MODE
	CFG_LINUX_MODE, // TOUCHPAD_MODE

};

enum RGB_MODE {
	RGB_MODE_DYNAMIC,
	RGB_MODE_CUSTOM,
};

static const char *const RGB_MODE_TEXT[] = {
	[RGB_MODE_DYNAMIC] = "dynamic",
	[RGB_MODE_CUSTOM] = "custom",
};

enum RGB_EFFECT {
	RGB_EFFECT_MONO,
	RGB_EFFECT_BREATHE,
	RGB_EFFECT_CHROMA,
	RGB_EFFECT_RAINBOW,
};

static const char *const RGB_EFFECT_TEXT[] = {
	[RGB_EFFECT_MONO] = "monocolor",
	[RGB_EFFECT_BREATHE] = "breathe",
	[RGB_EFFECT_CHROMA] = "chroma",
	[RGB_EFFECT_RAINBOW] = "rainbow",
};

/* GET/SET_LIGHT_CFG */
enum LIGHT_CFG_INDEX {
	LIGHT_MODE_SEL = 0x01,
	LIGHT_PROFILE_SEL,
	USR_LIGHT_PROFILE_1,
	USR_LIGHT_PROFILE_2,
	USR_LIGHT_PROFILE_3,
};

enum MCU_COMMAND {
	SEND_HEARTBEAT,
	GET_VERSION,
	GET_MCU_ID,
	GET_GAMEPAD_CFG,
	SET_GAMEPAD_CFG,
	GET_TP_PARAM,
	SET_TP_PARAM,
	GET_MOTOR_CFG,
	SET_MOTOR_CFG,
	GET_TRIGGER_CFG,
	SET_TRIGGER_CFG,
	GET_STICK_CFG,
	SET_STICK_CFG,
	GET_GYRO_CFG,
	SET_GYRO_CFG,
	GET_LIGHT_CFG,
	SET_LIGHT_CFG,
	GET_KEY_MAP,
	SET_KEY_MAP,
	INT_EVENT_REPORT = 0xc0,
	INT_EVENT_CLEAR,
	GET_PL_TEST = 0xdf,
	SET_PL_TEST,
	START_IAP_UPGRADE,
	DBG_CTRL,
	PL_TP_TEST,
	RESTORE_FACTORY,
	IC_RESET,
};

/*GET/SET_PL_TEST */
enum TEST_INDEX {
	TEST_EN = 0x01,
	TEST_TP_MFR, // TP_MANUFACTURER
	TEST_IMU_MFR, // IMU_MANUFACTURER
	TEST_TP_VER, // u8
	MOTOR_F0_CALI = 0x10,
	READ_MOTOR_F0,
	SAVE_MOTOR_F0,
	TEST_LED_L = 0x20,
	TEST_LED_R,
	LED_COLOR_CALI,
	STICK_CALI_TH = 0x30,
	TRIGGER_CALI_TH,
	STICK_CALI_DEAD,
	TRIGGER_CALI_DEAD,
	STICK_CALI_POLARITY,
	TRIGGER_CALI_POLARITY,
	GYRO_CALI_CFG,
	STICK_CALI_TOUT,
	TRIGGER_CALI_TOUT,
};

enum TP_MANUFACTURER {
	TP_NONE,
	TP_BETTERLIFE,
	TP_SIPO,
};

static const char *const TP_MANUFACTURER_TEXT[] = {
	[TP_NONE] = "none",
	[TP_BETTERLIFE] = "BetterLife",
	[TP_SIPO] = "SIPO",
};

enum IMU_MANUFACTURER {
	IMU_NONE,
	IMU_BOSCH,
	IMU_ST,
};

static const char *const IMU_MANUFACTURER_TEXT[] = {
	[IMU_NONE] = "none",
	[IMU_BOSCH] = "Bosch",
	[IMU_ST] = "ST",
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

struct legos_cfg_rw_attr {
	u8 index;
};

int legos_cfg_raw_event(u8 *data, int size)
{
	struct command_report *cmd_rep;

	pr_debug("Got raw event of length: %u, [%*ph]\n", size, size, data);

	if (size != GO_S_PACKET_SIZE)
		return -EINVAL;

	cmd_rep = (struct command_report *)data;
	switch (cmd_rep->cmd) {
	case GET_VERSION:
		drvdata.mcu_ver[0] = cmd_rep->data[2];
		drvdata.mcu_ver[1] = cmd_rep->data[1];
		drvdata.mcu_ver[2] = cmd_rep->data[0];
		drvdata.mcu_ver[3] = cmd_rep->sub_cmd;
		drvdata.last_cmd_ret = 0;
		break;
	case GET_MCU_ID:
		drvdata.mcu_id[0] = cmd_rep->sub_cmd;
		memcpy(&drvdata.mcu_id[1], cmd_rep->data, 11);
		drvdata.last_cmd_ret = 0;
		break;
	case GET_GAMEPAD_CFG:
	case GET_TP_PARAM:
		drvdata.last_cmd_val = cmd_rep->data[0];
		drvdata.last_cmd_ret = 0;
		break;
	case GET_PL_TEST:
		switch (cmd_rep->sub_cmd) {
		case TEST_TP_MFR:
		case TEST_IMU_MFR:
		case TEST_TP_VER:
			drvdata.last_cmd_val = cmd_rep->data[0];
			drvdata.last_cmd_ret = 0;
			break;
		default:
			drvdata.last_cmd_ret = EINVAL;
			break;
		}
		break;
	case GET_LIGHT_CFG:
		switch (cmd_rep->sub_cmd) {
		case LIGHT_MODE_SEL:
			drvdata.rgb_mode = cmd_rep->data[0];
			drvdata.last_cmd_ret = 0;
			break;
		case LIGHT_PROFILE_SEL:
			drvdata.rgb_profile = cmd_rep->data[0];
			drvdata.last_cmd_ret = 0;
			break;
		case USR_LIGHT_PROFILE_1:
		case USR_LIGHT_PROFILE_2:
		case USR_LIGHT_PROFILE_3:
			struct led_classdev_mc *mc_cdev = lcdev_to_mccdev(drvdata.led_cdev);

			drvdata.rgb_effect = cmd_rep->data[0];
			mc_cdev->subled_info[0].intensity = cmd_rep->data[1];
			mc_cdev->subled_info[1].intensity = cmd_rep->data[2];
			mc_cdev->subled_info[2].intensity = cmd_rep->data[3];
			drvdata.led_cdev->brightness = cmd_rep->data[4];
			drvdata.rgb_speed = cmd_rep->data[5];
			drvdata.last_cmd_ret = 0;
			break;
		default:
			drvdata.last_cmd_ret = EINVAL;
			break;
		}
		break;
	case GET_GYRO_CFG:
	case GET_KEY_MAP:
	case GET_MOTOR_CFG:
	case GET_STICK_CFG:
	case GET_TRIGGER_CFG:
		drvdata.last_cmd_ret = EINVAL;
		break;
	case SET_GAMEPAD_CFG:
	case SET_GYRO_CFG:
	case SET_KEY_MAP:
	case SET_LIGHT_CFG:
	case SET_MOTOR_CFG:
	case SET_STICK_CFG:
	case SET_TP_PARAM:
	case SET_TRIGGER_CFG:
		drvdata.last_cmd_ret = cmd_rep->data[0];
		break;
	default:
		drvdata.last_cmd_ret = EINVAL;
		break;
	};

	pr_debug("Last command: %u, sub_cmd: %u, ret: %u, val: %u\n",
		cmd_rep->cmd, cmd_rep->sub_cmd, drvdata.last_cmd_ret,
		drvdata.last_cmd_val);

	complete(&drvdata.send_cmd_complete);
	return -drvdata.last_cmd_ret;
}

static int legos_cfg_send_cmd(struct hid_device *hdev, u8 *buf, int ep)
{
	unsigned char *dmabuf __free(kfree) = NULL;
	size_t size = GO_S_PACKET_SIZE;
	int ret;

	pr_debug("Send data as raw output report: [%*ph]\n", GO_S_PACKET_SIZE, buf);

	dmabuf = kmemdup(buf, size, GFP_KERNEL);
	if (!dmabuf)
		return -ENOMEM;

	ret = hid_hw_output_report(hdev, dmabuf, size);

	if (ret != size)
		return -EINVAL;

	return 0;
}

static int mcu_property_out(struct hid_device *hdev, enum MCU_COMMAND command,
			    u8 index, u8 *val, size_t size)
{
	u8 outbuf[GO_S_PACKET_SIZE] = { command, index };
	int ep = get_endpoint_address(hdev);
	unsigned int i;
	int ret;

	if (ep != LEGION_GO_S_CFG_INTF_IN)
		return -ENODEV;


	for (i = 0; i < size; i++)
		outbuf[i + 2] = val[i];

	mutex_lock(&drvdata.cfg_mutex);
	drvdata.last_cmd_ret = 0;
	drvdata.last_cmd_val = 0;
	ret = legos_cfg_send_cmd(hdev, outbuf, ep);
	if (ret) {
		mutex_unlock(&drvdata.cfg_mutex);
		return ret;
	}

	ret = wait_for_completion_interruptible_timeout(&drvdata.send_cmd_complete,
							msecs_to_jiffies(5));

	if (ret == 0) /* timeout occured */
		ret = -EBUSY;
	if (ret > 0) /* timeout/interrupt didn't occur */
		ret = 0;

	reinit_completion(&drvdata.send_cmd_complete);
	mutex_unlock(&drvdata.cfg_mutex);
	return ret;
}

/* Read-Write Attributes */
static ssize_t gamepad_property_store(struct device *dev,
				      struct device_attribute *attr,
				      const char *buf, size_t count,
				      enum GAMEPAD_CFG_INDEX index)
{
	size_t size = 1;
	u8 val = 0;
	int ret;

	switch (index) {
	case CFG_GAMEPAD_MODE: {
		ret = sysfs_match_string(GAMEPAD_MODE_TEXT, buf);
		if (ret < 0)
			return ret;
		val = ret;
		break;
	}
	case CFG_AUTO_SLP_TIME:
		ret = kstrtou8(buf, 10, &val);
		if (ret)
			return ret;

		if (val < 0 || val > 255)
			return -EINVAL;
		break;
	case CFG_IMU_ENABLE:
		ret = sysfs_match_string(IMU_ENABLED_TEXT, buf);
		if (ret < 0)
			return ret;
		val = ret;
		break;
	case CFG_PASS_ENABLE:
	case CFG_LIGHT_ENABLE:
	case CFG_TPAD_ENABLE:
		ret = sysfs_match_string(FEATURE_ENABLE_STATUS_TEXT, buf);
		if (ret < 0)
			return ret;
		val = ret;
		break;
	case CFG_OS_TYPE:
		ret = sysfs_match_string(OS_TYPE_TEXT, buf);
		if (ret < 0)
			return ret;
		val = ret;
		break;
	case CFG_POLL_RATE:
		ret = sysfs_match_string(POLL_RATE_TEXT, buf);
		if (ret < 0)
			return ret;
		val = ret;
		break;
	case CFG_DPAD_MODE:
		ret = sysfs_match_string(DPAD_MODE_TEXT, buf);
		if (ret < 0)
			return ret;
		val = ret;
		break;
	case CFG_MS_WHEEL_STEP:
		ret = kstrtou8(buf, 10, &val);
		if (ret)
			return ret;
		if (val < 1 || val > 127)
			return -EINVAL;
		break;
	default:
		return -EINVAL;
	}

	if (!val)
		size = 0;

	ret = mcu_property_out(drvdata.hdev, SET_GAMEPAD_CFG, index, &val,
			       size);
	if (ret < 0)
		return ret;

	if (drvdata.last_cmd_ret)
		return -drvdata.last_cmd_ret;

	return count;
}

static ssize_t gamepad_property_show(struct device *dev,
				     struct device_attribute *attr, char *buf,
				     enum GAMEPAD_CFG_INDEX index)
{
	size_t count = 0;
	u8 i;

	count = mcu_property_out(drvdata.hdev, GET_GAMEPAD_CFG, index, 0, 0);
	if (count < 0)
		return count;

	if (drvdata.last_cmd_ret) {
		return -drvdata.last_cmd_ret;
	}

	i = drvdata.last_cmd_val;

	switch (index) {
	case CFG_GAMEPAD_MODE:
		if (i > DINPUT)
			count = -EINVAL;
		else
			count = sysfs_emit(buf, "%s\n", GAMEPAD_MODE_TEXT[i]);
		break;
	case CFG_AUTO_SLP_TIME:
		count = sysfs_emit(buf, "%u\n", i);
		break;
	case CFG_IMU_ENABLE:
		if (i > IMU_OFF_2S)
			count = -EINVAL;
		else
			count = sysfs_emit(buf, "%s\n", IMU_ENABLED_TEXT[i]);
		break;
	case CFG_PASS_ENABLE:
	case CFG_LIGHT_ENABLE:
	case CFG_TPAD_ENABLE:
		if (i > FEATURE_ENABLED)
			count = -EINVAL;
		else
			count = sysfs_emit(buf, "%s\n", FEATURE_ENABLE_STATUS_TEXT[i]);
		break;
	case CFG_OS_TYPE:
		if (i > LINUX)
			count = -EINVAL;
		else
			count = sysfs_emit(buf, "%s\n", OS_TYPE_TEXT[i]);
		break;
	case CFG_POLL_RATE:
		if (i > HZ1000)
			count = -EINVAL;
		else
			count = sysfs_emit(buf, "%s\n", POLL_RATE_TEXT[i]);
		break;
	case CFG_DPAD_MODE:
		if (i > DIR4)
			count = -EINVAL;
		else
			count = sysfs_emit(buf, "%s\n", DPAD_MODE_TEXT[i]);
		break;
	case CFG_MS_WHEEL_STEP:
		if (i < 1 || i > 127)
			count = -EINVAL;
		else
			count = sysfs_emit(buf, "%u\n", i);
		break;
	default:
		count = -EINVAL;
		break;
	}

	return count;
}

static ssize_t gamepad_property_options(struct device *dev,
					struct device_attribute *attr,
					char *buf, enum GAMEPAD_CFG_INDEX index)
{
	size_t count = 0;
	unsigned int i;

	switch (index) {
	case CFG_GAMEPAD_MODE:
		for (i = 0; i < ARRAY_SIZE(GAMEPAD_MODE_TEXT); i++) {
			count += sysfs_emit_at(buf, count, "%s ",
					       GAMEPAD_MODE_TEXT[i]);
		}
		break;
	case CFG_AUTO_SLP_TIME:
		return sysfs_emit(buf, "0-255\n");
	case CFG_IMU_ENABLE:
		for (i = 0; i < ARRAY_SIZE(IMU_ENABLED_TEXT); i++) {
			count += sysfs_emit_at(buf, count, "%s ",
					       IMU_ENABLED_TEXT[i]);
		}
		break;
	case CFG_PASS_ENABLE:
	case CFG_LIGHT_ENABLE:
	case CFG_TPAD_ENABLE:
		for (i = 0; i < ARRAY_SIZE(FEATURE_ENABLE_STATUS_TEXT); i++) {
			count += sysfs_emit_at(buf, count, "%s ",
					       FEATURE_ENABLE_STATUS_TEXT[i]);
		}
		break;
	case CFG_OS_TYPE:
		for (i = 0; i < ARRAY_SIZE(OS_TYPE_TEXT); i++) {
			count += sysfs_emit_at(buf, count, "%s ",
					       OS_TYPE_TEXT[i]);
		}
		break;
	case CFG_POLL_RATE:
		for (i = 0; i < ARRAY_SIZE(POLL_RATE_TEXT); i++) {
			count += sysfs_emit_at(buf, count, "%s ",
					       POLL_RATE_TEXT[i]);
		}
		break;
	case CFG_DPAD_MODE:
		for (i = 0; i < ARRAY_SIZE(DPAD_MODE_TEXT); i++) {
			count += sysfs_emit_at(buf, count, "%s ",
					       DPAD_MODE_TEXT[i]);
		}
		break;
	case CFG_MS_WHEEL_STEP:
		return sysfs_emit(buf, "1-127\n");
	default:
		return count;
	}

	if (count)
		buf[count - 1] = '\n';

	return count;
}

static ssize_t touchpad_property_store(struct device *dev,
				       struct device_attribute *attr,
				       const char *buf, size_t count,
				       enum TOUCHPAD_CFG_INDEX index)
{
	size_t size = 1;
	u8 val = 0;
	int ret;

	switch (index) {
	case CFG_WINDOWS_MODE:
	case CFG_LINUX_MODE:
		ret = sysfs_match_string(TOUCHPAD_MODE_TEXT, buf);
		if (ret < 0)
			return ret;
		val = ret;
		break;
	default:
		return -EINVAL;
	}
	if (!val)
		size = 0;

	ret = mcu_property_out(drvdata.hdev, SET_TP_PARAM, index, &val, size);
	if (ret < 0)
		return ret;

	if (drvdata.last_cmd_ret)
		return -drvdata.last_cmd_ret;

	return count;
}

static ssize_t touchpad_property_show(struct device *dev,
				      struct device_attribute *attr, char *buf,
				      enum TOUCHPAD_CFG_INDEX index)
{
	size_t count = 0;
	u8 i;

	count = mcu_property_out(drvdata.hdev, GET_TP_PARAM, index, 0, 0);
	if (count < 0)
		return count;

	if (drvdata.last_cmd_ret) {
		return -drvdata.last_cmd_ret;
	}

	i = drvdata.last_cmd_val;

	switch (index) {
	case CFG_WINDOWS_MODE:
	case CFG_LINUX_MODE:
		if (i > TP_ABS)
			count = -EINVAL;
		else
			count = sysfs_emit(buf, "%s\n", TOUCHPAD_MODE_TEXT[i]);
		break;
	default:
		count = -EINVAL;
		break;
	}

	return count;
}

static ssize_t touchpad_property_options(struct device *dev,
					 struct device_attribute *attr,
					 char *buf,
					 enum TOUCHPAD_CFG_INDEX index)
{
	size_t count = 0;
	unsigned int i;

	switch (index) {
	case CFG_WINDOWS_MODE:
	case CFG_LINUX_MODE:
		for (i = 0; i < ARRAY_SIZE(TOUCHPAD_MODE_TEXT); i++) {
			count += sysfs_emit_at(buf, count, "%s ",
					       TOUCHPAD_MODE_TEXT[i]);
		}
		break;
	default:
		return count;
	}

	if (count)
		buf[count - 1] = '\n';

	return count;
}

static ssize_t test_property_show(struct device *dev,
				  struct device_attribute *attr, char *buf,
				  enum TEST_INDEX index)
{
	size_t count = 0;
	u8 i;

	count = mcu_property_out(drvdata.hdev, GET_PL_TEST, index, 0, 0);
	if (count < 0)
		return count;

	if (drvdata.last_cmd_ret) {
		return -drvdata.last_cmd_ret;
	}

	i = drvdata.last_cmd_val;

	switch (index) {
	case TEST_TP_MFR:
		if (i > TP_SIPO)
			count = -EINVAL;
		else
			count = sysfs_emit(buf, "%s\n",
					   TP_MANUFACTURER_TEXT[i]);
		break;
	case TEST_IMU_MFR:
		if (i > IMU_ST)
			count = -EINVAL;
		else
			count = sysfs_emit(buf, "%s\n",
					   IMU_MANUFACTURER_TEXT[i]);
		break;
	case TEST_TP_VER:
		count = sysfs_emit(buf, "%u\n", i);
		break;
	default:
		count = -EINVAL;
		break;
	}

	return count;
}

static int mcu_id_get(void)
{
	u8 null_id[12] = {};
	int ret;

	ret = memcmp(&drvdata.mcu_id, &null_id, sizeof(null_id));
	if (ret)
		return -ENOMEM;

	ret = mcu_property_out(drvdata.hdev, GET_MCU_ID, NONE, 0, 0);
	if (ret < 0)
		return ret;

	return -drvdata.last_cmd_ret;
}

static ssize_t mcu_id_show(struct device *dev, struct device_attribute *attr,
			   char *buf)
{
	return sysfs_emit(buf, "%*phN\n", 12, &drvdata.mcu_id);
}

static int mcu_version_get(void)
{
	u8 null_ver[4] = {};
	int ret;

	ret = memcmp(&drvdata.mcu_ver, &null_ver, sizeof(null_ver));
	if (ret)
		return -ENOMEM;

	ret = mcu_property_out(drvdata.hdev, GET_VERSION, NONE, 0, 0);
	if (ret < 0)
		return ret;

	return -drvdata.last_cmd_ret;
}

static ssize_t mcu_version_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%x.%x.%x.%x\n", drvdata.mcu_ver[0],
			  drvdata.mcu_ver[1], drvdata.mcu_ver[2],
			  drvdata.mcu_ver[3]);
}

/* RGB LED */
static int rgb_cfg_call(struct hid_device *hdev, enum MCU_COMMAND cmd,
			enum LIGHT_CFG_INDEX index, u8 *val, size_t size)
{
	int ret;

	if (cmd != GET_LIGHT_CFG && cmd != SET_LIGHT_CFG)
		return -EINVAL;

	if (index < LIGHT_MODE_SEL || index > USR_LIGHT_PROFILE_3)
		return -EINVAL;

	ret = mcu_property_out(hdev, cmd, index, val, size);
	if (ret)
		return ret;

	if (drvdata.last_cmd_ret)
		return -drvdata.last_cmd_ret;

	return 0;
}

static int rgb_profile_call(enum MCU_COMMAND cmd, u8 *rgb_profile, size_t size)
{
	enum LIGHT_CFG_INDEX index;

	if (cmd != SET_LIGHT_CFG && cmd != GET_LIGHT_CFG)
		return -EINVAL;

	index = drvdata.rgb_profile + 2;

	return rgb_cfg_call(drvdata.hdev, cmd, index, rgb_profile, size);
}

static int rgb_write_profile(void)
{
	struct led_classdev_mc *mc_cdev = lcdev_to_mccdev(drvdata.led_cdev);

	u8 rgb_profile[6] = { drvdata.rgb_effect,
			      mc_cdev->subled_info[0].intensity,
			      mc_cdev->subled_info[1].intensity,
			      mc_cdev->subled_info[2].intensity,
			      drvdata.led_cdev->brightness,
			      drvdata.rgb_speed };

	return rgb_profile_call(SET_LIGHT_CFG, rgb_profile, 6);
}

static int rgb_attr_show(void)

{
	int ret;

	ret = rgb_profile_call(GET_LIGHT_CFG, 0, 0);
	if (ret < 0)
		return ret;

	if (drvdata.last_cmd_ret)
		return -drvdata.last_cmd_ret;

	return 0;
};

static int rgb_attr_store(void)
{
	int ret;

	if (drvdata.rgb_mode != RGB_MODE_CUSTOM)
		return -EINVAL;

	ret = rgb_write_profile();
	if (ret < 0)
		return ret;

	if (drvdata.last_cmd_ret)
		return -drvdata.last_cmd_ret;

	return 0;
}

static ssize_t rgb_effect_show(struct device *dev,
			       struct device_attribute *attr, char *buf)
{
	int ret;

	ret = rgb_attr_show();
	if (ret)
		return ret;

	return sysfs_emit(buf, "%s\n", RGB_EFFECT_TEXT[drvdata.rgb_effect]);
}

static ssize_t rgb_effect_store(struct device *dev,
				struct device_attribute *attr, const char *buf,
				size_t count)
{
	int ret;

	ret = sysfs_match_string(RGB_EFFECT_TEXT, buf);
	if (ret < 0)
		return ret;

	drvdata.rgb_effect = ret;

	ret = rgb_attr_store();
	if (ret)
		return ret;

	return count;
};

static ssize_t rgb_effect_index_show(struct device *dev,
				     struct device_attribute *attr, char *buf)
{
	size_t count = 0;
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(RGB_EFFECT_TEXT); i++)
		count += sysfs_emit_at(buf, count, "%s ", RGB_EFFECT_TEXT[i]);

	if (count)
		buf[count - 1] = '\n';

	return count;
}

static ssize_t rgb_speed_show(struct device *dev, struct device_attribute *attr,
			      char *buf)
{
	int ret;

	ret = rgb_attr_show();
	if (ret)
		return ret;

	return sysfs_emit(buf, "%hhu\n", drvdata.rgb_speed);
}

static ssize_t rgb_speed_store(struct device *dev,
			       struct device_attribute *attr, const char *buf,
			       size_t count)
{
	int val = 0;
	int ret;

	ret = kstrtoint(buf, 10, &val);
	if (ret)
		return ret;

	if (val > 100 || val < 0)
		return -EINVAL;

	drvdata.rgb_speed = val;

	ret = rgb_attr_store();
	if (ret)
		return ret;

	return count;
};

static ssize_t rgb_speed_range_show(struct device *dev,
				    struct device_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "0-100\n");
}

static ssize_t rgb_mode_show(struct device *dev, struct device_attribute *attr,
			     char *buf)
{
	int ret;

	ret = rgb_cfg_call(drvdata.hdev, GET_LIGHT_CFG, LIGHT_MODE_SEL, 0, 0);
	if (ret < 0)
		return ret;

	if (drvdata.last_cmd_ret)
		return -drvdata.last_cmd_ret;

	return sysfs_emit(buf, "%s\n", RGB_MODE_TEXT[drvdata.rgb_mode]);
};

static ssize_t rgb_mode_store(struct device *dev, struct device_attribute *attr,
			      const char *buf, size_t count)
{
	size_t size = 1;
	int ret;

	ret = sysfs_match_string(RGB_MODE_TEXT, buf);
	if (ret < 0)
		return ret;

	drvdata.rgb_mode = ret;

	if (!drvdata.rgb_mode)
		size = 0;

	ret = rgb_cfg_call(drvdata.hdev, SET_LIGHT_CFG, LIGHT_MODE_SEL,
			   &drvdata.rgb_mode, size);
	if (ret < 0)
		return ret;

	if (drvdata.last_cmd_ret)
		return -drvdata.last_cmd_ret;

	return count;
};

static ssize_t rgb_mode_index_show(struct device *dev,
				   struct device_attribute *attr, char *buf)
{
	size_t count = 0;
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(RGB_MODE_TEXT); i++)
		count += sysfs_emit_at(buf, count, "%s ", RGB_MODE_TEXT[i]);

	if (count)
		buf[count - 1] = '\n';

	return count;
}

static ssize_t rgb_profile_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	int ret;

	ret = rgb_cfg_call(drvdata.hdev, GET_LIGHT_CFG, LIGHT_PROFILE_SEL, 0,
			   0);
	if (ret < 0)
		return ret;

	if (drvdata.last_cmd_ret)
		return -drvdata.last_cmd_ret;

	return sysfs_emit(buf, "%hhu\n", drvdata.rgb_profile);
};

static ssize_t rgb_profile_store(struct device *dev,
				 struct device_attribute *attr, const char *buf,
				 size_t count)
{
	size_t size = 1;
	int ret;
	u8 val;

	ret = kstrtou8(buf, 10, &val);
	if (ret < 0)
		return ret;

	if (val > 3 || val < 1)
		return -EINVAL;

	drvdata.rgb_profile = val;

	if (!drvdata.rgb_profile)
		size = 0;

	ret = rgb_cfg_call(drvdata.hdev, SET_LIGHT_CFG, LIGHT_PROFILE_SEL,
			   &drvdata.rgb_profile, size);
	if (ret < 0)
		return ret;

	if (drvdata.last_cmd_ret)
		return -drvdata.last_cmd_ret;

	return count;
};

static ssize_t rgb_profile_range_show(struct device *dev,
				      struct device_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "1-3\n");
}

static enum led_brightness legos_rgb_color_get(struct led_classdev *led_cdev)
{
	return led_cdev->brightness;
};

static void legos_rgb_color_set(struct led_classdev *led_cdev,
				enum led_brightness brightness)
{
	int ret;

	led_cdev->brightness = brightness;

	ret = rgb_attr_store();
	if (ret)
		dev_err(led_cdev->dev, "Failed to write RGB profile: %u\n",
			ret);

	if (drvdata.last_cmd_ret)
		dev_err(led_cdev->dev, "Failed to write RGB profile: %u\n",
			drvdata.last_cmd_ret);
};

#define DEVICE_ATTR_RO_NAMED(_name, _attrname)               \
	struct device_attribute dev_attr_##_name = {         \
		.attr = { .name = _attrname, .mode = 0444 }, \
		.show = _name##_show,                        \
	}

#define DEVICE_ATTR_RW_NAMED(_name, _attrname)               \
	struct device_attribute dev_attr_##_name = {         \
		.attr = { .name = _attrname, .mode = 0644 }, \
		.show = _name##_show,                        \
		.store = _name##_store,                      \
	}

#define ATTR_LEGOS_GAMEPAD_RW(_name, _attrname, _rtype)                       \
	static ssize_t _name##_store(struct device *dev,                      \
				     struct device_attribute *attr,           \
				     const char *buf, size_t count)           \
	{                                                                     \
		return gamepad_property_store(dev, attr, buf, count,          \
					      _name.index);                   \
	}                                                                     \
	static ssize_t _name##_show(struct device *dev,                       \
				    struct device_attribute *attr, char *buf) \
	{                                                                     \
		return gamepad_property_show(dev, attr, buf, _name.index);    \
	}                                                                     \
	static ssize_t _name##_##_rtype##_show(                               \
		struct device *dev, struct device_attribute *attr, char *buf) \
	{                                                                     \
		return gamepad_property_options(dev, attr, buf, _name.index); \
	}                                                                     \
	DEVICE_ATTR_RW_NAMED(_name, _attrname)

#define ATTR_LEGOS_TOUCHPAD_RW(_name, _attrname, _rtype)                       \
	static ssize_t _name##_store(struct device *dev,                       \
				     struct device_attribute *attr,            \
				     const char *buf, size_t count)            \
	{                                                                      \
		return touchpad_property_store(dev, attr, buf, count,          \
					       _name.index);                   \
	}                                                                      \
	static ssize_t _name##_show(struct device *dev,                        \
				    struct device_attribute *attr, char *buf)  \
	{                                                                      \
		return touchpad_property_show(dev, attr, buf, _name.index);    \
	}                                                                      \
	static ssize_t _name##_##_rtype##_show(                                \
		struct device *dev, struct device_attribute *attr, char *buf)  \
	{                                                                      \
		return touchpad_property_options(dev, attr, buf, _name.index); \
	}                                                                      \
	DEVICE_ATTR_RW_NAMED(_name, _attrname)

#define ATTR_LEGOS_TEST_RO(_name, _attrname)                                  \
	static ssize_t _name##_show(struct device *dev,                       \
				    struct device_attribute *attr, char *buf) \
	{                                                                     \
		return test_property_show(dev, attr, buf, _name.index);       \
	}                                                                     \
	DEVICE_ATTR_RO_NAMED(_name, _attrname)

/* Gamepad */
struct legos_cfg_rw_attr auto_sleep_time = { CFG_AUTO_SLP_TIME };
struct legos_cfg_rw_attr dpad_mode = { CFG_DPAD_MODE };
struct legos_cfg_rw_attr gamepad_mode = { CFG_GAMEPAD_MODE };
struct legos_cfg_rw_attr gamepad_poll_rate = { CFG_POLL_RATE };

ATTR_LEGOS_GAMEPAD_RW(auto_sleep_time, "auto_sleep_time", range);
ATTR_LEGOS_GAMEPAD_RW(dpad_mode, "dpad_mode", index);
ATTR_LEGOS_GAMEPAD_RW(gamepad_mode, "mode", index);
ATTR_LEGOS_GAMEPAD_RW(gamepad_poll_rate, "poll_rate", index);
static DEVICE_ATTR_RO(auto_sleep_time_range);
static DEVICE_ATTR_RO(dpad_mode_index);
static DEVICE_ATTR_RO_NAMED(gamepad_mode_index, "mode_index");
static DEVICE_ATTR_RO_NAMED(gamepad_poll_rate_index, "poll_rate_index");

static struct attribute *legos_gamepad_attrs[] = {
	&dev_attr_auto_sleep_time.attr,
	&dev_attr_auto_sleep_time_range.attr,
	&dev_attr_dpad_mode.attr,
	&dev_attr_dpad_mode_index.attr,
	&dev_attr_gamepad_mode.attr,
	&dev_attr_gamepad_mode_index.attr,
	&dev_attr_gamepad_poll_rate.attr,
	&dev_attr_gamepad_poll_rate_index.attr,
	NULL,
};

/* IMU */
struct legos_cfg_rw_attr imu_bypass_enabled = { CFG_PASS_ENABLE };
struct legos_cfg_rw_attr imu_manufacturer = { TEST_IMU_MFR };
struct legos_cfg_rw_attr imu_sensor_enabled = { CFG_IMU_ENABLE };

ATTR_LEGOS_GAMEPAD_RW(imu_bypass_enabled, "bypass_enabled", index);
ATTR_LEGOS_GAMEPAD_RW(imu_sensor_enabled, "sensor_enabled", index);
ATTR_LEGOS_TEST_RO(imu_manufacturer, "manufacturer");
static DEVICE_ATTR_RO_NAMED(imu_bypass_enabled_index, "bypass_enabled_index");
static DEVICE_ATTR_RO_NAMED(imu_sensor_enabled_index, "sensor_enabled_index");

static struct attribute *legos_imu_attrs[] = {
	&dev_attr_imu_bypass_enabled.attr,
	&dev_attr_imu_bypass_enabled_index.attr,
	&dev_attr_imu_manufacturer.attr,
	&dev_attr_imu_sensor_enabled.attr,
	&dev_attr_imu_sensor_enabled_index.attr,
	NULL,
};

/* MCU */
struct legos_cfg_rw_attr os_mode = { CFG_OS_TYPE };

ATTR_LEGOS_GAMEPAD_RW(os_mode, "os_mode", index);
static DEVICE_ATTR_RO(os_mode_index);
static DEVICE_ATTR_RO(mcu_id);
static DEVICE_ATTR_RO(mcu_version);

static struct attribute *legos_mcu_attrs[] = {
	&dev_attr_mcu_id.attr,
	&dev_attr_mcu_version.attr,
	&dev_attr_os_mode.attr,
	&dev_attr_os_mode_index.attr,
	NULL,
};

/* Mouse */
struct legos_cfg_rw_attr mouse_wheel_step = { CFG_MS_WHEEL_STEP };

ATTR_LEGOS_GAMEPAD_RW(mouse_wheel_step, "step", range);
static DEVICE_ATTR_RO_NAMED(mouse_wheel_step_range, "step_range");

static struct attribute *legos_mouse_attrs[] = {
	&dev_attr_mouse_wheel_step.attr,
	&dev_attr_mouse_wheel_step_range.attr,
	NULL,
};

/* RGB */
struct legos_cfg_rw_attr rgb_enabled = { CFG_LIGHT_ENABLE };

ATTR_LEGOS_GAMEPAD_RW(rgb_enabled, "enabled", index);
static DEVICE_ATTR_RO_NAMED(rgb_effect_index, "effect_index");
static DEVICE_ATTR_RO_NAMED(rgb_enabled_index, "enabled_index");
static DEVICE_ATTR_RO_NAMED(rgb_mode_index, "mode_index");
static DEVICE_ATTR_RO_NAMED(rgb_profile_range, "profile_range");
static DEVICE_ATTR_RO_NAMED(rgb_speed_range, "speed_range");
static DEVICE_ATTR_RW_NAMED(rgb_effect, "effect");
static DEVICE_ATTR_RW_NAMED(rgb_mode, "mode");
static DEVICE_ATTR_RW_NAMED(rgb_profile, "profile");
static DEVICE_ATTR_RW_NAMED(rgb_speed, "speed");

static struct attribute *legos_rgb_attrs[] = {
	&dev_attr_rgb_effect.attr,
	&dev_attr_rgb_effect_index.attr,
	&dev_attr_rgb_speed.attr,
	&dev_attr_rgb_speed_range.attr,
	&dev_attr_rgb_mode.attr,
	&dev_attr_rgb_mode_index.attr,
	&dev_attr_rgb_profile.attr,
	&dev_attr_rgb_profile_range.attr,
	&dev_attr_rgb_enabled.attr,
	&dev_attr_rgb_enabled_index.attr,
	NULL,
};

/* Touchpad */
struct legos_cfg_rw_attr touchpad_enabled = { CFG_TPAD_ENABLE };
struct legos_cfg_rw_attr touchpad_linux_mode = { CFG_LINUX_MODE };
struct legos_cfg_rw_attr touchpad_manufacturer = { TEST_TP_MFR };
struct legos_cfg_rw_attr touchpad_version = { TEST_TP_VER };
struct legos_cfg_rw_attr touchpad_windows_mode = { CFG_WINDOWS_MODE };

ATTR_LEGOS_GAMEPAD_RW(touchpad_enabled, "enabled", index);
ATTR_LEGOS_TEST_RO(touchpad_manufacturer, "manufacturer");
ATTR_LEGOS_TEST_RO(touchpad_version, "version");
ATTR_LEGOS_TOUCHPAD_RW(touchpad_linux_mode, "linux_mode", index);
ATTR_LEGOS_TOUCHPAD_RW(touchpad_windows_mode, "windows_mode", index);
static DEVICE_ATTR_RO_NAMED(touchpad_enabled_index, "enabled_index");
static DEVICE_ATTR_RO_NAMED(touchpad_linux_mode_index, "linux_mode_index");
static DEVICE_ATTR_RO_NAMED(touchpad_windows_mode_index, "windows_mode_index");

static struct attribute *legos_touchpad_attrs[] = {
	&dev_attr_touchpad_enabled.attr,
	&dev_attr_touchpad_enabled_index.attr,
	&dev_attr_touchpad_linux_mode.attr,
	&dev_attr_touchpad_linux_mode_index.attr,
	&dev_attr_touchpad_manufacturer.attr,
	&dev_attr_touchpad_version.attr,
	&dev_attr_touchpad_windows_mode.attr,
	&dev_attr_touchpad_windows_mode_index.attr,
	NULL,
};

static const struct attribute_group gamepad_attr_group = {
	.name = "gamepad",
	.attrs = legos_gamepad_attrs,
};

static const struct attribute_group imu_attr_group = {
	.name = "imu",
	.attrs = legos_imu_attrs,
};

static const struct attribute_group mcu_attr_group = {
	.attrs = legos_mcu_attrs,
};

static const struct attribute_group mouse_attr_group = {
	.name = "mouse",
	.attrs = legos_mouse_attrs,
};

static struct attribute_group rgb_attr_group = {
	.attrs = legos_rgb_attrs,
};

static const struct attribute_group touchpad_attr_group = {
	.name = "touchpad",
	.attrs = legos_touchpad_attrs,
};

static const struct attribute_group *legos_top_level_attr_groups[] = {
	&gamepad_attr_group, &imu_attr_group,	   &mcu_attr_group,
	&mouse_attr_group,   &touchpad_attr_group, NULL,
};

struct mc_subled legos_rgb_subled_info[] = {
	{
		.color_index = LED_COLOR_ID_RED,
		.brightness = 0x50,
		.intensity = 0x24,
		.channel = 0x1,
	},
	{
		.color_index = LED_COLOR_ID_GREEN,
		.brightness = 0x50,
		.intensity = 0x22,
		.channel = 0x2,
	},
	{
		.color_index = LED_COLOR_ID_BLUE,
		.brightness = 0x50,
		.intensity = 0x99,
		.channel = 0x3,
	},
};

struct led_classdev_mc legos_cdev_rgb = {
	.led_cdev = {
		.name = "go_s:rgb:joystick_rings",
		.brightness = 0x50,
		.max_brightness = 0x64,
		.brightness_set = legos_rgb_color_set,
		.brightness_get = legos_rgb_color_get,
	},
	.num_colors = ARRAY_SIZE(legos_rgb_subled_info),
	.subled_info = legos_rgb_subled_info,
};

void cfg_setup(struct work_struct *work)
{
	int ret;

	ret = mcu_id_get();
	if (ret) {
		dev_err(drvdata.led_cdev->dev,
			"Failed to retrieve MCU ID: %u\n", ret);
		return;
	}

	ret = mcu_version_get();
	if (ret) {
		dev_err(drvdata.led_cdev->dev,
			"Failed to retrieve MCU Version: %u\n", ret);
		return;
	}

	ret = rgb_cfg_call(drvdata.hdev, GET_LIGHT_CFG, LIGHT_MODE_SEL, 0, 0);
	if (ret < 0) {
		dev_err(drvdata.led_cdev->dev,
			"Failed to retrieve RGB Mode: %u\n", ret);
		return;
	}

	ret = rgb_cfg_call(drvdata.hdev, GET_LIGHT_CFG, LIGHT_PROFILE_SEL, 0,
			   0);
	if (ret < 0) {
		dev_err(drvdata.led_cdev->dev,
			"Failed to retrieve RGB Profile: %u\n", ret);
		return;
	}

	ret = rgb_attr_show();
	if (ret < 0) {
		dev_err(drvdata.led_cdev->dev,
			"Failed to retrieve RGB Profile Data: %u\n", ret);
		return;
	}
}

int legos_cfg_probe(struct hid_device *hdev, const struct hid_device_id *_id)
{
	int ret;

	mutex_init(&drvdata.cfg_mutex);

	hid_set_drvdata(hdev, &drvdata);

	drvdata.hdev = hdev;

	ret = sysfs_create_groups(&hdev->dev.kobj, legos_top_level_attr_groups);
	if (ret) {
		dev_err(&hdev->dev,
			"Failed to create gamepad configuration attributes: %u\n",
			ret);
		return ret;
	}

	ret = devm_led_classdev_multicolor_register(&hdev->dev,
						    &legos_cdev_rgb);
	if (ret) {
		dev_err(&hdev->dev, "Failed to create RGB device: %u\n", ret);
		return ret;
	}

	ret = devm_device_add_group(legos_cdev_rgb.led_cdev.dev,
				    &rgb_attr_group);
	if (ret) {
		dev_err(&hdev->dev,
			"Failed to create RGB configuratiion attributes: %u\n",
			ret);
		return ret;
	}

	drvdata.led_cdev = &legos_cdev_rgb.led_cdev;

	init_completion(&drvdata.send_cmd_complete);

	/* Executing calls prior to returning from probe will lock the MCU. Schedule
	 * initial data call after probe has completed and MCU can accept calls.
	 */
	INIT_DELAYED_WORK(&drvdata.legos_cfg_setup, &cfg_setup);
	schedule_delayed_work(&drvdata.legos_cfg_setup, msecs_to_jiffies(2));

	return 0;
}

void legos_cfg_remove(struct hid_device *hdev)
{
	cancel_delayed_work_sync(&drvdata.legos_cfg_setup);
	sysfs_remove_groups(&hdev->dev.kobj, legos_top_level_attr_groups);

	hid_hw_close(hdev);
	hid_hw_stop(hdev);
}
