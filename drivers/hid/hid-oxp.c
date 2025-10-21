// SPDX-License-Identifier: GPL-2.0-or-later
/*
 *  HID driver for OneXPlayer gamepad configuratiion devices.
 *
 *  Copyright (c) 2026 Valve Corporation */
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/array_size.h>
#include <linux/cleanup.h>
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

#include "hid-ids.h"

#define OXP_PACKET_SIZE 64

#define GEN1_MESSAGE_HEADER 0xff
#define GEN1_USAGE_PAGE 0xff01
#define GEN2_MESSAGE_HEADER 0x3f
#define GEN2_USAGE_PAGE 0xff00

enum oxp_function_index {
	OXP_FID_GEN1_RGB_SET = 0x07,
	OXP_FID_GEN1_RGB_REPLY = 0x0f,
	OXP_FID_GEN2_TOGGLE_MODE = 0xb2,
	OXP_FID_GEN2_KEY_STATE = 0xb4,
	OXP_FID_GEN2_RGB_SET = 0xb8,
};

struct oxp_hid_cfg {
	struct led_classdev_mc *led_mc;
	struct hid_device *hdev;
	struct mutex cfg_mutex;
	unsigned char *buf;
	u8 rgb_brightness;
	u8 rgb_effect;
	u8 rgb_speed;
	u8 rgb_en;
} drvdata;

enum oxp_effect_index {
	OXP_UNKNOWN,
	OXP_EFFECT_AURORA,
	OXP_EFFECT_BIRTHDAY,
	OXP_EFFECT_FLOWING,
	OXP_EFFECT_CHROMA_1,
	OXP_EFFECT_NEON,
	OXP_EFFECT_CHROMA_2,
	OXP_EFFECT_DREAMY,
	OXP_EFFECT_WARM,
	OXP_EFFECT_CYBERPUNK,
	OXP_EFFECT_SEA,
	OXP_EFFECT_SUNSET,
	OXP_EFFECT_COLORFUL,
	OXP_EFFECT_MONSTER,
	OXP_EFFECT_GREEN,
	OXP_EFFECT_BLUE,
	OXP_EFFECT_YELLOW,
	OXP_EFFECT_TEAL,
	OXP_EFFECT_PURPLE,
	OXP_EFFECT_FOGGY,
	OXP_EFFECT_MONO_LIST, /* placeholder for effect_index_show */

};

/* These belong to rgb_effect_index, but we want to hide them from
 * rgb_effect_text
 */
#define OXP_GET_PROPERTY 0xfc
#define OXP_SET_PROPERTY 0xfd
#define OXP_EFFECT_MONO_TRUE 0xfe /* actual index for monocolor */

static const char *const oxp_effect_text[] = {
	[OXP_UNKNOWN] = "unknown",
	[OXP_EFFECT_AURORA] = "aurora",
	[OXP_EFFECT_BIRTHDAY] = "birthday_cake",
	[OXP_EFFECT_FLOWING] = "flowing_light",
	[OXP_EFFECT_CHROMA_1] = "chroma_popping",
	[OXP_EFFECT_NEON] = "neon",
	[OXP_EFFECT_CHROMA_2] = "chroma_breathing",
	[OXP_EFFECT_DREAMY] = "dreamy",
	[OXP_EFFECT_WARM] = "warm_sun",
	[OXP_EFFECT_CYBERPUNK] = "cyberpunk",
	[OXP_EFFECT_SEA] = "sea_foam",
	[OXP_EFFECT_SUNSET] = "sunset_afterglow",
	[OXP_EFFECT_COLORFUL] = "colorful",
	[OXP_EFFECT_MONSTER] = "monster_woke",
	[OXP_EFFECT_GREEN] = "green_breathing",
	[OXP_EFFECT_BLUE] = "blue_breathing",
	[OXP_EFFECT_YELLOW] = "yellow_breathing",
	[OXP_EFFECT_TEAL] = "teal_breathing",
	[OXP_EFFECT_PURPLE] = "purple_breathing",
	[OXP_EFFECT_FOGGY] = "foggy_haze",
	[OXP_EFFECT_MONO_LIST] = "monocolor",
};

enum rgb_enabled_index {
	RGB_DISABLED,
	RGB_ENABLED,
};

static const char *const rgb_enabled_text[] = {
	[RGB_DISABLED] = "false",
	[RGB_ENABLED] = "true",
};

struct command_report {
	u8 report_id;
	u8 message_id;
	u8 header_data[2];
	u8 effect;
	u8 enabled;
	u8 speed;
	u8 brightness;
	u8 red;
	u8 green;
	u8 blue;
} __packed;

static u16 get_usage_page(struct hid_device *hdev)
{
	return hdev->collection[0].usage >> 16;
}

static int oxp_hid_raw_event(struct hid_device *hdev, struct hid_report *report,
			     u8 *data, int size)
{
	struct command_report *cmd_rep;
	struct led_classdev_mc *led_mc;
	u16 up = get_usage_page(hdev);

	dev_dbg(&hdev->dev, "raw event data: [%*ph]\n", OXP_PACKET_SIZE, data);

	switch (up) {
	case GEN1_USAGE_PAGE:
		led_mc = drvdata.led_mc;
		cmd_rep = (struct command_report *)data;
		switch (cmd_rep->message_id) {
		case OXP_FID_GEN1_RGB_REPLY:
			/* Ensure we save monocolor as the list value */
			drvdata.rgb_effect =
				cmd_rep->effect == OXP_EFFECT_MONO_TRUE ?
					OXP_EFFECT_MONO_LIST :
					cmd_rep->effect;
			drvdata.rgb_speed = cmd_rep->speed;
			drvdata.rgb_en = cmd_rep->enabled == 0 ? RGB_DISABLED :
								 RGB_ENABLED;
			drvdata.rgb_brightness = cmd_rep->brightness;
			led_mc->led_cdev.brightness =
				cmd_rep->brightness / 4 *
				led_mc->led_cdev.max_brightness;
			/* If monocolor had less than 100% brightness on the previous boot,
			 * there will be no reliable way to determine the real intensity;
			 * since intensity scaling is used with a hardware brightness set at max,
			 * our brightness will always look like 100%. Use the last set value to
			 * prevent successive boots from lowering the brightness further.
			 * Brightness will be "wrong" but the effect will remain the same visually.
			 */
			led_mc->subled_info[0].intensity = cmd_rep->red;
			led_mc->subled_info[1].intensity = cmd_rep->green;
			led_mc->subled_info[2].intensity = cmd_rep->blue;
			return 0;
		default:
			break;
		}
		break;
	case GEN2_USAGE_PAGE:
		dev_dbg(&hdev->dev, "Got response for gen 2 query\n");
		break;
	default:
		break;
	}
	hid_input_report(hdev, HID_INPUT_REPORT, data, size, 1);

	return 0;
}

static int mcu_property_out(u8 *header, size_t header_size, u8 *data,
			    size_t data_size, u8 *footer, size_t footer_size)
{
	size_t message_size = header_size + data_size;
	int ret;

	guard(mutex)(&drvdata.cfg_mutex);
	memcpy(drvdata.buf, header, header_size);
	memcpy(drvdata.buf + header_size, data, data_size);
	memset(drvdata.buf + message_size, 0,
	       OXP_PACKET_SIZE - message_size - footer_size);
	if (footer_size)
		memcpy(drvdata.buf + OXP_PACKET_SIZE - footer_size, footer,
		       footer_size);

	dev_dbg(&drvdata.hdev->dev, "raw data: [%*ph]\n", OXP_PACKET_SIZE,
		drvdata.buf);

	ret = hid_hw_output_report(drvdata.hdev, drvdata.buf, OXP_PACKET_SIZE);
	if (ret < 0)
		return ret;

	/* MCU takes 200ms to be ready for another command. */
	msleep(200);
	return ret == OXP_PACKET_SIZE ? 0 : -EIO;
}

static int gen_1_property_out(enum oxp_function_index fid, u8 *data,
			      u8 data_size)
{
	u8 header[] = { fid, GEN1_MESSAGE_HEADER };
	size_t header_size = ARRAY_SIZE(header);

	return mcu_property_out(header, header_size, data, data_size, 0, 0);
}

static int gen_2_property_out(enum oxp_function_index fid, u8 *data,
			      u8 data_size)
{
	u8 header[] = { fid, GEN2_MESSAGE_HEADER, 0x00 };
	u8 footer[] = { GEN2_MESSAGE_HEADER, fid };
	size_t header_size = ARRAY_SIZE(header);
	size_t footer_size = ARRAY_SIZE(footer);

	return mcu_property_out(header, header_size, data, data_size, footer,
				footer_size);
}

static int oxp_rgb_status_store(u8 enabled, u8 speed, u8 brightness)
{
	u16 up = get_usage_page(drvdata.hdev);
	u8 *data;

	/* Always default to max brightness and use intensity scaling when in
	 * monocolor mode.
	 */
	switch (up) {
	case GEN1_USAGE_PAGE:
		data = (u8[4]){ OXP_SET_PROPERTY, enabled, speed, brightness };
		if (drvdata.rgb_effect == OXP_EFFECT_MONO_LIST)
			data[3] = 0x04;
		return gen_1_property_out(OXP_FID_GEN1_RGB_SET, data, 4);
	case GEN2_USAGE_PAGE:
		data = (u8[6]){ OXP_SET_PROPERTY, 0x00,	 0x02,
				enabled,	  speed, brightness };
		if (drvdata.rgb_effect == OXP_EFFECT_MONO_LIST)
			data[5] = 0x04;
		return gen_2_property_out(OXP_FID_GEN2_RGB_SET, data, 6);
		break;
	default:
		return -ENODEV;
	}
}

static ssize_t oxp_rgb_status_show(void)
{
	u16 up = get_usage_page(drvdata.hdev);
	u8 *data;

	switch (up) {
	case GEN1_USAGE_PAGE:
		data = (u8[1]){ OXP_GET_PROPERTY };
		return gen_1_property_out(OXP_FID_GEN1_RGB_SET, data, 1);
	case GEN2_USAGE_PAGE:
		data = (u8[3]){ OXP_GET_PROPERTY, 0x00, 0x02 };
		return gen_2_property_out(OXP_FID_GEN2_RGB_SET, data, 3);
		break;
	default:
		return -ENODEV;
	}
}

static int oxp_rgb_color_set(void)
{
	u8 max_br = drvdata.led_mc->led_cdev.max_brightness;
	u8 br = drvdata.led_mc->led_cdev.brightness;
	u8 green = br * drvdata.led_mc->subled_info[1].intensity / max_br;
	u8 blue = br * drvdata.led_mc->subled_info[2].intensity / max_br;
	u8 red = br * drvdata.led_mc->subled_info[0].intensity / max_br;
	u16 up = get_usage_page(drvdata.hdev);
	size_t size;
	u8 *data;
	int i;

	switch (up) {
	case GEN1_USAGE_PAGE:
		size = 55;
		data = (u8[55]){ OXP_EFFECT_MONO_TRUE };

		for (i = 1; i < size / 3; i++) {
			data[3 * i] = red;
			data[3 * i + 1] = green;
			data[3 * i + 2] = blue;
		}
		return gen_1_property_out(OXP_FID_GEN1_RGB_SET, data, size);
	case GEN2_USAGE_PAGE:
		size = 57;
		data = (u8[57]){ OXP_EFFECT_MONO_TRUE, 0x00, 0x02 };

		for (i = 3; i < size / 3; i++) {
			data[3 * i] = red;
			data[3 * i + 1] = green;
			data[3 * i + 2] = blue;
		}
		return gen_2_property_out(OXP_FID_GEN2_RGB_SET, data, size);
	default:
		return -ENODEV;
	}
}

static int oxp_rgb_effect_set(u8 effect)
{
	u16 up = get_usage_page(drvdata.hdev);
	u8 *data;
	int ret;

	switch (effect) {
	case OXP_EFFECT_AURORA:
	case OXP_EFFECT_BIRTHDAY:
	case OXP_EFFECT_FLOWING:
	case OXP_EFFECT_CHROMA_1:
	case OXP_EFFECT_NEON:
	case OXP_EFFECT_CHROMA_2:
	case OXP_EFFECT_DREAMY:
	case OXP_EFFECT_WARM:
	case OXP_EFFECT_CYBERPUNK:
	case OXP_EFFECT_SEA:
	case OXP_EFFECT_SUNSET:
	case OXP_EFFECT_COLORFUL:
	case OXP_EFFECT_MONSTER:
	case OXP_EFFECT_GREEN:
	case OXP_EFFECT_BLUE:
	case OXP_EFFECT_YELLOW:
	case OXP_EFFECT_TEAL:
	case OXP_EFFECT_PURPLE:
	case OXP_EFFECT_FOGGY:
		switch (up) {
		case GEN1_USAGE_PAGE:
			data = (u8[1]){ effect };
			ret = gen_1_property_out(OXP_FID_GEN1_RGB_SET, data, 1);
			break;
		case GEN2_USAGE_PAGE:
			data = (u8[3]){ effect, 0x00, 0x02 };
			ret = gen_2_property_out(OXP_FID_GEN2_RGB_SET, data, 3);
			break;
		default:
			ret = ENODEV;
		}
		break;
	case OXP_EFFECT_MONO_LIST:
		ret = oxp_rgb_color_set();
		break;
	default:
		return -EINVAL;
	}

	if (ret)
		return ret;

	drvdata.rgb_effect = effect;

	return 0;
}

static ssize_t enabled_store(struct device *dev, struct device_attribute *attr,
			     const char *buf, size_t count)
{
	int ret;
	u8 val;

	ret = sysfs_match_string(rgb_enabled_text, buf);
	if (ret < 0)
		return ret;
	val = ret;
	ret = oxp_rgb_status_store(val, drvdata.rgb_speed,
				   drvdata.rgb_brightness);
	if (ret)
		return ret;

	drvdata.rgb_en = val;
	return count;
}

static ssize_t enabled_show(struct device *dev, struct device_attribute *attr,
			    char *buf)
{
	int ret;

	ret = oxp_rgb_status_show();
	if (ret)
		return ret;

	if (drvdata.rgb_en >= ARRAY_SIZE(rgb_enabled_text))
		return -EINVAL;

	return sysfs_emit(buf, "%s\n", rgb_enabled_text[drvdata.rgb_en]);
}
static DEVICE_ATTR_RW(enabled);

static ssize_t enabled_index_show(struct device *dev,
				  struct device_attribute *attr, char *buf)
{
	size_t count = 0;
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(rgb_enabled_text); i++) {
		count += sysfs_emit_at(buf, count, "%s ", rgb_enabled_text[i]);
	}
	if (count)
		buf[count - 1] = '\n';

	return count;
}
static DEVICE_ATTR_RO(enabled_index);

static ssize_t effect_store(struct device *dev, struct device_attribute *attr,
			    const char *buf, size_t count)
{
	int ret;
	u8 val;

	ret = sysfs_match_string(oxp_effect_text, buf);
	if (ret < 0)
		return ret;

	val = ret;

	ret = oxp_rgb_status_store(drvdata.rgb_en, drvdata.rgb_speed,
				   drvdata.rgb_brightness);
	if (ret)
		return ret;

	ret = oxp_rgb_effect_set(val);
	if (ret)
		return ret;

	drvdata.rgb_effect = val;
	return count;
}

static ssize_t effect_show(struct device *dev, struct device_attribute *attr,
			   char *buf)
{
	int ret;

	ret = oxp_rgb_status_show();
	if (ret)
		return ret;

	if (drvdata.rgb_effect >= ARRAY_SIZE(oxp_effect_text))
		return -EINVAL;

	return sysfs_emit(buf, "%s\n", oxp_effect_text[drvdata.rgb_effect]);
}

static DEVICE_ATTR_RW(effect);

static ssize_t effect_index_show(struct device *dev,
				 struct device_attribute *attr, char *buf)
{
	size_t count = 0;
	unsigned int i;

	for (i = 1; i < ARRAY_SIZE(oxp_effect_text); i++) {
		count += sysfs_emit_at(buf, count, "%s ", oxp_effect_text[i]);
	}
	if (count)
		buf[count - 1] = '\n';

	return count;
}
static DEVICE_ATTR_RO(effect_index);

static ssize_t speed_store(struct device *dev, struct device_attribute *attr,
			   const char *buf, size_t count)
{
	int ret;
	u8 val;

	ret = kstrtou8(buf, 10, &val);
	if (ret)
		return ret;

	if (val > 9)
		return -EINVAL;

	ret = oxp_rgb_status_store(drvdata.rgb_en, val, drvdata.rgb_brightness);
	if (ret)
		return ret;

	drvdata.rgb_speed = val;
	return count;
}

static ssize_t speed_show(struct device *dev, struct device_attribute *attr,
			  char *buf)
{
	int ret;

	ret = oxp_rgb_status_show();
	if (ret)
		return ret;

	if (drvdata.rgb_speed > 9)
		return -EINVAL;

	return sysfs_emit(buf, "%hhu\n", drvdata.rgb_speed);
}
static DEVICE_ATTR_RW(speed);

static ssize_t speed_range_show(struct device *dev,
				struct device_attribute *attr, char *buf)

{
	return sysfs_emit(buf, "0-9\n");
}
static DEVICE_ATTR_RO(speed_range);

static void oxp_rgb_brightness_set(struct led_classdev *led_cdev,
				   enum led_brightness brightness)
{
	int ret;
	u8 val;
	val = 4 * brightness / drvdata.led_mc->led_cdev.max_brightness;

	ret = oxp_rgb_status_store(drvdata.rgb_en, drvdata.rgb_speed, val);
	if (ret)
		dev_err(led_cdev->dev,
			"Error: Failed to write RGB Status: %i\n", ret);

	led_cdev->brightness = brightness;
	drvdata.rgb_brightness = val;

	ret = oxp_rgb_effect_set(drvdata.rgb_effect);
	if (ret)
		dev_err(led_cdev->dev, "Error: Failed to write RGB color: %i\n",
			ret);
}

static struct attribute *oxp_rgb_attrs[] = {
	&dev_attr_effect.attr,
	&dev_attr_effect_index.attr,
	&dev_attr_enabled.attr,
	&dev_attr_enabled_index.attr,
	&dev_attr_speed.attr,
	&dev_attr_speed_range.attr,
	NULL,
};

static struct attribute_group rgb_attr_group = {
	.attrs = oxp_rgb_attrs,
};

struct mc_subled oxp_rgb_subled_info[] = {
	{
		.color_index = LED_COLOR_ID_RED,
		.intensity = 0x24,
		.channel = 0x1,
	},
	{
		.color_index = LED_COLOR_ID_GREEN,
		.intensity = 0x22,
		.channel = 0x2,
	},
	{
		.color_index = LED_COLOR_ID_BLUE,
		.intensity = 0x99,
		.channel = 0x3,
	},
};

struct led_classdev_mc oxp_cdev_rgb = {
	.led_cdev = {
		.name = "oxp:rgb:joystick_rings",
		.color = LED_COLOR_ID_RGB,
		.brightness = 0x64,
		.max_brightness = 0x64,
		.brightness_set = oxp_rgb_brightness_set,
	},
	.num_colors = ARRAY_SIZE(oxp_rgb_subled_info),
	.subled_info = oxp_rgb_subled_info,
};

static int oxp_rgb_probe(struct hid_device *hdev)
{
	unsigned char *buf;
	int ret;

	buf = devm_kzalloc(&hdev->dev, OXP_PACKET_SIZE, GFP_KERNEL);
	if (!buf) {
		dev_err_probe(&hdev->dev, -ENOMEM,
			      "Failed to allocate memory for device\n");
		return -ENOMEM;
	}

	hid_set_drvdata(hdev, &drvdata);
	drvdata.buf = buf;
	drvdata.hdev = hdev;
	drvdata.led_mc = &oxp_cdev_rgb;
	mutex_init(&drvdata.cfg_mutex);

	ret = oxp_rgb_status_show();
	if (ret)
		dev_err_probe(drvdata.led_mc->led_cdev.dev, ret,
			      "Failed to query RGB state.");

	ret = devm_led_classdev_multicolor_register(&hdev->dev, &oxp_cdev_rgb);
	if (ret) {
		dev_err_probe(&hdev->dev, ret, "Failed to create RGB device\n");
		return ret;
	}

	ret = devm_device_add_group(drvdata.led_mc->led_cdev.dev,
				    &rgb_attr_group);
	if (ret)
		dev_err_probe(
			drvdata.led_mc->led_cdev.dev, ret,
			"Failed to create RGB configuratiion attributes\n");

	return ret;
}

static int oxp_hid_probe(struct hid_device *hdev,
			 const struct hid_device_id *id)
{
	int ret;
	u16 up;

	ret = hid_parse(hdev);
	if (ret) {
		dev_err_probe(&hdev->dev, ret, "Failed to parse HID device\n");
		return ret;
	}

	ret = hid_hw_start(hdev, HID_CONNECT_DEFAULT);
	if (ret) {
		dev_err_probe(&hdev->dev, ret, "Failed to start HID device\n");
		return ret;
	}

	ret = hid_hw_open(hdev);
	if (ret) {
		dev_err_probe(&hdev->dev, ret, "Failed to open HID device\n");
		hid_hw_stop(hdev);
		return ret;
	}

	up = get_usage_page(hdev);
	dev_dbg(&hdev->dev, "Got usage page %04x\n", up);

	switch (up) {
	case GEN1_USAGE_PAGE:
	case GEN2_USAGE_PAGE:
		return oxp_rgb_probe(hdev);
	default:
		return 0;
	}
}

static void oxp_hid_remove(struct hid_device *hdev)
{
	hid_hw_close(hdev);
	hid_hw_stop(hdev);
	hid_set_drvdata(hdev, NULL);
}

static const struct hid_device_id oxp_devices[] = {
	{ HID_USB_DEVICE(USB_VENDOR_ID_CRSC, USB_DEVICE_ID_ONEXPLAYER_GEN1) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_WCH, USB_DEVICE_ID_ONEXPLAYER_GEN2) },
	{}
};

MODULE_DEVICE_TABLE(hid, oxp_devices);
static struct hid_driver hid_oxp = {
	.name = "hid-oxp",
	.id_table = oxp_devices,
	.probe = oxp_hid_probe,
	.remove = oxp_hid_remove,
	.raw_event = oxp_hid_raw_event,
};
module_hid_driver(hid_oxp);

MODULE_AUTHOR("Derek J. Clark");
MODULE_DESCRIPTION("Driver for OneXPlayer HID RGB Interfaces.");
MODULE_LICENSE("GPL");
