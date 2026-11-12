// SPDX-License-Identifier: GPL-2.0-or-later
/*
 *  HID driver for Lenovo Legion Go series gamepads.
 *
 *  Copyright (c) 2026 Derek J. Clark <derekjohn.clark@gmail.com>
 *  Copyright (c) 2026 Valve Corporation
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/array_size.h>
#include <linux/cleanup.h>
#include <linux/completion.h>
#include <linux/container_of.h>
#include <linux/delay.h>
#include <linux/dev_printk.h>
#include <linux/device.h>
#include <linux/device/devres.h>
#include <linux/hid.h>
#include <linux/jiffies.h>
#include <linux/kstrtox.h>
#include <linux/led-class-multicolor.h>
#include <linux/mutex.h>
#include <linux/pm.h>
#include <linux/printk.h>
#include <linux/spinlock.h>
#include <linux/sysfs.h>
#include <linux/types.h>
#include <linux/unaligned.h>
#include <linux/workqueue.h>
#include <linux/workqueue_types.h>

#include "hid-ids.h"

#define GO_VENDOR_USAGE_PAGE	0xFFA0
#define GO_INPUT_REPORT_ID	0x04
#define GO_OUTPUT_REPORT_ID	0x05
#define GO_GP_RESET_SUCCESS	0x01
#define GO_PACKET_SIZE		64
#define GO_COMMAND_TIMEOUT_MS	50
#define GO_GEN1_TIMEOUT_MS		500
#define GO_GEN1_RETRIES		3

/*
 * Fields of an armed command a reply must agree with. Gen1 replies carry
 * a length byte instead of a request id, and some commands echo a pad
 * device rather than the requested one, so the comparisons are opt-in
 * per call.
 */
enum cmd_match {
	CMD_MATCH_ID	= BIT(0),
	CMD_MATCH_CMD	= BIT(1),
	CMD_MATCH_SUB	= BIT(2),
	CMD_MATCH_DEV	= BIT(3),
};

/* Lenovo replies identify a command and a sub-command, but have no sequence. */
struct hid_go_cmd {
	struct completion done;
	spinlock_t lock; /* protects fields below */
	bool pending;
	unsigned int match;
	u8 id;
	u8 command;
	u8 sub_command;
	u8 device;
	int result;
};

enum go_hw_gen {
	GO_GEN1,
	GO_GEN2,
};

enum dev_type {
	UNSPECIFIED,
	USB_MCU,
	TX_DONGLE,
	LEFT_CONTROLLER,
	RIGHT_CONTROLLER,
};

struct go_cfg_drvdata {
	enum go_hw_gen gen;
	unsigned long support;
	struct delayed_work go_cfg_setup;
	struct hid_go_cmd cmd;
	bool gp_registered;
	bool rgb_registered;
	struct led_classdev_mc led_mc;
	struct hid_device *hdev;
	struct mutex cfg_mutex; /*ensure single synchronous output report*/
	u8 gen1_battery[RIGHT_CONTROLLER + 1];
	u8 gen1_link[RIGHT_CONTROLLER + 1];
	u8 gen1_cal_mode;
	u8 gen1_cal_state[RIGHT_CONTROLLER + 1];
	u32 gen1_dpi_current;
	u32 gen1_dpi_values[4];
	u8 fps_mode;
	u8 gp_left_auto_sleep_time;
	u8 gp_left_gyro_cal_status;
	u8 gp_left_joy_cal_status;
	u8 gp_left_notify_en;
	u8 gp_left_rumble_mode;
	u8 gp_left_trigg_cal_status;
	u32 gp_left_version_firmware;
	u8 gp_left_version_gen;
	u32 gp_left_version_hardware;
	u32 gp_left_version_product;
	u32 gp_left_version_protocol;
	u8 gp_mode;
	u8 gp_right_auto_sleep_time;
	u8 gp_right_gyro_cal_status;
	u8 gp_right_joy_cal_status;
	u8 gp_right_notify_en;
	u8 gp_right_rumble_mode;
	u8 gp_right_trigg_cal_status;
	u32 gp_right_version_firmware;
	u8 gp_right_version_gen;
	u32 gp_right_version_hardware;
	u32 gp_right_version_product;
	u32 gp_right_version_protocol;
	u8 gp_rumble_intensity;
	u8 imu_left_bypass_en;
	u8 imu_left_sensor_en;
	u8 imu_right_bypass_en;
	u8 imu_right_sensor_en;
	u32 mcu_version_firmware;
	u8 mcu_version_gen;
	u32 mcu_version_hardware;
	u32 mcu_version_product;
	u32 mcu_version_protocol;
	u32 mouse_dpi;
	u8 os_mode;
	u8 rgb_effect;
	u8 gen1_rgb_quick_en;
	u8 gen1_rgb_quick_id;
	u8 rgb_en;
	u8 rgb_mode;
	u8 rgb_profile;
	u8 rgb_speed;
	u8 tp_en;
	u8 tp_vibration_en;
	u8 tp_vibration_intensity;
	u32 tx_dongle_version_firmware;
	u8 tx_dongle_version_gen;
	u32 tx_dongle_version_hardware;
	u32 tx_dongle_version_product;
	u32 tx_dongle_version_protocol;
};

enum go_support {
	SUP_VERSION,
	SUP_HARDWARE,
	SUP_AUTO_SLEEP,
	SUP_IMU,
	SUP_IMU_BYPASS,
	SUP_DPI,
	SUP_FPS_MODE_DPI,
	SUP_FPS_SWITCH,
	SUP_GAMEPAD_MODE,
	SUP_OS_MODE,
	SUP_RESET,
	SUP_RUMBLE,
	SUP_TOUCHPAD,
	SUP_CAL_JOYSTICK,
	SUP_CAL_GYRO,
	SUP_CAL_TRIGGER,
	SUP_CAL_STICK,
	SUP_RGB,
	SUP_RGB_USR,
	SUP_BATTERY,
	SUP_CONNECTION,
	SUP_RGB_QUICK,
};

#define SUPPORT_GEN1	(BIT(SUP_VERSION) | BIT(SUP_AUTO_SLEEP) | \
			 BIT(SUP_IMU) | BIT(SUP_FPS_MODE_DPI) | \
			 BIT(SUP_TOUCHPAD) | BIT(SUP_RGB) | \
			 BIT(SUP_RGB_QUICK) | BIT(SUP_BATTERY) | \
			 BIT(SUP_CONNECTION) | BIT(SUP_CAL_STICK))
#define SUPPORT_GEN2	(BIT(SUP_VERSION) | BIT(SUP_HARDWARE) | \
			 BIT(SUP_AUTO_SLEEP) | BIT(SUP_IMU) | \
			 BIT(SUP_IMU_BYPASS) | BIT(SUP_DPI) | \
			 BIT(SUP_FPS_MODE_DPI) | BIT(SUP_FPS_SWITCH) | \
			 BIT(SUP_GAMEPAD_MODE) | BIT(SUP_OS_MODE) | \
			 BIT(SUP_RESET) | BIT(SUP_RUMBLE) | \
			 BIT(SUP_TOUCHPAD) | BIT(SUP_CAL_JOYSTICK) | \
			 BIT(SUP_CAL_GYRO) | BIT(SUP_CAL_TRIGGER) | \
			 BIT(SUP_RGB) | BIT(SUP_RGB_USR))

struct go_cfg_attr {
	u8 index;
	unsigned long support;
	struct device_attribute dev_attr;
};

struct command_report {
	u8 report_id;
	u8 id;
	u8 cmd;
	u8 sub_cmd;
	u8 device_type;
	u8 data[59];
} __packed;

enum command_id {
	MCU_CONFIG_DATA = 0x00,
	OS_MODE_DATA = 0x06,
	GAMEPAD_DATA = 0x3c,
};

/*
 * Gen1 config replies: status byte at the start of the payload, an error
 * code six bytes from the frame end on failure.
 */
struct gen1_cmd_data {
	u8 status;
	u8 content[];
};

enum mcu_command_index {
	GET_VERSION_DATA = 0x02,
	GET_FEATURE_STATUS,
	SET_FEATURE_STATUS,
	GET_MOTOR_CFG,
	SET_MOTOR_CFG,
	GET_DPI_CFG,
	SET_DPI_CFG,
	SET_TRIGGER_CFG = 0x0a,
	SET_JOYSTICK_CFG = 0x0c,
	SET_GYRO_CFG = 0x0e,
	GET_RGB_CFG,
	SET_RGB_CFG,
	GET_DEVICE_STATUS = 0xa0,

};

/*
 * Gen1 (2023) config protocol. Replies carry a length byte where gen2
 * carries a request id, so reply matching excludes the id field.
 */
enum gen1_command_id {
	GEN1_GET_AUTO_SLEEP_TIME	= 0x32,
	GEN1_SET_AUTO_SLEEP_TIME	= 0x33,
	GEN1_CAL			= 0x55,
	GEN1_CAL_STOP			= 0x56,
	GEN1_EVT_CAL_DONE		= 0x57,
	GEN1_DPI			= 0x68,
	GEN1_GYRO			= 0x6A,
	GEN1_TOUCHPAD			= 0x6B,
	GEN1_RGB_ENABLE			= 0x70,
	GEN1_RGB_PROFILE		= 0x71,
	GEN1_RGB_SET			= 0x72,
	GEN1_RGB_QUICK			= 0x73,
	GEN1_DEVICE_STATE		= 0x74,
	GEN1_VERSION			= 0x79,
};

enum gen1_dpi_op {
	GEN1_DPI_GET_CURRENT		= 0x01,
	GEN1_DPI_GET_LIST		= 0x02,
	GEN1_DPI_SET			= 0x03,
};

/*
 * Lighting sub commands are scoped to their command; the same value
 * names a different action across the four lighting commands.
 */
enum gen1_rgb_sub {
	GEN1_RGB_ENABLE_GET		= 0x01,
	GEN1_RGB_ENABLE_SET		= 0x02,
	GEN1_RGB_QUICK_EN_SET		= 0x03,
	GEN1_RGB_QUICK_EN_GET		= 0x04,
	GEN1_RGB_PROFILE_GET		= 0x01,
	GEN1_RGB_PROFILE_SET		= 0x01,
	GEN1_RGB_QUICK_GET		= 0x01,
	GEN1_RGB_QUICK_SET		= 0x02,
};

#define GEN1_ACK_OK			0x01
#define GEN1_VAL_INVALID		0x04
#define GEN1_OP_QUERY			0x01
#define GEN1_OP_SET		0x02

enum enabled_status_index {
	FEATURE_UNKNOWN,
	FEATURE_ENABLED,
	FEATURE_DISABLED,
};

static const char *const enabled_status_text[] = {
	[FEATURE_UNKNOWN] = "unknown",
	[FEATURE_ENABLED] = "true",
	[FEATURE_DISABLED] = "false",
};

enum version_data_index {
	PRODUCT_VERSION = 0x02,
	PROTOCOL_VERSION,
	FIRMWARE_VERSION,
	HARDWARE_VERSION,
	HARDWARE_GENERATION,
};

enum feature_status_index {
	FEATURE_RESET_GAMEPAD = 0x02,
	FEATURE_IMU_BYPASS,
	FEATURE_IMU_ENABLE = 0x05,
	FEATURE_TOUCHPAD_ENABLE = 0x07,
	FEATURE_LIGHT_ENABLE,
	FEATURE_AUTO_SLEEP_TIME,
	FEATURE_FPS_SWITCH_STATUS = 0x0b,
	FEATURE_GAMEPAD_MODE = 0x0e,
};

#define FEATURE_OS_MODE 0x69

enum fps_switch_status_index {
	FPS_STATUS_UNKNOWN,
	GAMEPAD,
	FPS,
};

static const char *const fps_switch_text[] = {
	[FPS_STATUS_UNKNOWN] = "unknown",
	[GAMEPAD] = "gamepad",
	[FPS] = "fps",
};

enum gamepad_mode_index {
	GAMEPAD_MODE_UNKNOWN,
	XINPUT,
	DINPUT,
};

static const char *const gamepad_mode_text[] = {
	[GAMEPAD_MODE_UNKNOWN] = "unknown",
	[XINPUT] = "xinput",
	[DINPUT] = "dinput",
};

enum motor_cfg_index {
	MOTOR_CFG_ALL = 0x01,
	MOTOR_INTENSITY,
	VIBRATION_NOTIFY_ENABLE,
	RUMBLE_MODE,
	TP_VIBRATION_ENABLE,
	TP_VIBRATION_INTENSITY,
};

enum intensity_index {
	INTENSITY_UNKNOWN,
	INTENSITY_OFF,
	INTENSITY_LOW,
	INTENSITY_MEDIUM,
	INTENSITY_HIGH,
};

static const char *const intensity_text[] = {
	[INTENSITY_UNKNOWN] = "unknown",
	[INTENSITY_OFF] = "off",
	[INTENSITY_LOW] = "low",
	[INTENSITY_MEDIUM] = "medium",
	[INTENSITY_HIGH] = "high",
};

enum rumble_mode_index {
	RUMBLE_MODE_UNKNOWN,
	RUMBLE_MODE_FPS,
	RUMBLE_MODE_RACE,
	RUMBLE_MODE_AVERAGE,
	RUMBLE_MODE_SPG,
	RUMBLE_MODE_RPG,
};

static const char *const rumble_mode_text[] = {
	[RUMBLE_MODE_UNKNOWN] = "unknown",
	[RUMBLE_MODE_FPS] = "fps",
	[RUMBLE_MODE_RACE] = "racing",
	[RUMBLE_MODE_AVERAGE] = "standard",
	[RUMBLE_MODE_SPG] = "spg",
	[RUMBLE_MODE_RPG] = "rpg",
};

#define FPS_MODE_DPI		0x02
#define TRIGGER_CALIBRATE	0x04
#define JOYSTICK_CALIBRATE	0x04
#define GYRO_CALIBRATE		0x06

enum cal_device_type {
	CALDEV_GYROSCOPE = 0x01,
	CALDEV_JOYSTICK,
	CALDEV_TRIGGER,
	CALDEV_JOY_TRIGGER,
};

enum cal_enable {
	CAL_UNKNOWN,
	CAL_START,
	CAL_STOP,
};

static const char *const cal_enabled_text[] = {
	[CAL_UNKNOWN] = "unknown",
	[CAL_START] = "start",
	[CAL_STOP] = "stop",
};

enum cal_status_index {
	CAL_STAT_UNKNOWN,
	CAL_STAT_SUCCESS,
	CAL_STAT_FAILURE,
};

static const char *const cal_status_text[] = {
	[CAL_STAT_UNKNOWN] = "unknown",
	[CAL_STAT_SUCCESS] = "success",
	[CAL_STAT_FAILURE] = "failure",
};

enum rgb_config_index {
	LIGHT_CFG_ALL = 0x01,
	LIGHT_MODE_SEL,
	LIGHT_PROFILE_SEL,
	USR_LIGHT_PROFILE_1,
	USR_LIGHT_PROFILE_2,
	USR_LIGHT_PROFILE_3,
};

enum rgb_mode_index {
	RGB_MODE_UNKNOWN,
	RGB_MODE_DYNAMIC,
	RGB_MODE_CUSTOM,
};

static const char *const rgb_mode_text[] = {
	[RGB_MODE_UNKNOWN] = "unknown",
	[RGB_MODE_DYNAMIC] = "dynamic",
	[RGB_MODE_CUSTOM] = "custom",
};

enum rgb_effect_index {
	RGB_EFFECT_MONO,
	RGB_EFFECT_BREATHE,
	RGB_EFFECT_CHROMA,
	RGB_EFFECT_RAINBOW,
};

static const char *const rgb_effect_text[] = {
	[RGB_EFFECT_MONO] = "monocolor",
	[RGB_EFFECT_BREATHE] = "breathe",
	[RGB_EFFECT_CHROMA] = "chroma",
	[RGB_EFFECT_RAINBOW] = "rainbow",
};

enum device_status_index {
	GET_CAL_STATUS = 0x02,
	GET_UPGRADE_STATUS,
	GET_MACRO_REC_STATUS,
	GET_HOTKEY_TRIGG_STATUS,
};

enum os_mode_cfg_index {
	SET_OS_MODE = 0x09,
	GET_OS_MODE,
};

enum os_mode_type_index {
	OS_UNKNOWN,
	WINDOWS,
	LINUX,
};

static const char *const os_mode_text[] = {
	[OS_UNKNOWN] = "unknown",
	[WINDOWS] = "windows",
	[LINUX] = "linux",
};

static bool hid_go_cmd_matches(const struct hid_go_cmd *cmd,
			       const struct command_report *rep)
{
	if ((cmd->match & CMD_MATCH_ID) && rep->id != cmd->id)
		return false;

	if ((cmd->match & CMD_MATCH_CMD) && rep->cmd != cmd->command)
		return false;

	if ((cmd->match & CMD_MATCH_SUB) && rep->sub_cmd != cmd->sub_command)
		return false;

	if ((cmd->match & CMD_MATCH_DEV) && rep->device_type != cmd->device)
		return false;

	return true;
}

static void hid_go_cmd_arm(struct go_cfg_drvdata *drvdata, u8 id, u8 command,
			   u8 sub_command, u8 device, unsigned int match)
{
	guard(spinlock_irqsave)(&drvdata->cmd.lock);

	reinit_completion(&drvdata->cmd.done);
	drvdata->cmd.pending = true;
	drvdata->cmd.match = match;
	drvdata->cmd.id = id;
	drvdata->cmd.command = command;
	drvdata->cmd.sub_command = sub_command;
	drvdata->cmd.device = device;
}

static void hid_go_cmd_consume(struct go_cfg_drvdata *drvdata,
			       const struct command_report *cmd_rep, int result)
{
	guard(spinlock_irqsave)(&drvdata->cmd.lock);

	if (drvdata->cmd.pending && hid_go_cmd_matches(&drvdata->cmd, cmd_rep)) {
		drvdata->cmd.pending = false;
		drvdata->cmd.result = result;
		complete(&drvdata->cmd.done);
	}
}

static int hid_go_cmd_finish(struct go_cfg_drvdata *drvdata, long wait_result)
{
	guard(spinlock_irqsave)(&drvdata->cmd.lock);

	if (wait_result <= 0) {
		drvdata->cmd.pending = false;
		return wait_result < 0 ? wait_result : -ETIMEDOUT;
	}

	return drvdata->cmd.result;
}

static int hid_go_cmd_cancel(struct go_cfg_drvdata *drvdata, int result)
{
	guard(spinlock_irqsave)(&drvdata->cmd.lock);

	drvdata->cmd.pending = false;
	return result;
}

static int hid_go_send_output_report(struct hid_device *hdev, u8 *packet)
{
	int ret;

	ret = hid_hw_output_report(hdev, packet, GO_PACKET_SIZE);
	if (ret < 0)
		return ret;
	return ret == GO_PACKET_SIZE ? 0 : -EINVAL;
}

static int hid_go_gen1_errno(u8 code)
{
	if (code == GEN1_VAL_INVALID)
		return -EINVAL;

	return -EIO;
}

/*
 * Classify a gen1 SET ack. A success frame is six bytes with GEN1_ACK_OK
 * first; fail frames are longer and carry an error code six bytes from
 * the frame end.
 */
static int hid_go_gen1_ack(const struct command_report *cmd_rep)
{
	const struct gen1_cmd_data *data =
		(const struct gen1_cmd_data *)cmd_rep->data;
	u8 len = cmd_rep->id;

	if (len < 6 || len >= GO_PACKET_SIZE)
		return -EBADMSG;

	if (data->status == GEN1_ACK_OK)
		return 0;

	return hid_go_gen1_errno(((const u8 *)data)[len - 6]);
}

static int hid_go_gen1_version_event(struct go_cfg_drvdata *drvdata,
				     struct command_report *cmd_rep)
{
	u32 product, protocol, firmware;

	if (cmd_rep->id < 0x15)
		return -EBADMSG;

	product = get_unaligned_le24(&cmd_rep->data[1]);
	protocol = get_unaligned_le24(&cmd_rep->data[4]);
	firmware = get_unaligned_le32(&cmd_rep->data[8]);

	switch (cmd_rep->device_type) {
	case USB_MCU:
		drvdata->mcu_version_product = product;
		drvdata->mcu_version_protocol = protocol;
		drvdata->mcu_version_firmware = firmware;
		break;
	case TX_DONGLE:
		drvdata->tx_dongle_version_product = product;
		drvdata->tx_dongle_version_protocol = protocol;
		drvdata->tx_dongle_version_firmware = firmware;
		break;
	case LEFT_CONTROLLER:
		drvdata->gp_left_version_product = product;
		drvdata->gp_left_version_protocol = protocol;
		drvdata->gp_left_version_firmware = firmware;
		break;
	case RIGHT_CONTROLLER:
		drvdata->gp_right_version_product = product;
		drvdata->gp_right_version_protocol = protocol;
		drvdata->gp_right_version_firmware = firmware;
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static int hid_go_gen1_auto_sleep_event(struct go_cfg_drvdata *drvdata,
					struct command_report *cmd_rep)
{
	if (cmd_rep->id < 6)
		return -EBADMSG;

	switch (cmd_rep->device_type) {
	case LEFT_CONTROLLER:
		drvdata->gp_left_auto_sleep_time = cmd_rep->data[0];
		return 0;
	case RIGHT_CONTROLLER:
		drvdata->gp_right_auto_sleep_time = cmd_rep->data[0];
		return 0;
	default:
		return -EINVAL;
	}
}

static int hid_go_gen1_gyro_event(struct go_cfg_drvdata *drvdata,
				  struct command_report *cmd_rep)
{
	if (cmd_rep->id < 6)
		return -EBADMSG;

	switch (cmd_rep->device_type) {
	case LEFT_CONTROLLER:
		drvdata->imu_left_sensor_en = cmd_rep->data[0];
		return 0;
	case RIGHT_CONTROLLER:
		drvdata->imu_right_sensor_en = cmd_rep->data[0];
		return 0;
	default:
		return -EINVAL;
	}
}

static int hid_go_gen1_dpi_event(struct go_cfg_drvdata *drvdata,
				 struct command_report *cmd_rep)
{
	int i;

	switch (cmd_rep->sub_cmd) {
	case GEN1_DPI_GET_CURRENT:
		if (cmd_rep->id < 9)
			return -EBADMSG;
		drvdata->gen1_dpi_current =
			get_unaligned_be32(&cmd_rep->data[0]);
		return 0;
	case GEN1_DPI_GET_LIST:
		if (cmd_rep->id < 21)
			return -EBADMSG;
		for (i = 0; i < ARRAY_SIZE(drvdata->gen1_dpi_values); i++)
			drvdata->gen1_dpi_values[i] =
				get_unaligned_be32(&cmd_rep->data[4 * i]);
		return 0;
	default:
		return hid_go_gen1_ack(cmd_rep);
	}
}

static int hid_go_gen1_touchpad_event(struct go_cfg_drvdata *drvdata,
				      struct command_report *cmd_rep)
{
	if (cmd_rep->id < 6)
		return -EBADMSG;

	drvdata->tp_en = cmd_rep->data[0];
	return 0;
}

static int hid_go_gen1_rgb_enable_event(struct go_cfg_drvdata *drvdata,
					struct command_report *cmd_rep)
{
	if (cmd_rep->id < 6)
		return -EBADMSG;

	switch (cmd_rep->sub_cmd) {
	case GEN1_RGB_ENABLE_GET:
		drvdata->rgb_en = cmd_rep->data[0];
		return 0;
	case GEN1_RGB_QUICK_EN_GET:
		drvdata->gen1_rgb_quick_en = cmd_rep->data[0];
		return 0;
	default:
		return hid_go_gen1_ack(cmd_rep);
	}
}

static int hid_go_gen1_rgb_profile_event(struct go_cfg_drvdata *drvdata,
					 struct command_report *cmd_rep)
{
	if (cmd_rep->id < 12)
		return -EBADMSG;

	drvdata->rgb_effect = cmd_rep->data[0];
	drvdata->led_mc.subled_info[0].intensity = cmd_rep->data[1];
	drvdata->led_mc.subled_info[1].intensity = cmd_rep->data[2];
	drvdata->led_mc.subled_info[2].intensity = cmd_rep->data[3];
	drvdata->led_mc.led_cdev.brightness = cmd_rep->data[4];
	drvdata->rgb_speed = cmd_rep->data[5];
	drvdata->rgb_profile = cmd_rep->data[6];
	return 0;
}

static int hid_go_gen1_rgb_quick_event(struct go_cfg_drvdata *drvdata,
				       struct command_report *cmd_rep)
{
	if (cmd_rep->id < 6)
		return -EBADMSG;

	switch (cmd_rep->sub_cmd) {
	case GEN1_RGB_QUICK_GET:
		drvdata->gen1_rgb_quick_id = cmd_rep->data[0];
		return 0;
	default:
		return hid_go_gen1_ack(cmd_rep);
	}
}

static int hid_go_gen1_device_state_event(struct go_cfg_drvdata *drvdata,
					  struct command_report *cmd_rep)
{
	/* Link query reply: two trailing bytes, no content echo */
	if (cmd_rep->sub_cmd == GEN1_OP_SET) {
		if (cmd_rep->id < 6)
			return -EBADMSG;

		drvdata->gen1_link[LEFT_CONTROLLER] = cmd_rep->data[0];
		drvdata->gen1_link[RIGHT_CONTROLLER] = cmd_rep->data[1];
		return 0;
	}

	/* Periodic state report: battery levels and link states */
	if (cmd_rep->id < 14)
		return -EBADMSG;

	drvdata->gen1_battery[LEFT_CONTROLLER] = cmd_rep->data[0];
	drvdata->gen1_battery[RIGHT_CONTROLLER] = cmd_rep->data[2];
	drvdata->gen1_link[LEFT_CONTROLLER] = cmd_rep->data[7];
	drvdata->gen1_link[RIGHT_CONTROLLER] = cmd_rep->data[8];

	return 0;
}

static int hid_go_gen1_cal_done_event(struct go_cfg_drvdata *drvdata,
				      struct command_report *cmd_rep
				      __maybe_unused)
{
	int dev;

	for (dev = LEFT_CONTROLLER; dev <= RIGHT_CONTROLLER; dev++)
		drvdata->gen1_cal_state[dev] = 0;

	drvdata->gen1_cal_mode = 0;

	sysfs_notify(&drvdata->hdev->dev.kobj, "left_handle",
		     "calibrate_stick_status");
	sysfs_notify(&drvdata->hdev->dev.kobj, "right_handle",
		     "calibrate_stick_status");

	return 0;
}

static int hid_go_gen1_raw_event(struct go_cfg_drvdata *drvdata,
				 struct command_report *cmd_rep)
{
	switch (cmd_rep->cmd) {
	case GEN1_GET_AUTO_SLEEP_TIME:
		return hid_go_gen1_auto_sleep_event(drvdata, cmd_rep);
	case GEN1_DPI:
		return hid_go_gen1_dpi_event(drvdata, cmd_rep);
	case GEN1_GYRO:
		if (cmd_rep->sub_cmd == GEN1_OP_SET)
			return hid_go_gen1_ack(cmd_rep);
		return hid_go_gen1_gyro_event(drvdata, cmd_rep);
	case GEN1_TOUCHPAD:
		if (cmd_rep->sub_cmd == GEN1_OP_SET)
			return hid_go_gen1_ack(cmd_rep);
		return hid_go_gen1_touchpad_event(drvdata, cmd_rep);
	case GEN1_EVT_CAL_DONE:
		return hid_go_gen1_cal_done_event(drvdata, cmd_rep);
	case GEN1_DEVICE_STATE:
		return hid_go_gen1_device_state_event(drvdata, cmd_rep);
	case GEN1_RGB_ENABLE:
		return hid_go_gen1_rgb_enable_event(drvdata, cmd_rep);
	case GEN1_RGB_PROFILE:
		return hid_go_gen1_rgb_profile_event(drvdata, cmd_rep);
	case GEN1_RGB_SET:
		return hid_go_gen1_ack(cmd_rep);
	case GEN1_RGB_QUICK:
		return hid_go_gen1_rgb_quick_event(drvdata, cmd_rep);
	case GEN1_VERSION:
		return hid_go_gen1_version_event(drvdata, cmd_rep);
	default:
		return hid_go_gen1_ack(cmd_rep);
	}
}

static int hid_go_gen2_version_event(struct go_cfg_drvdata *drvdata,
				     struct command_report *cmd_rep)
{
	switch (cmd_rep->sub_cmd) {
	case PRODUCT_VERSION:
		switch (cmd_rep->device_type) {
		case USB_MCU:
			drvdata->mcu_version_product =
				get_unaligned_be32(cmd_rep->data);
			return 0;
		case TX_DONGLE:
			drvdata->tx_dongle_version_product =
				get_unaligned_be32(cmd_rep->data);
			return 0;
		case LEFT_CONTROLLER:
			drvdata->gp_left_version_product =
				get_unaligned_be32(cmd_rep->data);
			return 0;
		case RIGHT_CONTROLLER:
			drvdata->gp_right_version_product =
				get_unaligned_be32(cmd_rep->data);
			return 0;
		default:
			return -EINVAL;
		}
	case PROTOCOL_VERSION:
		switch (cmd_rep->device_type) {
		case USB_MCU:
			drvdata->mcu_version_protocol =
				get_unaligned_be32(cmd_rep->data);
			return 0;
		case TX_DONGLE:
			drvdata->tx_dongle_version_protocol =
				get_unaligned_be32(cmd_rep->data);
			return 0;
		case LEFT_CONTROLLER:
			drvdata->gp_left_version_protocol =
				get_unaligned_be32(cmd_rep->data);
			return 0;
		case RIGHT_CONTROLLER:
			drvdata->gp_right_version_protocol =
				get_unaligned_be32(cmd_rep->data);
			return 0;
		default:
			return -EINVAL;
		}
	case FIRMWARE_VERSION:
		switch (cmd_rep->device_type) {
		case USB_MCU:
			drvdata->mcu_version_firmware =
				get_unaligned_be32(cmd_rep->data);
			return 0;
		case TX_DONGLE:
			drvdata->tx_dongle_version_firmware =
				get_unaligned_be32(cmd_rep->data);
			return 0;
		case LEFT_CONTROLLER:
			drvdata->gp_left_version_firmware =
				get_unaligned_be32(cmd_rep->data);
			return 0;
		case RIGHT_CONTROLLER:
			drvdata->gp_right_version_firmware =
				get_unaligned_be32(cmd_rep->data);
			return 0;
		default:
			return -EINVAL;
		}
	case HARDWARE_VERSION:
		switch (cmd_rep->device_type) {
		case USB_MCU:
			drvdata->mcu_version_hardware =
				get_unaligned_be32(cmd_rep->data);
			return 0;
		case TX_DONGLE:
			drvdata->tx_dongle_version_hardware =
				get_unaligned_be32(cmd_rep->data);
			return 0;
		case LEFT_CONTROLLER:
			drvdata->gp_left_version_hardware =
				get_unaligned_be32(cmd_rep->data);
			return 0;
		case RIGHT_CONTROLLER:
			drvdata->gp_right_version_hardware =
				get_unaligned_be32(cmd_rep->data);
			return 0;
		default:
			return -EINVAL;
		}
	case HARDWARE_GENERATION:
		switch (cmd_rep->device_type) {
		case USB_MCU:
			drvdata->mcu_version_gen = cmd_rep->data[0];
			return 0;
		case TX_DONGLE:
			drvdata->tx_dongle_version_gen = cmd_rep->data[0];
			return 0;
		case LEFT_CONTROLLER:
			drvdata->gp_left_version_gen = cmd_rep->data[0];
			return 0;
		case RIGHT_CONTROLLER:
			drvdata->gp_right_version_gen = cmd_rep->data[0];
			return 0;
		default:
			return -EINVAL;
		}
	default:
		return -EINVAL;
	}
}

static int hid_go_gen2_feature_status_event(struct go_cfg_drvdata *drvdata,
					    struct command_report *cmd_rep)
{
	switch (cmd_rep->sub_cmd) {
	case FEATURE_RESET_GAMEPAD:
		return 0;
	case FEATURE_IMU_ENABLE:
		switch (cmd_rep->device_type) {
		case LEFT_CONTROLLER:
			drvdata->imu_left_sensor_en = cmd_rep->data[0];
			return 0;
		case RIGHT_CONTROLLER:
			drvdata->imu_right_sensor_en = cmd_rep->data[0];
			return 0;
		default:
			return -EINVAL;
		}
	case FEATURE_IMU_BYPASS:
		switch (cmd_rep->device_type) {
		case LEFT_CONTROLLER:
			drvdata->imu_left_bypass_en = cmd_rep->data[0];
			return 0;
		case RIGHT_CONTROLLER:
			drvdata->imu_right_bypass_en = cmd_rep->data[0];
			return 0;
		default:
			return -EINVAL;
		}
		break;
	case FEATURE_LIGHT_ENABLE:
		drvdata->rgb_en = cmd_rep->data[0];
		return 0;
	case FEATURE_AUTO_SLEEP_TIME:
		switch (cmd_rep->device_type) {
		case LEFT_CONTROLLER:
			drvdata->gp_left_auto_sleep_time = cmd_rep->data[0];
			return 0;
		case RIGHT_CONTROLLER:
			drvdata->gp_right_auto_sleep_time = cmd_rep->data[0];
			return 0;
		default:
			return -EINVAL;
		}
		break;
	case FEATURE_TOUCHPAD_ENABLE:
		drvdata->tp_en = cmd_rep->data[0];
		return 0;
	case FEATURE_GAMEPAD_MODE:
		drvdata->gp_mode = cmd_rep->data[0];
		return 0;
	case FEATURE_FPS_SWITCH_STATUS:
		drvdata->fps_mode = cmd_rep->data[0];
		return 0;
	default:
		return -EINVAL;
	}
}

static int hid_go_gen2_motor_event(struct go_cfg_drvdata *drvdata,
				   struct command_report *cmd_rep)
{
	switch (cmd_rep->sub_cmd) {
	case MOTOR_CFG_ALL:
		return -EINVAL;
	case MOTOR_INTENSITY:
		drvdata->gp_rumble_intensity = cmd_rep->data[0];
		return 0;
	case VIBRATION_NOTIFY_ENABLE:
		switch (cmd_rep->device_type) {
		case LEFT_CONTROLLER:
			drvdata->gp_left_notify_en = cmd_rep->data[0];
			return 0;
		case RIGHT_CONTROLLER:
			drvdata->gp_right_notify_en = cmd_rep->data[0];
			return 0;
		default:
			return -EINVAL;
		}
		break;
	case RUMBLE_MODE:
		switch (cmd_rep->device_type) {
		case LEFT_CONTROLLER:
			drvdata->gp_left_rumble_mode = cmd_rep->data[0];
			return 0;
		case RIGHT_CONTROLLER:
			drvdata->gp_right_rumble_mode = cmd_rep->data[0];
			return 0;
		default:
			return -EINVAL;
		}
	case TP_VIBRATION_ENABLE:
		drvdata->tp_vibration_en = cmd_rep->data[0];
		return 0;
	case TP_VIBRATION_INTENSITY:
		drvdata->tp_vibration_intensity = cmd_rep->data[0];
		return 0;
	}
	return -EINVAL;
}

static int hid_go_gen2_fps_dpi_event(struct go_cfg_drvdata *drvdata,
				     struct command_report *cmd_rep)
{
	if (cmd_rep->sub_cmd != FPS_MODE_DPI)
		return -EINVAL;

	drvdata->mouse_dpi = get_unaligned_le32(cmd_rep->data);

	return 0;
}

static int hid_go_gen2_light_event(struct go_cfg_drvdata *drvdata,
				   struct command_report *cmd_rep)
{
	struct led_classdev_mc *mc_cdev;

	switch (cmd_rep->sub_cmd) {
	case LIGHT_MODE_SEL:
		drvdata->rgb_mode = cmd_rep->data[0];
		return 0;
	case LIGHT_PROFILE_SEL:
		drvdata->rgb_profile = cmd_rep->data[0];
		return 0;
	case USR_LIGHT_PROFILE_1:
	case USR_LIGHT_PROFILE_2:
	case USR_LIGHT_PROFILE_3:
		mc_cdev = &drvdata->led_mc;
		drvdata->rgb_effect = cmd_rep->data[0];
		mc_cdev->subled_info[0].intensity = cmd_rep->data[1];
		mc_cdev->subled_info[1].intensity = cmd_rep->data[2];
		mc_cdev->subled_info[2].intensity = cmd_rep->data[3];
		drvdata->led_mc.led_cdev.brightness = cmd_rep->data[4];
		drvdata->rgb_speed = 100 - cmd_rep->data[5];
		return 0;
	default:
		return -EINVAL;
	}
}

static int hid_go_gen2_device_status_event(struct go_cfg_drvdata *drvdata,
					   struct command_report *cmd_rep)
{
	u8 device_type = cmd_rep->device_type;
	u8 caldev = cmd_rep->data[0];
	u8 status = cmd_rep->data[1];
	const char *attr;

	if (status > CAL_STAT_FAILURE)
		status = CAL_STAT_FAILURE;

	if (device_type != LEFT_CONTROLLER && device_type != RIGHT_CONTROLLER)
		return -EINVAL;

	switch (caldev) {
	case CALDEV_GYROSCOPE:
		if (device_type == LEFT_CONTROLLER)
			drvdata->gp_left_gyro_cal_status = status;
		else
			drvdata->gp_right_gyro_cal_status = status;
		attr = "calibrate_gyro_status";
		break;
	case CALDEV_JOYSTICK:
		if (device_type == LEFT_CONTROLLER)
			drvdata->gp_left_joy_cal_status = status;
		else
			drvdata->gp_right_joy_cal_status = status;
		attr = "calibrate_joystick_status";
		break;
	case CALDEV_TRIGGER:
		if (device_type == LEFT_CONTROLLER)
			drvdata->gp_left_trigg_cal_status = status;
		else
			drvdata->gp_right_trigg_cal_status = status;
		attr = "calibrate_trigger_status";
		break;
	default:
		return -EINVAL;
	}

	if (device_type == LEFT_CONTROLLER)
		sysfs_notify(&drvdata->hdev->dev.kobj, "left_handle", attr);
	else
		sysfs_notify(&drvdata->hdev->dev.kobj, "right_handle", attr);

	return 0;
}
static int hid_go_gen2_os_mode_cfg_event(struct go_cfg_drvdata *drvdata,
					 struct command_report *cmd_rep)
{
	switch (cmd_rep->sub_cmd) {
	case SET_OS_MODE:
		if (cmd_rep->data[0] != 1)
			return -EIO;
		return 0;
	case GET_OS_MODE:
		drvdata->os_mode = cmd_rep->data[0];
		return 0;
	default:
		return -EINVAL;
	}
}

static int hid_go_gen2_set_event_return(struct command_report *cmd_rep)
{
	if (cmd_rep->data[0] != 0)
		return -EIO;

	return 0;
}

static bool hid_go_is_config_iface(struct hid_device *hdev)
{
	struct hid_report *report;
	int i;

	list_for_each_entry(report, &hdev->report_enum[HID_OUTPUT_REPORT].report_list,
			    list) {
		if (report->id != GO_OUTPUT_REPORT_ID)
			continue;

		for (i = 0; i < report->maxfield; i++) {
			if ((report->field[i]->application >> 16) == GO_VENDOR_USAGE_PAGE)
				return true;
		}
	}

	return false;
}

static int hid_go_raw_event(struct hid_device *hdev, struct hid_report *report,
			    u8 *data, int size)
{
	struct go_cfg_drvdata *drvdata = hid_get_drvdata(hdev);
	struct command_report *cmd_rep;
	int ret;

	if (!drvdata || size != GO_PACKET_SIZE || data[0] != GO_INPUT_REPORT_ID)
		goto passthrough;

	cmd_rep = (struct command_report *)data;

	if (drvdata->gen == GO_GEN1) {
		ret = hid_go_gen1_raw_event(drvdata, cmd_rep);
		goto consume;
	}

	switch (cmd_rep->id) {
	case MCU_CONFIG_DATA:
		switch (cmd_rep->cmd) {
		case GET_VERSION_DATA:
			ret = hid_go_gen2_version_event(drvdata, cmd_rep);
			break;
		case GET_FEATURE_STATUS:
			ret = hid_go_gen2_feature_status_event(drvdata, cmd_rep);
			break;
		case GET_MOTOR_CFG:
			ret = hid_go_gen2_motor_event(drvdata, cmd_rep);
			break;
		case GET_DPI_CFG:
			ret = hid_go_gen2_fps_dpi_event(drvdata, cmd_rep);
			break;
		case GET_RGB_CFG:
			ret = hid_go_gen2_light_event(drvdata, cmd_rep);
			break;
		case GET_DEVICE_STATUS:
			ret = hid_go_gen2_device_status_event(drvdata, cmd_rep);
			break;
		case SET_FEATURE_STATUS:
		case SET_MOTOR_CFG:
		case SET_DPI_CFG:
		case SET_RGB_CFG:
		case SET_TRIGGER_CFG:
		case SET_JOYSTICK_CFG:
		case SET_GYRO_CFG:
			ret = hid_go_gen2_set_event_return(cmd_rep);
			break;
		default:
			ret = -EINVAL;
			break;
		}
		break;
	case OS_MODE_DATA:
		ret = hid_go_gen2_os_mode_cfg_event(drvdata, cmd_rep);
		break;
	default:
		goto passthrough;
	}
consume:
	dev_dbg(&hdev->dev, "Rx data as raw input report: [%*ph]\n",
		GO_PACKET_SIZE, data);

	hid_go_cmd_consume(drvdata, cmd_rep, ret);
	return ret;

passthrough:
	/* Forward other HID reports so they generate events */
	hid_input_report(hdev, HID_INPUT_REPORT, data, size, 1);
	return 0;
}

static int mcu_property_out(struct hid_device *hdev, u8 id, u8 command,
			    u8 index, enum dev_type device, u8 *data, size_t len,
			    unsigned int timeout_ms)
{
	struct go_cfg_drvdata *drvdata = hid_get_drvdata(hdev);
	unsigned char *dmabuf __free(kfree) = NULL;
	unsigned int retries = 1;
	unsigned int off, match;
	unsigned long timeout;
	long wait_result;
	int ret, try;

	if (!drvdata)
		return -ENODEV;

	if (drvdata->gen == GO_GEN1) {
		/* Gen1 frame: [report][length][command][sub?][content][ack] */
		off = 3 + (index ? 1 : 0);
		if (off + len + 1 > GO_PACKET_SIZE)
			return -EINVAL;
	} else {
		off = 5;
		if (off + len > GO_PACKET_SIZE)
			return -EINVAL;
	}

	guard(mutex)(&drvdata->cfg_mutex);
	/* We can't use a devm_alloc reusable buffer without side effects during suspend */
	dmabuf = kzalloc(GO_PACKET_SIZE, GFP_KERNEL);
	if (!dmabuf)
		return -ENOMEM;

	if (drvdata->gen == GO_GEN1) {
		dmabuf[0] = GO_OUTPUT_REPORT_ID;
		dmabuf[1] = off + len;
		dmabuf[2] = command;
		if (index)
			dmabuf[3] = index;
		if (len)
			memcpy(dmabuf + off, data, len);
		dmabuf[off + len] = GEN1_ACK_OK;
		match = CMD_MATCH_CMD | (index ? CMD_MATCH_SUB : 0) |
			(device != UNSPECIFIED ? CMD_MATCH_DEV : 0);
		/*
		 * Gen1 replies echo the command, subID, and device but no
		 * request id. The vendor stack repeats a request when a
		 * reply is dropped, so mirror that retry behavior.
		 */
		retries = GO_GEN1_RETRIES;
	} else {
		u8 header[] = { GO_OUTPUT_REPORT_ID, id, command, index, device };

		memcpy(dmabuf, header, ARRAY_SIZE(header));
		memcpy(dmabuf + 5, data, len);
		match = CMD_MATCH_ID | CMD_MATCH_CMD | CMD_MATCH_SUB |
			CMD_MATCH_DEV;
	}
	timeout = msecs_to_jiffies(timeout_ms);

	dev_dbg(&hdev->dev, "Send data as raw output report: [%*ph]\n",
		GO_PACKET_SIZE, dmabuf);

	for (try = 0; try < retries; try++) {
		hid_go_cmd_arm(drvdata, id, command, index, device, match);
		ret = hid_go_send_output_report(hdev, dmabuf);
		if (ret)
			return hid_go_cmd_cancel(drvdata, ret);

		wait_result = wait_for_completion_interruptible_timeout(&drvdata->cmd.done,
									timeout);
		if (wait_result != 0)
			break;
	}

	return hid_go_cmd_finish(drvdata, wait_result);
}

enum gen1_state_index {
	GEN1_STATE_BATTERY,
	GEN1_STATE_CONNECTION,
};

static ssize_t gen1_state_show(struct device *dev,
			       struct device_attribute *attr, char *buf,
			       enum gen1_state_index index,
			       enum dev_type device_type)
{
	struct go_cfg_drvdata *drvdata = hid_get_drvdata(to_hid_device(dev));

	/* Pairs with smp_store_release in the config setup path */
	if (!smp_load_acquire(&drvdata->gp_registered))
		return -ENODEV;

	if (index == GEN1_STATE_CONNECTION) {
		u8 dev_byte = UNSPECIFIED;
		int ret;

		ret = mcu_property_out(drvdata->hdev, 0, GEN1_DEVICE_STATE,
				       GEN1_OP_SET, UNSPECIFIED, &dev_byte,
				       sizeof(dev_byte), GO_GEN1_TIMEOUT_MS);
		if (ret)
			return ret;
	}

	if (index == GEN1_STATE_BATTERY)
		return sysfs_emit(buf, "%u\n",
				  drvdata->gen1_battery[device_type]);

	return sysfs_emit(buf, "%u\n", drvdata->gen1_link[device_type]);
}

static ssize_t version_show(struct device *dev, struct device_attribute *attr,
			    char *buf, enum version_data_index index,
			    enum dev_type device_type)
{
	struct go_cfg_drvdata *drvdata = hid_get_drvdata(to_hid_device(dev));
	ssize_t count = 0;
	int ret;

	/* Pairs with smp_store_release in the config setup path */
	if (!smp_load_acquire(&drvdata->gp_registered))
		return -ENODEV;

	if (drvdata->gen == GO_GEN1)
		ret = mcu_property_out(drvdata->hdev, 0, GEN1_VERSION,
				       GEN1_OP_QUERY, device_type, NULL, 0,
				       GO_GEN1_TIMEOUT_MS);
	else
		ret = mcu_property_out(drvdata->hdev, MCU_CONFIG_DATA,
				       GET_VERSION_DATA, index, device_type,
				       NULL, 0, GO_COMMAND_TIMEOUT_MS);
	if (ret)
		return ret;

	switch (index) {
	case PRODUCT_VERSION:
		switch (device_type) {
		case USB_MCU:
			count = sysfs_emit(buf, "%x\n",
					   drvdata->mcu_version_product);
			break;
		case TX_DONGLE:
			count = sysfs_emit(buf, "%x\n",
					   drvdata->tx_dongle_version_product);
			break;
		case LEFT_CONTROLLER:
			count = sysfs_emit(buf, "%x\n",
					   drvdata->gp_left_version_product);
			break;
		case RIGHT_CONTROLLER:
			count = sysfs_emit(buf, "%x\n",
					   drvdata->gp_right_version_product);
			break;
		default:
			return -EINVAL;
		}
		break;
	case PROTOCOL_VERSION:
		switch (device_type) {
		case USB_MCU:
			count = sysfs_emit(buf, "%x\n",
					   drvdata->mcu_version_protocol);
			break;
		case TX_DONGLE:
			count = sysfs_emit(buf, "%x\n",
					   drvdata->tx_dongle_version_protocol);
			break;
		case LEFT_CONTROLLER:
			count = sysfs_emit(buf, "%x\n",
					   drvdata->gp_left_version_protocol);
			break;
		case RIGHT_CONTROLLER:
			count = sysfs_emit(buf, "%x\n",
					   drvdata->gp_right_version_protocol);
			break;
		default:
			return -EINVAL;
		}
		break;
	case FIRMWARE_VERSION:
		switch (device_type) {
		case USB_MCU:
			count = sysfs_emit(buf, "%x\n",
					   drvdata->mcu_version_firmware);
			break;
		case TX_DONGLE:
			count = sysfs_emit(buf, "%x\n",
					   drvdata->tx_dongle_version_firmware);
			break;
		case LEFT_CONTROLLER:
			count = sysfs_emit(buf, "%x\n",
					   drvdata->gp_left_version_firmware);
			break;
		case RIGHT_CONTROLLER:
			count = sysfs_emit(buf, "%x\n",
					   drvdata->gp_right_version_firmware);
			break;
		default:
			return -EINVAL;
		}
		break;
	case HARDWARE_VERSION:
		switch (device_type) {
		case USB_MCU:
			count = sysfs_emit(buf, "%x\n",
					   drvdata->mcu_version_hardware);
			break;
		case TX_DONGLE:
			count = sysfs_emit(buf, "%x\n",
					   drvdata->tx_dongle_version_hardware);
			break;
		case LEFT_CONTROLLER:
			count = sysfs_emit(buf, "%x\n",
					   drvdata->gp_left_version_hardware);
			break;
		case RIGHT_CONTROLLER:
			count = sysfs_emit(buf, "%x\n",
					   drvdata->gp_right_version_hardware);
			break;
		default:
			return -EINVAL;
		}
		break;
	case HARDWARE_GENERATION:
		switch (device_type) {
		case USB_MCU:
			count = sysfs_emit(buf, "%x\n",
					   drvdata->mcu_version_gen);
			break;
		case TX_DONGLE:
			count = sysfs_emit(buf, "%x\n",
					   drvdata->tx_dongle_version_gen);
			break;
		case LEFT_CONTROLLER:
			count = sysfs_emit(buf, "%x\n",
					   drvdata->gp_left_version_gen);
			break;
		case RIGHT_CONTROLLER:
			count = sysfs_emit(buf, "%x\n",
					   drvdata->gp_right_version_gen);
			break;
		default:
			return -EINVAL;
		}
		break;
	}

	return count;
}

static ssize_t hid_go_gen1_feature_status_store(struct go_cfg_drvdata *drvdata,
						const char *buf, size_t count,
						u8 index,
						enum dev_type device_type)
{
	int ret;

	switch (index) {
	case FEATURE_AUTO_SLEEP_TIME: {
		u8 content[2] = { device_type };

		ret = kstrtou8(buf, 10, &content[1]);
		if (ret)
			return ret;

		ret = mcu_property_out(drvdata->hdev, 0,
				       GEN1_SET_AUTO_SLEEP_TIME, GEN1_OP_QUERY,
				       UNSPECIFIED, content, sizeof(content),
				       GO_GEN1_TIMEOUT_MS);
		if (ret)
			return ret;

		if (device_type == LEFT_CONTROLLER)
			drvdata->gp_left_auto_sleep_time = content[1];
		else
			drvdata->gp_right_auto_sleep_time = content[1];

		return count;
	}
	case FEATURE_IMU_ENABLE: {
		u8 content[2] = { device_type };

		ret = sysfs_match_string(enabled_status_text, buf);
		if (ret < 0)
			return ret;
		content[1] = ret;

		ret = mcu_property_out(drvdata->hdev, 0, GEN1_GYRO,
				       GEN1_OP_SET, UNSPECIFIED, content,
				       sizeof(content), GO_GEN1_TIMEOUT_MS);
		if (ret)
			return ret;

		if (device_type == LEFT_CONTROLLER)
			drvdata->imu_left_sensor_en = content[1];
		else
			drvdata->imu_right_sensor_en = content[1];

		return count;
	}
	case FEATURE_TOUCHPAD_ENABLE: {
		u8 content[2] = { UNSPECIFIED };

		ret = sysfs_match_string(enabled_status_text, buf);
		if (ret < 0)
			return ret;

		content[1] = ret;

		ret = mcu_property_out(drvdata->hdev, 0, GEN1_TOUCHPAD,
				       GEN1_OP_SET, UNSPECIFIED, content,
				       sizeof(content), GO_GEN1_TIMEOUT_MS);
		if (ret)
			return ret;

		drvdata->tp_en = content[1];

		return count;
	}
	case FEATURE_LIGHT_ENABLE: {
		u8 content[2] = { UNSPECIFIED };

		ret = sysfs_match_string(enabled_status_text, buf);
		if (ret < 0)
			return ret;

		content[1] = ret;

		ret = mcu_property_out(drvdata->hdev, 0, GEN1_RGB_ENABLE,
				       GEN1_RGB_ENABLE_SET, UNSPECIFIED,
				       content, sizeof(content),
				       GO_GEN1_TIMEOUT_MS);
		if (ret)
			return ret;

		drvdata->rgb_en = content[1];

		return count;
	}
	default:
		return -EOPNOTSUPP;
	}
}

static ssize_t hid_go_gen1_feature_status_show(struct go_cfg_drvdata *drvdata,
					       char *buf, u8 index,
					       enum dev_type device_type)
{
	u8 dev_byte = device_type;
	int i, ret;

	switch (index) {
	case FEATURE_AUTO_SLEEP_TIME:
		ret = mcu_property_out(drvdata->hdev, 0,
				       GEN1_GET_AUTO_SLEEP_TIME, GEN1_OP_QUERY,
				       device_type, &dev_byte,
				       sizeof(dev_byte), GO_GEN1_TIMEOUT_MS);
		if (ret)
			return ret;

		if (device_type == LEFT_CONTROLLER)
			return sysfs_emit(buf, "%u\n",
					  drvdata->gp_left_auto_sleep_time);

		return sysfs_emit(buf, "%u\n",
				  drvdata->gp_right_auto_sleep_time);
	case FEATURE_IMU_ENABLE:
		ret = mcu_property_out(drvdata->hdev, 0, GEN1_GYRO,
				       GEN1_OP_QUERY, device_type, &dev_byte,
				       sizeof(dev_byte), GO_GEN1_TIMEOUT_MS);
		if (ret)
			return ret;

		if (device_type == LEFT_CONTROLLER)
			i = drvdata->imu_left_sensor_en;
		else
			i = drvdata->imu_right_sensor_en;

		if (i >= ARRAY_SIZE(enabled_status_text))
			return -EINVAL;

		return sysfs_emit(buf, "%s\n", enabled_status_text[i]);
	case FEATURE_TOUCHPAD_ENABLE:
		dev_byte = LEFT_CONTROLLER;
		ret = mcu_property_out(drvdata->hdev, 0, GEN1_TOUCHPAD,
				       GEN1_OP_QUERY, UNSPECIFIED, &dev_byte,
				       sizeof(dev_byte), GO_GEN1_TIMEOUT_MS);
		if (ret)
			return ret;

		i = drvdata->tp_en;
		if (i >= ARRAY_SIZE(enabled_status_text))
			return -EINVAL;

		return sysfs_emit(buf, "%s\n", enabled_status_text[i]);
	case FEATURE_LIGHT_ENABLE:
		dev_byte = UNSPECIFIED;
		ret = mcu_property_out(drvdata->hdev, 0, GEN1_RGB_ENABLE,
				       GEN1_RGB_ENABLE_GET, UNSPECIFIED,
				       &dev_byte, sizeof(dev_byte),
				       GO_GEN1_TIMEOUT_MS);
		if (ret)
			return ret;

		i = drvdata->rgb_en;
		if (i >= ARRAY_SIZE(enabled_status_text))
			return -EINVAL;

		return sysfs_emit(buf, "%s\n", enabled_status_text[i]);
	default:
		return -EOPNOTSUPP;
	}
}

static ssize_t feature_status_store(struct device *dev,
				    struct device_attribute *attr,
				    const char *buf, size_t count,
				    enum feature_status_index index,
				    enum dev_type device_type)
{
	struct go_cfg_drvdata *drvdata;
	bool dev_registered;
	size_t size = 1;
	u8 val = 0;
	int ret;

	/* rgb_enabled is attached to the led_classdev, not the hid_device */
	if (index == FEATURE_LIGHT_ENABLE) {
		struct led_classdev *led_cdev = dev_get_drvdata(dev);
		struct led_classdev_mc *mc_cdev = lcdev_to_mccdev(led_cdev);

		drvdata = container_of(mc_cdev, struct go_cfg_drvdata, led_mc);
		/* Pairs with smp_store_release in the config setup path */
		dev_registered = smp_load_acquire(&drvdata->rgb_registered);
	} else {
		drvdata = hid_get_drvdata(to_hid_device(dev));
		/* Pairs with smp_store_release in the config setup path */
		dev_registered = smp_load_acquire(&drvdata->gp_registered);
	}

	if (!dev_registered)
		return -ENODEV;

	switch (drvdata->gen) {
	case GO_GEN1:
		return hid_go_gen1_feature_status_store(drvdata, buf, count,
							index, device_type);
	case GO_GEN2:
		break;
	default:
		return -EINVAL;
	}

	switch (index) {
	case FEATURE_IMU_ENABLE:
	case FEATURE_IMU_BYPASS:
	case FEATURE_LIGHT_ENABLE:
	case FEATURE_TOUCHPAD_ENABLE:
		ret = sysfs_match_string(enabled_status_text, buf);
		val = ret;
		break;
	case FEATURE_AUTO_SLEEP_TIME:
		ret = kstrtou8(buf, 10, &val);
		break;
	case FEATURE_RESET_GAMEPAD:
		ret = kstrtou8(buf, 10, &val);
		if (val != GO_GP_RESET_SUCCESS)
			return -EINVAL;
		break;
	case FEATURE_FPS_SWITCH_STATUS:
		ret = sysfs_match_string(fps_switch_text, buf);
		val = ret;
		break;
	case FEATURE_GAMEPAD_MODE:
		ret = sysfs_match_string(gamepad_mode_text, buf);
		val = ret;
		break;
	default:
		return -EINVAL;
	}

	if (ret < 0)
		return ret;

	if (!val)
		size = 0;

	ret = mcu_property_out(drvdata->hdev, MCU_CONFIG_DATA,
			       SET_FEATURE_STATUS, index, device_type, &val,
			       size,
			       GO_COMMAND_TIMEOUT_MS);
	if (ret < 0)
		return ret;

	return count;
}

static ssize_t feature_status_show(struct device *dev,
				   struct device_attribute *attr, char *buf,
				   enum feature_status_index index,
				   enum dev_type device_type)
{
	struct go_cfg_drvdata *drvdata;
	bool dev_registered;
	ssize_t count = 0;
	int ret;
	u8 i;

	/* rgb_enabled is attached to the led_classdev, not the hid_device */
	if (index == FEATURE_LIGHT_ENABLE) {
		struct led_classdev *led_cdev = dev_get_drvdata(dev);
		struct led_classdev_mc *mc_cdev = lcdev_to_mccdev(led_cdev);

		drvdata = container_of(mc_cdev, struct go_cfg_drvdata, led_mc);
		/* Pairs with smp_store_release in the config setup path */
		dev_registered = smp_load_acquire(&drvdata->rgb_registered);
	} else {
		drvdata = hid_get_drvdata(to_hid_device(dev));
		/* Pairs with smp_store_release in the config setup path */
		dev_registered = smp_load_acquire(&drvdata->gp_registered);
	}

	if (!dev_registered)
		return -ENODEV;

	switch (drvdata->gen) {
	case GO_GEN1:
		return hid_go_gen1_feature_status_show(drvdata, buf, index,
						       device_type);
	case GO_GEN2:
		break;
	default:
		return -EINVAL;
	}

	ret = mcu_property_out(drvdata->hdev, MCU_CONFIG_DATA,
			       GET_FEATURE_STATUS, index, device_type, NULL, 0,
			       GO_COMMAND_TIMEOUT_MS);
	if (ret)
		return ret;

	switch (index) {
	case FEATURE_IMU_ENABLE:
		switch (device_type) {
		case LEFT_CONTROLLER:
			i = drvdata->imu_left_sensor_en;
			break;
		case RIGHT_CONTROLLER:
			i = drvdata->imu_right_sensor_en;
			break;
		default:
			return -EINVAL;
		}
		if (i >= ARRAY_SIZE(enabled_status_text))
			return -EINVAL;

		count = sysfs_emit(buf, "%s\n", enabled_status_text[i]);
		break;
	case FEATURE_IMU_BYPASS:
		switch (device_type) {
		case LEFT_CONTROLLER:
			i = drvdata->imu_left_bypass_en;
			break;
		case RIGHT_CONTROLLER:
			i = drvdata->imu_right_bypass_en;
			break;
		default:
			return -EINVAL;
		}
		if (i >= ARRAY_SIZE(enabled_status_text))
			return -EINVAL;

		count = sysfs_emit(buf, "%s\n", enabled_status_text[i]);
		break;
	case FEATURE_LIGHT_ENABLE:
		i = drvdata->rgb_en;
		if (i >= ARRAY_SIZE(enabled_status_text))
			return -EINVAL;

		count = sysfs_emit(buf, "%s\n", enabled_status_text[i]);
		break;
	case FEATURE_TOUCHPAD_ENABLE:
		i = drvdata->tp_en;
		if (i >= ARRAY_SIZE(enabled_status_text))
			return -EINVAL;

		count = sysfs_emit(buf, "%s\n", enabled_status_text[i]);
		break;
	case FEATURE_AUTO_SLEEP_TIME:
		switch (device_type) {
		case LEFT_CONTROLLER:
			i = drvdata->gp_left_auto_sleep_time;
			break;
		case RIGHT_CONTROLLER:
			i = drvdata->gp_right_auto_sleep_time;
			break;
		default:
			return -EINVAL;
		}
		count = sysfs_emit(buf, "%u\n", i);
		break;
	case FEATURE_FPS_SWITCH_STATUS:
		i = drvdata->fps_mode;
		if (i >= ARRAY_SIZE(fps_switch_text))
			return -EINVAL;

		count = sysfs_emit(buf, "%s\n", fps_switch_text[i]);
		break;
	case FEATURE_GAMEPAD_MODE:
		i = drvdata->gp_mode;
		if (i >= ARRAY_SIZE(gamepad_mode_text))
			return -EINVAL;

		count = sysfs_emit(buf, "%s\n", gamepad_mode_text[i]);
		break;
	default:
		return -EINVAL;
	}

	return count;
}

static ssize_t feature_status_options(struct device *dev,
				      struct device_attribute *attr, char *buf,
				      enum feature_status_index index)
{
	ssize_t count = 0;
	unsigned int i;

	switch (index) {
	case FEATURE_IMU_ENABLE:
	case FEATURE_IMU_BYPASS:
	case FEATURE_LIGHT_ENABLE:
	case FEATURE_TOUCHPAD_ENABLE:
		for (i = 1; i < ARRAY_SIZE(enabled_status_text); i++) {
			count += sysfs_emit_at(buf, count, "%s ",
					       enabled_status_text[i]);
		}
		break;
	case FEATURE_AUTO_SLEEP_TIME:
		return sysfs_emit(buf, "0-255\n");
	case FEATURE_FPS_SWITCH_STATUS:
		for (i = 1; i < ARRAY_SIZE(fps_switch_text); i++) {
			count += sysfs_emit_at(buf, count, "%s ",
					       fps_switch_text[i]);
		}
		break;
	case FEATURE_GAMEPAD_MODE:
		for (i = 1; i < ARRAY_SIZE(gamepad_mode_text); i++) {
			count += sysfs_emit_at(buf, count, "%s ",
					       gamepad_mode_text[i]);
		}
		break;
	default:
		return -EINVAL;
	}

	if (count)
		buf[count - 1] = '\n';

	return count;
}

static ssize_t motor_config_store(struct device *dev,
				  struct device_attribute *attr,
				  const char *buf, size_t count,
				  enum motor_cfg_index index,
				  enum dev_type device_type)
{
	struct go_cfg_drvdata *drvdata = hid_get_drvdata(to_hid_device(dev));
	size_t size = 1;
	u8 val = 0;
	int ret;

	/* Pairs with smp_store_release in the config setup path */
	if (!smp_load_acquire(&drvdata->gp_registered))
		return -ENODEV;

	switch (index) {
	case MOTOR_CFG_ALL:
		return -EINVAL;
	case MOTOR_INTENSITY:
		ret = sysfs_match_string(intensity_text, buf);
		val = ret;
		break;
	case VIBRATION_NOTIFY_ENABLE:
		ret = sysfs_match_string(enabled_status_text, buf);
		val = ret;
		break;
	case RUMBLE_MODE:
		ret = sysfs_match_string(rumble_mode_text, buf);
		val = ret;
		break;
	case TP_VIBRATION_ENABLE:
		ret = sysfs_match_string(enabled_status_text, buf);
		val = ret;
		break;
	case TP_VIBRATION_INTENSITY:
		ret = sysfs_match_string(intensity_text, buf);
		val = ret;
		break;
	}

	if (ret < 0)
		return ret;

	if (!val)
		size = 0;

	ret = mcu_property_out(drvdata->hdev, MCU_CONFIG_DATA, SET_MOTOR_CFG,
			       index, device_type, &val, size,
			       GO_COMMAND_TIMEOUT_MS);
	if (ret < 0)
		return ret;

	return count;
}

static ssize_t motor_config_show(struct device *dev,
				 struct device_attribute *attr, char *buf,
				 enum motor_cfg_index index,
				 enum dev_type device_type)
{
	struct go_cfg_drvdata *drvdata = hid_get_drvdata(to_hid_device(dev));
	ssize_t count = 0;
	int ret;
	u8 i;

	/* Pairs with smp_store_release in the config setup path */
	if (!smp_load_acquire(&drvdata->gp_registered))
		return -ENODEV;

	ret = mcu_property_out(drvdata->hdev, MCU_CONFIG_DATA, GET_MOTOR_CFG,
			       index, device_type, NULL, 0,
			       GO_COMMAND_TIMEOUT_MS);
	if (ret)
		return ret;

	switch (index) {
	case MOTOR_CFG_ALL:
		return -EINVAL;
	case MOTOR_INTENSITY:
		i = drvdata->gp_rumble_intensity;
		if (i >= ARRAY_SIZE(intensity_text))
			return -EINVAL;

		count = sysfs_emit(buf, "%s\n", intensity_text[i]);
		break;
	case VIBRATION_NOTIFY_ENABLE:
		switch (device_type) {
		case LEFT_CONTROLLER:
			i = drvdata->gp_left_notify_en;
			break;
		case RIGHT_CONTROLLER:
			i = drvdata->gp_right_notify_en;
			break;
		default:
			return -EINVAL;
		}
		if (i >= ARRAY_SIZE(enabled_status_text))
			return -EINVAL;

		count = sysfs_emit(buf, "%s\n", enabled_status_text[i]);
		break;
	case RUMBLE_MODE:
		switch (device_type) {
		case LEFT_CONTROLLER:
			i = drvdata->gp_left_rumble_mode;
			break;
		case RIGHT_CONTROLLER:
			i = drvdata->gp_right_rumble_mode;
			break;
		default:
			return -EINVAL;
		}
		if (i >= ARRAY_SIZE(rumble_mode_text))
			return -EINVAL;

		count = sysfs_emit(buf, "%s\n", rumble_mode_text[i]);
		break;
	case TP_VIBRATION_ENABLE:
		i = drvdata->tp_vibration_en;
		if (i >= ARRAY_SIZE(enabled_status_text))
			return -EINVAL;

		count = sysfs_emit(buf, "%s\n", enabled_status_text[i]);
		break;
	case TP_VIBRATION_INTENSITY:
		i = drvdata->tp_vibration_intensity;
		if (i >= ARRAY_SIZE(intensity_text))
			return -EINVAL;

		count = sysfs_emit(buf, "%s\n", intensity_text[i]);
		break;
	}

	return count;
}

static ssize_t motor_config_options(struct device *dev,
				    struct device_attribute *attr, char *buf,
				    enum motor_cfg_index index)
{
	ssize_t count = 0;
	unsigned int i;

	switch (index) {
	case MOTOR_CFG_ALL:
		break;
	case RUMBLE_MODE:
		for (i = 1; i < ARRAY_SIZE(rumble_mode_text); i++) {
			count += sysfs_emit_at(buf, count, "%s ",
					       rumble_mode_text[i]);
		}
		break;
	case MOTOR_INTENSITY:
	case TP_VIBRATION_INTENSITY:
		for (i = 1; i < ARRAY_SIZE(intensity_text); i++) {
			count += sysfs_emit_at(buf, count, "%s ",
					       intensity_text[i]);
		}
		break;
	case VIBRATION_NOTIFY_ENABLE:
	case TP_VIBRATION_ENABLE:
		for (i = 1; i < ARRAY_SIZE(enabled_status_text); i++) {
			count += sysfs_emit_at(buf, count, "%s ",
					       enabled_status_text[i]);
		}
		break;
	}

	if (count)
		buf[count - 1] = '\n';

	return count;
}

static ssize_t hid_go_gen1_fps_mode_dpi_store(struct go_cfg_drvdata *drvdata,
					      const char *buf, size_t count)
{
	u8 content[5] = { RIGHT_CONTROLLER };
	u8 dev_byte = RIGHT_CONTROLLER;
	bool valid = false;
	u32 value;
	int i, ret;

	ret = kstrtou32(buf, 10, &value);
	if (ret)
		return ret;

	ret = mcu_property_out(drvdata->hdev, 0, GEN1_DPI, GEN1_DPI_GET_LIST,
			       UNSPECIFIED, &dev_byte, sizeof(dev_byte),
			       GO_GEN1_TIMEOUT_MS);
	if (ret < 0)
		return ret;

	for (i = 0; i < ARRAY_SIZE(drvdata->gen1_dpi_values); i++) {
		if (drvdata->gen1_dpi_values[i] == value) {
			valid = true;
			break;
		}
	}

	if (!valid)
		return -EINVAL;

	put_unaligned_be32(value, &content[1]);

	ret = mcu_property_out(drvdata->hdev, 0, GEN1_DPI, GEN1_DPI_SET,
			       UNSPECIFIED, content, sizeof(content),
			       GO_GEN1_TIMEOUT_MS);
	if (ret < 0)
		return ret;

	drvdata->gen1_dpi_current = value;

	return count;
}

static ssize_t hid_go_gen1_fps_mode_dpi_show(struct go_cfg_drvdata *drvdata,
					     char *buf)
{
	u8 dev_byte = RIGHT_CONTROLLER;
	int ret;

	ret = mcu_property_out(drvdata->hdev, 0, GEN1_DPI,
			       GEN1_DPI_GET_CURRENT, UNSPECIFIED,
			       &dev_byte, sizeof(dev_byte),
			       GO_GEN1_TIMEOUT_MS);
	if (ret < 0)
		return ret;

	return sysfs_emit(buf, "%u\n", drvdata->gen1_dpi_current);
}

static ssize_t hid_go_gen1_fps_mode_dpi_index_show(struct go_cfg_drvdata *drvdata,
						   char *buf)
{
	u8 dev_byte = RIGHT_CONTROLLER;
	ssize_t count = 0;
	int i, ret;

	ret = mcu_property_out(drvdata->hdev, 0, GEN1_DPI, GEN1_DPI_GET_LIST,
			       UNSPECIFIED, &dev_byte, sizeof(dev_byte),
			       GO_GEN1_TIMEOUT_MS);
	if (ret < 0)
		return ret;

	for (i = 0; i < ARRAY_SIZE(drvdata->gen1_dpi_values); i++)
		count += sysfs_emit_at(buf, count, "%u ",
				       drvdata->gen1_dpi_values[i]);

	if (count)
		buf[count - 1] = '\n';

	return count;
}

static ssize_t fps_mode_dpi_store(struct device *dev,
				  struct device_attribute *attr,
				  const char *buf, size_t count)

{
	struct go_cfg_drvdata *drvdata = hid_get_drvdata(to_hid_device(dev));
	size_t size = 4;
	u32 value;
	u8 val[4];
	int ret;

	/* Pairs with smp_store_release in the config setup path */
	if (!smp_load_acquire(&drvdata->gp_registered))
		return -ENODEV;

	switch (drvdata->gen) {
	case GO_GEN1:
		return hid_go_gen1_fps_mode_dpi_store(drvdata, buf, count);
	case GO_GEN2:
		break;
	default:
		return -EINVAL;
	}

	ret = kstrtou32(buf, 10, &value);
	if (ret)
		return ret;

	if (value != 500 && value != 800 && value != 1200 && value != 1800)
		return -EINVAL;

	put_unaligned_le32(value, val);

	ret = mcu_property_out(drvdata->hdev, MCU_CONFIG_DATA, SET_DPI_CFG,
			       FPS_MODE_DPI, UNSPECIFIED, val, size,
			       GO_COMMAND_TIMEOUT_MS);
	if (ret < 0)
		return ret;

	return count;
}

static ssize_t fps_mode_dpi_show(struct device *dev,
				 struct device_attribute *attr, char *buf)
{
	struct go_cfg_drvdata *drvdata = hid_get_drvdata(to_hid_device(dev));
	int ret;

	/* Pairs with smp_store_release in the config setup path */
	if (!smp_load_acquire(&drvdata->gp_registered))
		return -ENODEV;

	switch (drvdata->gen) {
	case GO_GEN1:
		return hid_go_gen1_fps_mode_dpi_show(drvdata, buf);
	case GO_GEN2:
		break;
	default:
		return -EINVAL;
	}

	ret = mcu_property_out(drvdata->hdev, MCU_CONFIG_DATA, GET_DPI_CFG,
			       FPS_MODE_DPI, UNSPECIFIED, NULL, 0,
			       GO_COMMAND_TIMEOUT_MS);
	if (ret < 0)
		return ret;

	return sysfs_emit(buf, "%u\n", drvdata->mouse_dpi);
}

static ssize_t fps_mode_dpi_index_show(struct device *dev,
				       struct device_attribute *attr, char *buf)
{
	struct go_cfg_drvdata *drvdata = hid_get_drvdata(to_hid_device(dev));

	/* Pairs with smp_store_release in the config setup path */
	if (!smp_load_acquire(&drvdata->gp_registered))
		return -ENODEV;

	switch (drvdata->gen) {
	case GO_GEN1:
		return hid_go_gen1_fps_mode_dpi_index_show(drvdata, buf);
	case GO_GEN2:
		return sysfs_emit(buf, "500 800 1200 1800\n");
	default:
		return -EINVAL;
	}
}

static u8 *go_cal_status_ptr(struct go_cfg_drvdata *drvdata, enum dev_type device_type,
			     enum cal_device_type cal_type)
{
	switch (device_type) {
	case LEFT_CONTROLLER:
		switch (cal_type) {
		case CALDEV_GYROSCOPE:
			return &drvdata->gp_left_gyro_cal_status;
		case CALDEV_JOYSTICK:
			return &drvdata->gp_left_joy_cal_status;
		case CALDEV_TRIGGER:
			return &drvdata->gp_left_trigg_cal_status;
		default:
			return NULL;
		}
	case RIGHT_CONTROLLER:
		switch (cal_type) {
		case CALDEV_GYROSCOPE:
			return &drvdata->gp_right_gyro_cal_status;
		case CALDEV_JOYSTICK:
			return &drvdata->gp_right_joy_cal_status;
		case CALDEV_TRIGGER:
			return &drvdata->gp_right_trigg_cal_status;
		default:
			return NULL;
		}
	default:
		return NULL;
	}
}

static const char *const gen1_cal_mode_text[] = {
	"stick_and_trigger",
	"stick",
	"trigger",
};

static const char *const gen1_cal_state_text[] = {
	[0] = "idle",
	[1] = "calibrating",
};

static ssize_t device_status_show(struct device *dev,
				  struct device_attribute *attr, char *buf,
				  enum device_status_index index,
				  enum dev_type device_type,
				  enum cal_device_type cal_type)
{
	struct go_cfg_drvdata *drvdata = hid_get_drvdata(to_hid_device(dev));
	u8 i;

	/* Pairs with smp_store_release in the config setup path */
	if (!smp_load_acquire(&drvdata->gp_registered))
		return -ENODEV;

	switch (index) {
	case GET_CAL_STATUS:
		switch (device_type) {
		case LEFT_CONTROLLER:
			switch (cal_type) {
			case CALDEV_GYROSCOPE:
				i = drvdata->gp_left_gyro_cal_status;
				break;
			case CALDEV_JOYSTICK:
				i = drvdata->gp_left_joy_cal_status;
				break;
			case CALDEV_TRIGGER:
				i = drvdata->gp_left_trigg_cal_status;
				break;
			default:
				return -EINVAL;
			}
			break;
		case RIGHT_CONTROLLER:
			switch (cal_type) {
			case CALDEV_GYROSCOPE:
				i = drvdata->gp_right_gyro_cal_status;
				break;
			case CALDEV_JOYSTICK:
				i = drvdata->gp_right_joy_cal_status;
				break;
			case CALDEV_TRIGGER:
				i = drvdata->gp_right_trigg_cal_status;
				break;
			default:
				return -EINVAL;
			}
			break;
		default:
			return -EINVAL;
		}
		break;
	default:
		return -EINVAL;
	}

	if (i >= ARRAY_SIZE(cal_status_text))
		return -EINVAL;

	return sysfs_emit(buf, "%s\n", cal_status_text[i]);
}

enum gen1_cal_index {
	GEN1_CAL_STICK,
};

static ssize_t gen1_cal_show(struct device *dev,
			     struct device_attribute *attr, char *buf,
			     enum gen1_cal_index index,
			     enum dev_type device_type)
{
	struct go_cfg_drvdata *drvdata = hid_get_drvdata(to_hid_device(dev));
	u8 state = drvdata->gen1_cal_state[device_type];

	if (state >= ARRAY_SIZE(gen1_cal_state_text))
		return -EINVAL;

	return sysfs_emit(buf, "%s\n", gen1_cal_state_text[state]);
}

static ssize_t gen1_cal_store(struct device *dev,
			      struct device_attribute *attr, const char *buf,
			      size_t count, enum gen1_cal_index index,
			      enum dev_type device_type)
{
	struct go_cfg_drvdata *drvdata = hid_get_drvdata(to_hid_device(dev));
	u8 content[1] = { device_type };
	const char *group;
	u8 command, sub_cmd;
	int ret;

	/* Pairs with smp_store_release in the config setup path */
	if (!smp_load_acquire(&drvdata->gp_registered))
		return -ENODEV;

	ret = sysfs_match_string(gen1_cal_mode_text, buf);
	if (ret >= 0) {
		command = GEN1_CAL;
		sub_cmd = ret + 1;
	} else if (sysfs_streq(buf, "stop")) {
		command = GEN1_CAL_STOP;
		sub_cmd = drvdata->gen1_cal_mode ? drvdata->gen1_cal_mode : 1;
	} else {
		return -EINVAL;
	}

	/* The MCU echoes the accepted sub-command when calibration starts */
	ret = mcu_property_out(drvdata->hdev, 0, command, sub_cmd,
			       device_type, content, sizeof(content),
			       GO_GEN1_TIMEOUT_MS);
	if (ret)
		return ret;

	if (command == GEN1_CAL) {
		drvdata->gen1_cal_mode = sub_cmd;
		drvdata->gen1_cal_state[device_type] = 1;
	} else {
		drvdata->gen1_cal_state[device_type] = 0;
	}

	group = device_type == LEFT_CONTROLLER ? "left_handle" :
						 "right_handle";
	sysfs_notify(&drvdata->hdev->dev.kobj, group,
		     "calibrate_stick_status");

	return count;
}

static ssize_t calibrate_config_store(struct device *dev,
				      struct device_attribute *attr,
				      const char *buf, u8 cmd, u8 sub_cmd,
				      size_t count, enum dev_type device_type,
				      u8 cal_type)
{
	struct go_cfg_drvdata *drvdata = hid_get_drvdata(to_hid_device(dev));
	u8 val;
	int ret;

	/* Pairs with smp_store_release in the config setup path */
	if (!smp_load_acquire(&drvdata->gp_registered))
		return -ENODEV;

	ret = sysfs_match_string(cal_enabled_text, buf);
	if (ret < 0)
		return ret;
	if (ret == CAL_UNKNOWN)
		return -EINVAL;

	val = ret;
	if (val == CAL_START) {
		u8 *cal_status = go_cal_status_ptr(drvdata, device_type, cal_type);

		if (!cal_status)
			return -EINVAL;

		*cal_status = CAL_STAT_UNKNOWN;
	}

	ret = mcu_property_out(drvdata->hdev, MCU_CONFIG_DATA, cmd, sub_cmd,
			       device_type, &val, sizeof(val),
			       GO_COMMAND_TIMEOUT_MS);
	if (ret < 0)
		return ret;

	return count;
}

static ssize_t calibrate_config_options(struct device *dev,
					struct device_attribute *attr,
					char *buf)
{
	ssize_t count = 0;
	unsigned int i;

	for (i = 1; i < ARRAY_SIZE(cal_enabled_text); i++)
		count += sysfs_emit_at(buf, count, "%s ", cal_enabled_text[i]);

	buf[count - 1] = '\n';

	return count;
}

static ssize_t os_mode_store(struct device *dev, struct device_attribute *attr,
			     const char *buf, size_t count)
{
	struct go_cfg_drvdata *drvdata = hid_get_drvdata(to_hid_device(dev));
	size_t size = 1;
	int ret;
	u8 val;

	/* Pairs with smp_store_release in the config setup path */
	if (!smp_load_acquire(&drvdata->gp_registered))
		return -ENODEV;

	ret = sysfs_match_string(os_mode_text, buf);
	if (ret <= 0)
		return ret;

	val = ret;
	ret = mcu_property_out(drvdata->hdev, OS_MODE_DATA, FEATURE_OS_MODE,
			       SET_OS_MODE, USB_MCU, &val, size,
			       GO_COMMAND_TIMEOUT_MS);
	if (ret < 0)
		return ret;

	drvdata->os_mode = val;

	return count;
}

static ssize_t os_mode_show(struct device *dev, struct device_attribute *attr,
			    char *buf)
{
	struct go_cfg_drvdata *drvdata = hid_get_drvdata(to_hid_device(dev));
	ssize_t count = 0;
	int ret;
	u8 i;

	/* Pairs with smp_store_release in the config setup path */
	if (!smp_load_acquire(&drvdata->gp_registered))
		return -ENODEV;

	ret = mcu_property_out(drvdata->hdev, OS_MODE_DATA, FEATURE_OS_MODE,
			       GET_OS_MODE, USB_MCU, NULL, 0,
			       GO_COMMAND_TIMEOUT_MS);
	if (ret)
		return ret;

	i = drvdata->os_mode;
	if (i >= ARRAY_SIZE(os_mode_text))
		return -EINVAL;

	count = sysfs_emit(buf, "%s\n", os_mode_text[i]);

	return count;
}

static ssize_t os_mode_index_show(struct device *dev,
				  struct device_attribute *attr, char *buf)
{
	ssize_t count = 0;
	unsigned int i;

	for (i = 1; i < ARRAY_SIZE(os_mode_text); i++)
		count += sysfs_emit_at(buf, count, "%s ", os_mode_text[i]);

	if (count)
		buf[count - 1] = '\n';

	return count;
}

static ssize_t rgb_quick_enable_show(struct device *dev,
				     struct device_attribute *attr, char *buf)
{
	struct led_classdev *led_cdev = dev_get_drvdata(dev);
	struct led_classdev_mc *mc_cdev = lcdev_to_mccdev(led_cdev);
	struct go_cfg_drvdata *drvdata = container_of(mc_cdev,
						     struct go_cfg_drvdata,
						     led_mc);
	u8 dev_byte = UNSPECIFIED;
	int ret;

	/* Pairs with smp_store_release in the config setup path */
	if (!smp_load_acquire(&drvdata->rgb_registered))
		return -ENODEV;

	ret = mcu_property_out(drvdata->hdev, 0, GEN1_RGB_ENABLE,
			       GEN1_RGB_QUICK_EN_GET, UNSPECIFIED,
			       &dev_byte, sizeof(dev_byte),
			       GO_GEN1_TIMEOUT_MS);
	if (ret)
		return ret;

	if (drvdata->gen1_rgb_quick_en > 1)
		return -EINVAL;

	return sysfs_emit(buf, "%s\n",
			  enabled_status_text[drvdata->gen1_rgb_quick_en]);
}

static ssize_t rgb_quick_enable_store(struct device *dev,
				      struct device_attribute *attr,
				      const char *buf, size_t count)
{
	struct led_classdev *led_cdev = dev_get_drvdata(dev);
	struct led_classdev_mc *mc_cdev = lcdev_to_mccdev(led_cdev);
	struct go_cfg_drvdata *drvdata = container_of(mc_cdev,
						     struct go_cfg_drvdata,
						     led_mc);
	u8 content[2] = { UNSPECIFIED };
	int ret;

	/* Pairs with smp_store_release in the config setup path */
	if (!smp_load_acquire(&drvdata->rgb_registered))
		return -ENODEV;

	ret = sysfs_match_string(enabled_status_text, buf);
	if (ret < 0)
		return ret;

	content[1] = ret;

	ret = mcu_property_out(drvdata->hdev, 0, GEN1_RGB_ENABLE,
			       GEN1_RGB_QUICK_EN_SET, UNSPECIFIED, content,
			       sizeof(content), GO_GEN1_TIMEOUT_MS);
	if (ret)
		return ret;

	drvdata->gen1_rgb_quick_en = content[1];

	return count;
}

static ssize_t rgb_quick_enable_index_show(struct device *dev,
					   struct device_attribute *attr,
					   char *buf)
{
	ssize_t count = 0;
	unsigned int i;

	for (i = 1; i < ARRAY_SIZE(enabled_status_text); i++)
		count += sysfs_emit_at(buf, count, "%s ",
				       enabled_status_text[i]);

	if (count)
		buf[count - 1] = '\n';

	return count;
}

static ssize_t rgb_quick_effect_show(struct device *dev,
				     struct device_attribute *attr, char *buf)
{
	struct led_classdev *led_cdev = dev_get_drvdata(dev);
	struct led_classdev_mc *mc_cdev = lcdev_to_mccdev(led_cdev);
	struct go_cfg_drvdata *drvdata = container_of(mc_cdev,
						     struct go_cfg_drvdata,
						     led_mc);
	u8 dev_byte = UNSPECIFIED;
	int ret;

	/* Pairs with smp_store_release in the config setup path */
	if (!smp_load_acquire(&drvdata->rgb_registered))
		return -ENODEV;

	ret = mcu_property_out(drvdata->hdev, 0, GEN1_RGB_QUICK,
			       GEN1_RGB_QUICK_GET, UNSPECIFIED,
			       &dev_byte, sizeof(dev_byte),
			       GO_GEN1_TIMEOUT_MS);
	if (ret)
		return ret;

	return sysfs_emit(buf, "profile %u\n", drvdata->gen1_rgb_quick_id);
}

static ssize_t rgb_quick_effect_store(struct device *dev,
				      struct device_attribute *attr,
				      const char *buf, size_t count)
{
	struct led_classdev *led_cdev = dev_get_drvdata(dev);
	struct led_classdev_mc *mc_cdev = lcdev_to_mccdev(led_cdev);
	struct go_cfg_drvdata *drvdata = container_of(mc_cdev,
						     struct go_cfg_drvdata,
						     led_mc);
	u8 content[2] = { UNSPECIFIED };
	int ret;

	/* Pairs with smp_store_release in the config setup path */
	if (!smp_load_acquire(&drvdata->rgb_registered))
		return -ENODEV;

	ret = kstrtou8(buf, 10, &content[1]);
	if (ret)
		return ret;

	/* The firmware accepts stored profiles one and two */
	if (content[1] < 1 || content[1] > 2)
		return -EINVAL;

	ret = mcu_property_out(drvdata->hdev, 0, GEN1_RGB_QUICK,
			       GEN1_RGB_QUICK_SET, UNSPECIFIED, content,
			       sizeof(content), GO_GEN1_TIMEOUT_MS);
	if (ret)
		return ret;

	drvdata->gen1_rgb_quick_id = content[1];

	return count;
}

static ssize_t rgb_quick_effect_index_show(struct device *dev,
					   struct device_attribute *attr,
					   char *buf)
{
	return sysfs_emit(buf, "1-2\n");
}

static int rgb_cfg_call(struct hid_device *hdev, enum mcu_command_index cmd,
			enum rgb_config_index index, u8 *val, size_t size)
{
	if (cmd != SET_RGB_CFG && cmd != GET_RGB_CFG)
		return -EINVAL;

	if (index < LIGHT_CFG_ALL || index > USR_LIGHT_PROFILE_3)
		return -EINVAL;

	struct go_cfg_drvdata *drvdata = hid_get_drvdata(hdev);

	if (drvdata->gen == GO_GEN1) {
		u8 dev_byte = UNSPECIFIED;
		u8 content[8] = { UNSPECIFIED };

		if (cmd == GET_RGB_CFG)
			return mcu_property_out(hdev, 0, GEN1_RGB_PROFILE,
						GEN1_RGB_PROFILE_GET,
						UNSPECIFIED, &dev_byte,
						sizeof(dev_byte),
						GO_GEN1_TIMEOUT_MS);

		if (size != 6)
			return -EOPNOTSUPP;

		memcpy(&content[1], val, 6);
		content[7] = index - USR_LIGHT_PROFILE_1 + 1;

		return mcu_property_out(hdev, 0, GEN1_RGB_SET,
					GEN1_RGB_PROFILE_SET, UNSPECIFIED,
					content, sizeof(content),
					GO_GEN1_TIMEOUT_MS);
	}

	return mcu_property_out(hdev, MCU_CONFIG_DATA, cmd, index, UNSPECIFIED,
				val, size,
				GO_COMMAND_TIMEOUT_MS);
}

static int rgb_attr_show(struct go_cfg_drvdata *drvdata)
{
	enum rgb_config_index index;

	index = drvdata->rgb_profile + 3;

	return rgb_cfg_call(drvdata->hdev, GET_RGB_CFG, index, NULL, 0);
}

static ssize_t rgb_effect_store(struct device *dev,
				struct device_attribute *attr, const char *buf,
				size_t count)
{
	struct led_classdev *led_cdev = dev_get_drvdata(dev);
	struct led_classdev_mc *mc_cdev = lcdev_to_mccdev(led_cdev);
	struct go_cfg_drvdata *drvdata = container_of(mc_cdev,
					     struct go_cfg_drvdata, led_mc);
	enum rgb_config_index index;
	u8 effect;
	int ret;

	/* Pairs with smp_store_release in the config setup path */
	if (!smp_load_acquire(&drvdata->rgb_registered))
		return -ENODEV;

	ret = sysfs_match_string(rgb_effect_text, buf);
	if (ret < 0)
		return ret;

	effect = ret;

	/* Gen1 wire effect values are the shared enum indices plus one */
	if (drvdata->gen == GO_GEN1)
		effect++;

	index = drvdata->rgb_profile + 3;
	u8 rgb_profile[6] = { effect,
			      mc_cdev->subled_info[0].intensity,
			      mc_cdev->subled_info[1].intensity,
			      mc_cdev->subled_info[2].intensity,
			      drvdata->led_mc.led_cdev.brightness,
			      drvdata->rgb_speed };

	ret = rgb_cfg_call(drvdata->hdev, SET_RGB_CFG, index, rgb_profile, 6);
	if (ret)
		return ret;

	drvdata->rgb_effect = effect;
	return count;
}

static ssize_t rgb_effect_show(struct device *dev,
			       struct device_attribute *attr, char *buf)
{
	struct led_classdev *led_cdev = dev_get_drvdata(dev);
	struct led_classdev_mc *mc_cdev = lcdev_to_mccdev(led_cdev);
	struct go_cfg_drvdata *drvdata = container_of(mc_cdev,
					     struct go_cfg_drvdata, led_mc);
	int ret;

	/* Pairs with smp_store_release in the config setup path */
	if (!smp_load_acquire(&drvdata->rgb_registered))
		return -ENODEV;

	ret = rgb_attr_show(drvdata);
	if (ret)
		return ret;

	if (drvdata->gen == GO_GEN1) {
		if (drvdata->rgb_effect < 1 ||
		    drvdata->rgb_effect - 1 >= ARRAY_SIZE(rgb_effect_text))
			return -EINVAL;

		return sysfs_emit(buf, "%s\n",
				  rgb_effect_text[drvdata->rgb_effect - 1]);
	}

	if (drvdata->rgb_effect >= ARRAY_SIZE(rgb_effect_text))
		return -EINVAL;

	return sysfs_emit(buf, "%s\n", rgb_effect_text[drvdata->rgb_effect]);
}

static ssize_t rgb_effect_index_show(struct device *dev,
				     struct device_attribute *attr, char *buf)
{
	ssize_t count = 0;
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(rgb_effect_text); i++)
		count += sysfs_emit_at(buf, count, "%s ", rgb_effect_text[i]);

	if (count)
		buf[count - 1] = '\n';

	return count;
}

static ssize_t rgb_speed_store(struct device *dev,
			       struct device_attribute *attr, const char *buf,
			       size_t count)
{
	struct led_classdev *led_cdev = dev_get_drvdata(dev);
	struct led_classdev_mc *mc_cdev = lcdev_to_mccdev(led_cdev);
	struct go_cfg_drvdata *drvdata = container_of(mc_cdev,
					     struct go_cfg_drvdata, led_mc);
	enum rgb_config_index index;
	int val = 0;
	int ret;

	/* Pairs with smp_store_release in the config setup path */
	if (!smp_load_acquire(&drvdata->rgb_registered))
		return -ENODEV;

	ret = kstrtoint(buf, 10, &val);
	if (ret)
		return ret;

	if (val < 0 || val > 100)
		return -EINVAL;

	/* This is a delay setting, invert logic for consistency with other drivers */
	val = 100 - val;

	index = drvdata->rgb_profile + 3;
	u8 rgb_profile[6] = { drvdata->rgb_effect,
			      mc_cdev->subled_info[0].intensity,
			      mc_cdev->subled_info[1].intensity,
			      mc_cdev->subled_info[2].intensity,
			      drvdata->led_mc.led_cdev.brightness,
			      val };

	ret = rgb_cfg_call(drvdata->hdev, SET_RGB_CFG, index, rgb_profile, 6);
	if (ret)
		return ret;

	drvdata->rgb_speed = val;

	return count;
}

static ssize_t rgb_speed_show(struct device *dev, struct device_attribute *attr,
			      char *buf)
{
	struct led_classdev *led_cdev = dev_get_drvdata(dev);
	struct led_classdev_mc *mc_cdev = lcdev_to_mccdev(led_cdev);
	struct go_cfg_drvdata *drvdata = container_of(mc_cdev,
					     struct go_cfg_drvdata, led_mc);
	int ret, val;

	/* Pairs with smp_store_release in the config setup path */
	if (!smp_load_acquire(&drvdata->rgb_registered))
		return -ENODEV;

	ret = rgb_attr_show(drvdata);
	if (ret)
		return ret;

	if (drvdata->rgb_speed > 100)
		return -EINVAL;

	val = drvdata->rgb_speed;

	return sysfs_emit(buf, "%hhu\n", val);
}

static ssize_t rgb_speed_range_show(struct device *dev,
				    struct device_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "0-100\n");
}

static ssize_t rgb_mode_store(struct device *dev, struct device_attribute *attr,
			      const char *buf, size_t count)
{
	struct led_classdev *led_cdev = dev_get_drvdata(dev);
	struct led_classdev_mc *mc_cdev = lcdev_to_mccdev(led_cdev);
	struct go_cfg_drvdata *drvdata = container_of(mc_cdev,
					     struct go_cfg_drvdata, led_mc);
	int ret;
	u8 val;

	/* Pairs with smp_store_release in the config setup path */
	if (!smp_load_acquire(&drvdata->rgb_registered))
		return -ENODEV;

	ret = sysfs_match_string(rgb_mode_text, buf);
	if (ret <= 0)
		return ret;

	val = ret;

	ret = rgb_cfg_call(drvdata->hdev, SET_RGB_CFG, LIGHT_MODE_SEL, &val, 1);
	if (ret)
		return ret;

	drvdata->rgb_mode = val;

	return count;
}

static ssize_t rgb_mode_show(struct device *dev, struct device_attribute *attr,
			     char *buf)
{
	struct led_classdev *led_cdev = dev_get_drvdata(dev);
	struct led_classdev_mc *mc_cdev = lcdev_to_mccdev(led_cdev);
	struct go_cfg_drvdata *drvdata = container_of(mc_cdev,
					     struct go_cfg_drvdata, led_mc);
	int ret;

	/* Pairs with smp_store_release in the config setup path */
	if (!smp_load_acquire(&drvdata->rgb_registered))
		return -ENODEV;

	ret = rgb_cfg_call(drvdata->hdev, GET_RGB_CFG, LIGHT_MODE_SEL, NULL, 0);
	if (ret)
		return ret;

	if (drvdata->rgb_mode >= ARRAY_SIZE(rgb_mode_text))
		return -EINVAL;

	return sysfs_emit(buf, "%s\n", rgb_mode_text[drvdata->rgb_mode]);
}

static ssize_t rgb_mode_index_show(struct device *dev,
				   struct device_attribute *attr, char *buf)
{
	ssize_t count = 0;
	unsigned int i;

	for (i = 1; i < ARRAY_SIZE(rgb_mode_text); i++)
		count += sysfs_emit_at(buf, count, "%s ", rgb_mode_text[i]);

	if (count)
		buf[count - 1] = '\n';

	return count;
}

static ssize_t rgb_profile_store(struct device *dev,
				 struct device_attribute *attr, const char *buf,
				 size_t count)
{
	struct led_classdev *led_cdev = dev_get_drvdata(dev);
	struct led_classdev_mc *mc_cdev = lcdev_to_mccdev(led_cdev);
	struct go_cfg_drvdata *drvdata = container_of(mc_cdev,
					     struct go_cfg_drvdata, led_mc);
	size_t size = 1;
	int ret;
	u8 val;

	/* Pairs with smp_store_release in the config setup path */
	if (!smp_load_acquire(&drvdata->rgb_registered))
		return -ENODEV;

	ret = kstrtou8(buf, 10, &val);
	if (ret < 0)
		return ret;

	if (val < 1 || val > 3)
		return -EINVAL;

	ret = rgb_cfg_call(drvdata->hdev, SET_RGB_CFG, LIGHT_PROFILE_SEL, &val, size);
	if (ret)
		return ret;

	drvdata->rgb_profile = val;

	return count;
}

static ssize_t rgb_profile_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct led_classdev *led_cdev = dev_get_drvdata(dev);
	struct led_classdev_mc *mc_cdev = lcdev_to_mccdev(led_cdev);
	struct go_cfg_drvdata *drvdata = container_of(mc_cdev,
					     struct go_cfg_drvdata, led_mc);
	int ret;

	/* Pairs with smp_store_release in the config setup path */
	if (!smp_load_acquire(&drvdata->rgb_registered))
		return -ENODEV;

	ret = rgb_cfg_call(drvdata->hdev, GET_RGB_CFG, LIGHT_PROFILE_SEL, NULL, 0);
	if (ret)
		return ret;

	if (drvdata->rgb_profile < 1 || drvdata->rgb_profile > 3)
		return -EINVAL;

	return sysfs_emit(buf, "%hhu\n", drvdata->rgb_profile);
}

static ssize_t rgb_profile_range_show(struct device *dev,
				      struct device_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "1-3\n");
}

static int hid_go_brightness_set_blocking(struct led_classdev *led_cdev,
					  enum led_brightness brightness)
{
	struct led_classdev_mc *mc_cdev = lcdev_to_mccdev(led_cdev);
	struct go_cfg_drvdata *drvdata = container_of(mc_cdev,
						      struct go_cfg_drvdata, led_mc);
	enum rgb_config_index index = drvdata->rgb_profile + 3;
	u8 rgb_profile[6] = { drvdata->rgb_effect,
			      mc_cdev->subled_info[0].intensity,
			      mc_cdev->subled_info[1].intensity,
			      mc_cdev->subled_info[2].intensity,
			      brightness,
			      drvdata->rgb_speed };

	return rgb_cfg_call(drvdata->hdev, SET_RGB_CFG, index, rgb_profile, 6);
}

#define GO_DEV_ATTR_RO(_name, _attrname)                        \
	{ .attr = { .name = _attrname, .mode = 0444 },              \
	  __DEVICE_ATTR_SHOW_STORE(_name##_show, NULL) }

#define GO_DEV_ATTR_WO(_name, _attrname)                        \
	{ .attr = { .name = _attrname, .mode = 0200 },              \
	  __DEVICE_ATTR_SHOW_STORE(NULL, _name##_store) }

#define GO_DEV_ATTR_RW(_name, _attrname)                        \
	{ .attr = { .name = _attrname, .mode = 0644 },              \
	  __DEVICE_ATTR_SHOW_STORE(_name##_show, _name##_store) }

/* Read-only attribute backed by an already defined _name##_show() */
#define LEGO_ATTR_RO(_name, _attrname, _support)                 \
	static struct go_cfg_attr _name = {                      \
		.support = BIT(_support),                        \
		.dev_attr = GO_DEV_ATTR_RO(_name, _attrname),    \
	}

/* Read-write attribute backed by existing _name##_show() and _name##_store() */
#define LEGO_ATTR_RW(_name, _attrname, _support)                 \
	static struct go_cfg_attr _name = {                      \
		.support = BIT(_support),                        \
		.dev_attr = GO_DEV_ATTR_RW(_name, _attrname),    \
	}

#define LEGO_DEVICE_ATTR_RW(_name, _attrname, _dtype, _rtype, _group,    \
			    _index, _support)                            \
	static ssize_t _name##_store(struct device *dev,                 \
				     struct device_attribute *attr,      \
				     const char *buf, size_t count)      \
	{                                                                \
		return _group##_store(dev, attr, buf, count, _index,     \
				      _dtype);                           \
	}                                                                \
	static ssize_t _name##_show(struct device *dev,                  \
				    struct device_attribute *attr,       \
				    char *buf)                           \
	{                                                                \
		return _group##_show(dev, attr, buf, _index, _dtype);    \
	}                                                                \
	static ssize_t _name##_##_rtype##_show(                          \
		struct device *dev, struct device_attribute *attr,       \
		char *buf)                                               \
	{                                                                \
		return _group##_options(dev, attr, buf, _index);         \
	}                                                                \
	static struct go_cfg_attr _name = {                              \
		.index = _index,                                         \
		.support = BIT(_support),                                \
		.dev_attr = GO_DEV_ATTR_RW(_name, _attrname),            \
	}

#define LEGO_DEVICE_ATTR_WO(_name, _attrname, _dtype, _group, _index,    \
			    _support)                                    \
	static ssize_t _name##_store(struct device *dev,                 \
				     struct device_attribute *attr,      \
				     const char *buf, size_t count)      \
	{                                                                \
		return _group##_store(dev, attr, buf, count, _index,     \
				      _dtype);                           \
	}                                                                \
	static struct go_cfg_attr _name = {                              \
		.index = _index,                                         \
		.support = BIT(_support),                                \
		.dev_attr = GO_DEV_ATTR_WO(_name, _attrname),            \
	}

#define LEGO_DEVICE_ATTR_RO(_name, _attrname, _dtype, _group, _index,    \
			    _support)                                    \
	static ssize_t _name##_show(struct device *dev,                  \
				    struct device_attribute *attr,       \
				    char *buf)                           \
	{                                                                \
		return _group##_show(dev, attr, buf, _index, _dtype);    \
	}                                                                \
	static struct go_cfg_attr _name = {                              \
		.index = _index,                                         \
		.support = BIT(_support),                                \
		.dev_attr = GO_DEV_ATTR_RO(_name, _attrname),            \
	}

#define LEGO_CAL_DEVICE_ATTR(_name, _attrname, _index, _scmd, _caltype,    \
			     _dtype, _rtype, _support)                     \
	static ssize_t _name##_store(struct device *dev,                   \
				     struct device_attribute *attr,        \
				     const char *buf, size_t count)        \
	{                                                                  \
		return calibrate_config_store(dev, attr, buf, _index,      \
					      _scmd, count, _dtype,        \
					      _caltype);                   \
	}                                                                  \
	static ssize_t _name##_##_rtype##_show(                            \
		struct device *dev, struct device_attribute *attr,         \
		char *buf)                                                 \
	{                                                                  \
		return calibrate_config_options(dev, attr, buf);           \
	}                                                                  \
	static struct go_cfg_attr _name = {                                \
		.index = _index,                                           \
		.support = BIT(_support),                                  \
		.dev_attr = GO_DEV_ATTR_WO(_name, _attrname),              \
	}

#define LEGO_DEVICE_STATUS_ATTR(_name, _attrname, _scmd, _dtype, _index,    \
				_support)                                   \
	static ssize_t _name##_show(struct device *dev,                     \
				    struct device_attribute *attr,          \
				    char *buf)                              \
	{                                                                   \
		return device_status_show(dev, attr, buf, _index, _scmd,    \
					  _dtype);                          \
	}                                                                   \
	static struct go_cfg_attr _name = {                                 \
		.index = _index,                                            \
		.support = BIT(_support),                                   \
		.dev_attr = GO_DEV_ATTR_RO(_name, _attrname),               \
	}

/* Gamepad - MCU */
LEGO_DEVICE_ATTR_RO(version_product_mcu, "product_version", USB_MCU, version, PRODUCT_VERSION,
		   SUP_VERSION);

LEGO_DEVICE_ATTR_RO(version_protocol_mcu, "protocol_version", USB_MCU, version, PROTOCOL_VERSION,
		   SUP_VERSION);

LEGO_DEVICE_ATTR_RO(version_firmware_mcu, "firmware_version", USB_MCU, version, FIRMWARE_VERSION,
		   SUP_VERSION);

LEGO_DEVICE_ATTR_RO(version_hardware_mcu, "hardware_version", USB_MCU, version, HARDWARE_VERSION,
		   SUP_HARDWARE);

LEGO_DEVICE_ATTR_RO(version_gen_mcu, "hardware_generation", USB_MCU, version, HARDWARE_GENERATION,
		   SUP_HARDWARE);

LEGO_DEVICE_ATTR_RO(fps_switch_status, "fps_switch_status", UNSPECIFIED, feature_status,
		    FEATURE_FPS_SWITCH_STATUS, SUP_FPS_SWITCH);

LEGO_DEVICE_ATTR_RW(gamepad_mode, "mode", UNSPECIFIED, index, feature_status, FEATURE_GAMEPAD_MODE,
		   SUP_GAMEPAD_MODE);
LEGO_ATTR_RO(gamepad_mode_index, "mode_index", SUP_GAMEPAD_MODE);

LEGO_DEVICE_ATTR_WO(reset_mcu, "reset_mcu", USB_MCU, feature_status, FEATURE_RESET_GAMEPAD,
		   SUP_RESET);

LEGO_DEVICE_ATTR_RW(gamepad_rumble_intensity, "rumble_intensity", UNSPECIFIED, index, motor_config,
		    MOTOR_INTENSITY, SUP_RUMBLE);
LEGO_ATTR_RO(gamepad_rumble_intensity_index, "rumble_intensity_index",
		   SUP_RUMBLE);

LEGO_ATTR_RW(fps_mode_dpi, "fps_mode_dpi", SUP_FPS_MODE_DPI);
LEGO_ATTR_RO(fps_mode_dpi_index, "fps_mode_dpi_index", SUP_FPS_MODE_DPI);

LEGO_ATTR_RW(os_mode, "os_mode", SUP_OS_MODE);
LEGO_ATTR_RO(os_mode_index, "os_mode_index", SUP_OS_MODE);

static struct attribute *mcu_attrs[] = {
	&fps_mode_dpi.dev_attr.attr,
	&fps_mode_dpi_index.dev_attr.attr,
	&fps_switch_status.dev_attr.attr,
	&gamepad_mode.dev_attr.attr,
	&gamepad_mode_index.dev_attr.attr,
	&gamepad_rumble_intensity.dev_attr.attr,
	&gamepad_rumble_intensity_index.dev_attr.attr,
	&os_mode.dev_attr.attr,
	&os_mode_index.dev_attr.attr,
	&reset_mcu.dev_attr.attr,
	&version_firmware_mcu.dev_attr.attr,
	&version_gen_mcu.dev_attr.attr,
	&version_hardware_mcu.dev_attr.attr,
	&version_product_mcu.dev_attr.attr,
	&version_protocol_mcu.dev_attr.attr,
	NULL,
};

static umode_t hid_go_attr_is_visible(struct kobject *kobj,
				      struct attribute *attr, int idx)
{
	struct device *dev = container_of(kobj, struct device, kobj);
	struct go_cfg_drvdata *drvdata = hid_get_drvdata(to_hid_device(dev));
	const struct device_attribute *dattr;
	const struct go_cfg_attr *gattr;

	dattr = container_of_const(attr, struct device_attribute, attr);
	gattr = container_of_const(dattr, struct go_cfg_attr, dev_attr);

	if (gattr->support & drvdata->support)
		return attr->mode;

	return 0;
}

/* The lighting attributes hang off the led class device, not the hid device */
static umode_t hid_go_rgb_attr_is_visible(struct kobject *kobj,
					  struct attribute *attr, int idx)
{
	struct device *dev = container_of(kobj, struct device, kobj);
	struct led_classdev *led_cdev = dev_get_drvdata(dev);
	struct led_classdev_mc *mc_cdev = lcdev_to_mccdev(led_cdev);
	struct go_cfg_drvdata *drvdata = container_of(mc_cdev,
						     struct go_cfg_drvdata,
						     led_mc);
	const struct device_attribute *dattr;
	const struct go_cfg_attr *gattr;

	dattr = container_of_const(attr, struct device_attribute, attr);
	gattr = container_of_const(dattr, struct go_cfg_attr, dev_attr);

	if (gattr->support & drvdata->support)
		return attr->mode;

	return 0;
}

static const struct attribute_group mcu_attr_group = {
	.attrs = mcu_attrs,
	.is_visible = hid_go_attr_is_visible,
};

/* Gamepad - TX Dongle */
LEGO_DEVICE_ATTR_RO(version_product_tx_dongle, "product_version", TX_DONGLE, version,
		    PRODUCT_VERSION, SUP_VERSION);

LEGO_DEVICE_ATTR_RO(version_protocol_tx_dongle, "protocol_version", TX_DONGLE, version,
		    PROTOCOL_VERSION, SUP_VERSION);

LEGO_DEVICE_ATTR_RO(version_firmware_tx_dongle, "firmware_version", TX_DONGLE, version,
		    FIRMWARE_VERSION, SUP_VERSION);

LEGO_DEVICE_ATTR_RO(version_hardware_tx_dongle, "hardware_version", TX_DONGLE, version,
		    HARDWARE_VERSION, SUP_HARDWARE);

LEGO_DEVICE_ATTR_RO(version_gen_tx_dongle, "hardware_generation", TX_DONGLE, version,
		    HARDWARE_GENERATION, SUP_HARDWARE);

LEGO_DEVICE_ATTR_RO(reset_tx_dongle, "reset", TX_DONGLE, feature_status, FEATURE_RESET_GAMEPAD,
		   SUP_RESET);

static struct attribute *tx_dongle_attrs[] = {
	&reset_tx_dongle.dev_attr.attr,
	&version_hardware_tx_dongle.dev_attr.attr,
	&version_firmware_tx_dongle.dev_attr.attr,
	&version_gen_tx_dongle.dev_attr.attr,
	&version_product_tx_dongle.dev_attr.attr,
	&version_protocol_tx_dongle.dev_attr.attr,
	NULL,
};

static const struct attribute_group tx_dongle_attr_group = {
	.name = "tx_dongle",
	.attrs = tx_dongle_attrs,
	.is_visible = hid_go_attr_is_visible,
};

/* Gamepad - Left */
LEGO_DEVICE_ATTR_RO(version_product_left, "product_version", LEFT_CONTROLLER, version,
		    PRODUCT_VERSION, SUP_VERSION);

LEGO_DEVICE_ATTR_RO(version_protocol_left, "protocol_version", LEFT_CONTROLLER, version,
		    PROTOCOL_VERSION, SUP_VERSION);

LEGO_DEVICE_ATTR_RO(version_firmware_left, "firmware_version", LEFT_CONTROLLER, version,
		    FIRMWARE_VERSION, SUP_VERSION);

LEGO_DEVICE_ATTR_RO(version_hardware_left, "hardware_version", LEFT_CONTROLLER, version,
		    HARDWARE_VERSION, SUP_HARDWARE);

LEGO_DEVICE_ATTR_RO(version_gen_left, "hardware_generation", LEFT_CONTROLLER, version,
		    HARDWARE_GENERATION, SUP_HARDWARE);

LEGO_DEVICE_ATTR_RW(auto_sleep_time_left, "auto_sleep_time", LEFT_CONTROLLER, range, feature_status,
		    FEATURE_AUTO_SLEEP_TIME, SUP_AUTO_SLEEP);
LEGO_ATTR_RO(auto_sleep_time_left_range, "auto_sleep_time_range",
		   SUP_AUTO_SLEEP);

LEGO_DEVICE_ATTR_RO(battery_left, "battery", LEFT_CONTROLLER, gen1_state,
		    GEN1_STATE_BATTERY, SUP_BATTERY);
LEGO_DEVICE_ATTR_RO(connection_left, "connection", LEFT_CONTROLLER,
		    gen1_state, GEN1_STATE_CONNECTION, SUP_CONNECTION);
LEGO_DEVICE_ATTR_RW(imu_bypass_left, "imu_bypass_enabled", LEFT_CONTROLLER, index, feature_status,
		    FEATURE_IMU_BYPASS, SUP_IMU_BYPASS);
LEGO_ATTR_RO(imu_bypass_left_index, "imu_bypass_enabled_index", SUP_IMU_BYPASS);

LEGO_DEVICE_ATTR_RW(imu_enabled_left, "imu_enabled", LEFT_CONTROLLER, index, feature_status,
		    FEATURE_IMU_ENABLE, SUP_IMU);
LEGO_ATTR_RO(imu_enabled_left_index, "imu_enabled_index", SUP_IMU);

LEGO_DEVICE_ATTR_WO(reset_left, "reset", LEFT_CONTROLLER, feature_status, FEATURE_RESET_GAMEPAD,
		   SUP_RESET);

LEGO_DEVICE_ATTR_RW(rumble_mode_left, "rumble_mode", LEFT_CONTROLLER, index, motor_config,
		    RUMBLE_MODE, SUP_RUMBLE);
LEGO_ATTR_RO(rumble_mode_left_index, "rumble_mode_index", SUP_RUMBLE);

LEGO_DEVICE_ATTR_RW(rumble_notification_left, "rumble_notification", LEFT_CONTROLLER, index,
		    motor_config, VIBRATION_NOTIFY_ENABLE, SUP_RUMBLE);
LEGO_ATTR_RO(rumble_notification_left_index, "rumble_notification_index",
		   SUP_RUMBLE);

LEGO_DEVICE_ATTR_WO(cal_stick_left, "calibrate_stick", LEFT_CONTROLLER,
		    gen1_cal, GEN1_CAL_STICK, SUP_CAL_STICK);
LEGO_DEVICE_ATTR_RO(cal_stick_left_status, "calibrate_stick_status",
		    LEFT_CONTROLLER, gen1_cal, GEN1_CAL_STICK,
		    SUP_CAL_STICK);
LEGO_CAL_DEVICE_ATTR(cal_trigg_left, "calibrate_trigger", SET_TRIGGER_CFG, TRIGGER_CALIBRATE,
		     CALDEV_TRIGGER, LEFT_CONTROLLER, index, SUP_CAL_TRIGGER);
LEGO_ATTR_RO(cal_trigg_left_index, "calibrate_trigger_index", SUP_CAL_TRIGGER);

LEGO_CAL_DEVICE_ATTR(cal_joy_left, "calibrate_joystick", SET_JOYSTICK_CFG, JOYSTICK_CALIBRATE,
		     CALDEV_JOYSTICK, LEFT_CONTROLLER, index, SUP_CAL_JOYSTICK);
LEGO_ATTR_RO(cal_joy_left_index, "calibrate_joystick_index", SUP_CAL_JOYSTICK);

LEGO_CAL_DEVICE_ATTR(cal_gyro_left, "calibrate_gyro", SET_GYRO_CFG, GYRO_CALIBRATE,
		     CALDEV_GYROSCOPE, LEFT_CONTROLLER, index, SUP_CAL_GYRO);
LEGO_ATTR_RO(cal_gyro_left_index, "calibrate_gyro_index", SUP_CAL_GYRO);

LEGO_DEVICE_STATUS_ATTR(cal_trigg_left_status, "calibrate_trigger_status", LEFT_CONTROLLER,
			CALDEV_TRIGGER, GET_CAL_STATUS, SUP_CAL_TRIGGER);

LEGO_DEVICE_STATUS_ATTR(cal_joy_left_status, "calibrate_joystick_status", LEFT_CONTROLLER,
			CALDEV_JOYSTICK, GET_CAL_STATUS, SUP_CAL_JOYSTICK);

LEGO_DEVICE_STATUS_ATTR(cal_gyro_left_status, "calibrate_gyro_status", LEFT_CONTROLLER,
			CALDEV_GYROSCOPE, GET_CAL_STATUS, SUP_CAL_GYRO);

static struct attribute *left_gamepad_attrs[] = {
	&auto_sleep_time_left.dev_attr.attr,
	&auto_sleep_time_left_range.dev_attr.attr,
	&battery_left.dev_attr.attr,
	&cal_gyro_left.dev_attr.attr,
	&cal_gyro_left_index.dev_attr.attr,
	&cal_gyro_left_status.dev_attr.attr,
	&cal_joy_left.dev_attr.attr,
	&cal_joy_left_index.dev_attr.attr,
	&cal_joy_left_status.dev_attr.attr,
	&cal_stick_left.dev_attr.attr,
	&cal_stick_left_status.dev_attr.attr,
	&cal_trigg_left.dev_attr.attr,
	&cal_trigg_left_index.dev_attr.attr,
	&cal_trigg_left_status.dev_attr.attr,
	&connection_left.dev_attr.attr,
	&imu_bypass_left.dev_attr.attr,
	&imu_bypass_left_index.dev_attr.attr,
	&imu_enabled_left.dev_attr.attr,
	&imu_enabled_left_index.dev_attr.attr,
	&reset_left.dev_attr.attr,
	&rumble_mode_left.dev_attr.attr,
	&rumble_mode_left_index.dev_attr.attr,
	&rumble_notification_left.dev_attr.attr,
	&rumble_notification_left_index.dev_attr.attr,
	&version_hardware_left.dev_attr.attr,
	&version_firmware_left.dev_attr.attr,
	&version_gen_left.dev_attr.attr,
	&version_product_left.dev_attr.attr,
	&version_protocol_left.dev_attr.attr,
	NULL,
};

static const struct attribute_group left_gamepad_attr_group = {
	.name = "left_handle",
	.attrs = left_gamepad_attrs,
	.is_visible = hid_go_attr_is_visible,
};

/* Gamepad - Right */
LEGO_DEVICE_ATTR_RO(version_product_right, "product_version", RIGHT_CONTROLLER, version,
		    PRODUCT_VERSION, SUP_VERSION);

LEGO_DEVICE_ATTR_RO(version_protocol_right, "protocol_version", RIGHT_CONTROLLER, version,
		    PROTOCOL_VERSION, SUP_VERSION);

LEGO_DEVICE_ATTR_RO(version_firmware_right, "firmware_version", RIGHT_CONTROLLER, version,
		    FIRMWARE_VERSION, SUP_VERSION);

LEGO_DEVICE_ATTR_RO(version_hardware_right, "hardware_version", RIGHT_CONTROLLER, version,
		    HARDWARE_VERSION, SUP_HARDWARE);

LEGO_DEVICE_ATTR_RO(version_gen_right, "hardware_generation", RIGHT_CONTROLLER, version,
		    HARDWARE_GENERATION, SUP_HARDWARE);

LEGO_DEVICE_ATTR_RW(auto_sleep_time_right, "auto_sleep_time", RIGHT_CONTROLLER, range,
		    feature_status, FEATURE_AUTO_SLEEP_TIME, SUP_AUTO_SLEEP);
LEGO_ATTR_RO(auto_sleep_time_right_range, "auto_sleep_time_range",
		   SUP_AUTO_SLEEP);

LEGO_DEVICE_ATTR_RO(battery_right, "battery", RIGHT_CONTROLLER, gen1_state,
		    GEN1_STATE_BATTERY, SUP_BATTERY);
LEGO_DEVICE_ATTR_RO(connection_right, "connection", RIGHT_CONTROLLER,
		    gen1_state, GEN1_STATE_CONNECTION, SUP_CONNECTION);
LEGO_DEVICE_ATTR_RW(imu_bypass_right, "imu_bypass_enabled", RIGHT_CONTROLLER, index, feature_status,
		    FEATURE_IMU_BYPASS, SUP_IMU_BYPASS);
LEGO_ATTR_RO(imu_bypass_right_index, "imu_bypass_enabled_index",
		   SUP_IMU_BYPASS);

LEGO_DEVICE_ATTR_RW(imu_enabled_right, "imu_enabled", RIGHT_CONTROLLER, index, feature_status,
		    FEATURE_IMU_ENABLE, SUP_IMU);
LEGO_ATTR_RO(imu_enabled_right_index, "imu_enabled_index", SUP_IMU);

LEGO_DEVICE_ATTR_WO(reset_right, "reset", RIGHT_CONTROLLER, feature_status, FEATURE_RESET_GAMEPAD,
		   SUP_RESET);

LEGO_DEVICE_ATTR_RW(rumble_mode_right, "rumble_mode", RIGHT_CONTROLLER, index, motor_config,
		    RUMBLE_MODE, SUP_RUMBLE);
LEGO_ATTR_RO(rumble_mode_right_index, "rumble_mode_index", SUP_RUMBLE);

LEGO_DEVICE_ATTR_RW(rumble_notification_right, "rumble_notification", RIGHT_CONTROLLER, index,
		    motor_config, VIBRATION_NOTIFY_ENABLE, SUP_RUMBLE);
LEGO_ATTR_RO(rumble_notification_right_index, "rumble_notification_index",
		   SUP_RUMBLE);

LEGO_DEVICE_ATTR_WO(cal_stick_right, "calibrate_stick", RIGHT_CONTROLLER,
		    gen1_cal, GEN1_CAL_STICK, SUP_CAL_STICK);
LEGO_DEVICE_ATTR_RO(cal_stick_right_status, "calibrate_stick_status",
		    RIGHT_CONTROLLER, gen1_cal, GEN1_CAL_STICK,
		    SUP_CAL_STICK);
LEGO_CAL_DEVICE_ATTR(cal_trigg_right, "calibrate_trigger", SET_TRIGGER_CFG, TRIGGER_CALIBRATE,
		     CALDEV_TRIGGER, RIGHT_CONTROLLER, index, SUP_CAL_TRIGGER);
LEGO_ATTR_RO(cal_trigg_right_index, "calibrate_trigger_index", SUP_CAL_TRIGGER);

LEGO_CAL_DEVICE_ATTR(cal_joy_right, "calibrate_joystick", SET_JOYSTICK_CFG, JOYSTICK_CALIBRATE,
		     CALDEV_JOYSTICK, RIGHT_CONTROLLER, index, SUP_CAL_JOYSTICK);
LEGO_ATTR_RO(cal_joy_right_index, "calibrate_joystick_index", SUP_CAL_JOYSTICK);

LEGO_CAL_DEVICE_ATTR(cal_gyro_right, "calibrate_gyro", SET_GYRO_CFG, GYRO_CALIBRATE,
		     CALDEV_GYROSCOPE, RIGHT_CONTROLLER, index, SUP_CAL_GYRO);
LEGO_ATTR_RO(cal_gyro_right_index, "calibrate_gyro_index", SUP_CAL_GYRO);

LEGO_DEVICE_STATUS_ATTR(cal_trigg_right_status, "calibrate_trigger_status", RIGHT_CONTROLLER,
			CALDEV_TRIGGER, GET_CAL_STATUS, SUP_CAL_TRIGGER);

LEGO_DEVICE_STATUS_ATTR(cal_joy_right_status, "calibrate_joystick_status", RIGHT_CONTROLLER,
			CALDEV_JOYSTICK, GET_CAL_STATUS, SUP_CAL_JOYSTICK);

LEGO_DEVICE_STATUS_ATTR(cal_gyro_right_status, "calibrate_gyro_status", RIGHT_CONTROLLER,
			CALDEV_GYROSCOPE, GET_CAL_STATUS, SUP_CAL_GYRO);

static struct attribute *right_gamepad_attrs[] = {
	&auto_sleep_time_right.dev_attr.attr,
	&auto_sleep_time_right_range.dev_attr.attr,
	&battery_right.dev_attr.attr,
	&cal_gyro_right.dev_attr.attr,
	&cal_gyro_right_index.dev_attr.attr,
	&cal_gyro_right_status.dev_attr.attr,
	&cal_joy_right.dev_attr.attr,
	&cal_joy_right_index.dev_attr.attr,
	&cal_joy_right_status.dev_attr.attr,
	&cal_stick_right.dev_attr.attr,
	&cal_stick_right_status.dev_attr.attr,
	&cal_trigg_right.dev_attr.attr,
	&cal_trigg_right_index.dev_attr.attr,
	&cal_trigg_right_status.dev_attr.attr,
	&connection_right.dev_attr.attr,
	&imu_bypass_right.dev_attr.attr,
	&imu_bypass_right_index.dev_attr.attr,
	&imu_enabled_right.dev_attr.attr,
	&imu_enabled_right_index.dev_attr.attr,
	&reset_right.dev_attr.attr,
	&rumble_mode_right.dev_attr.attr,
	&rumble_mode_right_index.dev_attr.attr,
	&rumble_notification_right.dev_attr.attr,
	&rumble_notification_right_index.dev_attr.attr,
	&version_hardware_right.dev_attr.attr,
	&version_firmware_right.dev_attr.attr,
	&version_gen_right.dev_attr.attr,
	&version_product_right.dev_attr.attr,
	&version_protocol_right.dev_attr.attr,
	NULL,
};

static const struct attribute_group right_gamepad_attr_group = {
	.name = "right_handle",
	.attrs = right_gamepad_attrs,
	.is_visible = hid_go_attr_is_visible,
};

/* Touchpad */
LEGO_DEVICE_ATTR_RW(touchpad_enabled, "enabled", UNSPECIFIED, index, feature_status,
		    FEATURE_TOUCHPAD_ENABLE, SUP_TOUCHPAD);
LEGO_ATTR_RO(touchpad_enabled_index, "enabled_index", SUP_TOUCHPAD);

LEGO_DEVICE_ATTR_RW(touchpad_vibration_enabled, "vibration_enabled", UNSPECIFIED, index,
		    motor_config, TP_VIBRATION_ENABLE, SUP_RUMBLE);
LEGO_ATTR_RO(touchpad_vibration_enabled_index, "vibration_enabled_index",
		   SUP_RUMBLE);

LEGO_DEVICE_ATTR_RW(touchpad_vibration_intensity, "vibration_intensity", UNSPECIFIED, index,
		    motor_config, TP_VIBRATION_INTENSITY, SUP_RUMBLE);
LEGO_ATTR_RO(touchpad_vibration_intensity_index, "vibration_intensity_index",
		   SUP_RUMBLE);

static struct attribute *touchpad_attrs[] = {
	&touchpad_enabled.dev_attr.attr,
	&touchpad_enabled_index.dev_attr.attr,
	&touchpad_vibration_enabled.dev_attr.attr,
	&touchpad_vibration_enabled_index.dev_attr.attr,
	&touchpad_vibration_intensity.dev_attr.attr,
	&touchpad_vibration_intensity_index.dev_attr.attr,
	NULL,
};

static const struct attribute_group touchpad_attr_group = {
	.name = "touchpad",
	.attrs = touchpad_attrs,
	.is_visible = hid_go_attr_is_visible,
};

static const struct attribute_group *top_level_attr_groups[] = {
	&mcu_attr_group,	  &tx_dongle_attr_group,
	&left_gamepad_attr_group, &right_gamepad_attr_group,
	&touchpad_attr_group,	  NULL,
};

/* RGB */
LEGO_DEVICE_ATTR_RW(rgb_enabled, "enabled", UNSPECIFIED, index, feature_status,
		    FEATURE_LIGHT_ENABLE, SUP_RGB);
LEGO_ATTR_RO(rgb_effect_index, "effect_index", SUP_RGB);
LEGO_ATTR_RO(rgb_enabled_index, "enabled_index", SUP_RGB);
LEGO_ATTR_RO(rgb_mode_index, "mode_index", SUP_RGB_USR);
LEGO_ATTR_RO(rgb_profile_range, "profile_range", SUP_RGB_USR);
LEGO_ATTR_RO(rgb_speed_range, "speed_range", SUP_RGB_USR);
LEGO_ATTR_RW(rgb_effect, "effect", SUP_RGB);
LEGO_ATTR_RW(rgb_mode, "mode", SUP_RGB_USR);
LEGO_ATTR_RW(rgb_profile, "profile", SUP_RGB_USR);
LEGO_ATTR_RW(rgb_speed, "speed", SUP_RGB_USR);
LEGO_ATTR_RW(rgb_quick_enable, "quick_enable", SUP_RGB_QUICK);
LEGO_ATTR_RO(rgb_quick_enable_index, "quick_enable_index", SUP_RGB_QUICK);
LEGO_ATTR_RW(rgb_quick_effect, "quick_effect", SUP_RGB_QUICK);
LEGO_ATTR_RO(rgb_quick_effect_index, "quick_effect_index", SUP_RGB_QUICK);

static struct attribute *go_rgb_attrs[] = {
	&rgb_effect.dev_attr.attr,
	&rgb_effect_index.dev_attr.attr,
	&rgb_enabled.dev_attr.attr,
	&rgb_enabled_index.dev_attr.attr,
	&rgb_mode.dev_attr.attr,
	&rgb_mode_index.dev_attr.attr,
	&rgb_profile.dev_attr.attr,
	&rgb_profile_range.dev_attr.attr,
	&rgb_speed.dev_attr.attr,
	&rgb_speed_range.dev_attr.attr,
	&rgb_quick_enable.dev_attr.attr,
	&rgb_quick_enable_index.dev_attr.attr,
	&rgb_quick_effect.dev_attr.attr,
	&rgb_quick_effect_index.dev_attr.attr,
	NULL,
};

static struct attribute_group rgb_attr_group = {
	.attrs = go_rgb_attrs,
	.is_visible = hid_go_rgb_attr_is_visible,
};

static struct mc_subled go_rgb_subled_info[] = {
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

static void hid_go_cfg_setup(struct work_struct *work)
{
	struct go_cfg_drvdata *drvdata = container_of(work, struct go_cfg_drvdata,
						      go_cfg_setup.work);
	bool gp_registered, rgb_registered;
	enum dev_type dev;
	int ret;

	if (drvdata->gen == GO_GEN1) {
		for (dev = USB_MCU; dev <= RIGHT_CONTROLLER; dev++) {
			ret = mcu_property_out(drvdata->hdev, 0, GEN1_VERSION,
					       GEN1_OP_QUERY, dev, NULL, 0,
					       GO_GEN1_TIMEOUT_MS);
			if (ret)
				dev_warn(&drvdata->hdev->dev,
					 "Failed to retrieve device %u version: %i\n",
					 dev, ret);
		}

		goto try_gp;
	}

	/* MCU Version Attrs */
	ret = mcu_property_out(drvdata->hdev, MCU_CONFIG_DATA, GET_VERSION_DATA,
			       PRODUCT_VERSION, USB_MCU, NULL, 0,
			       GO_COMMAND_TIMEOUT_MS);
	if (ret < 0) {
		dev_err(&drvdata->hdev->dev,
			"Failed to retrieve USB_MCU Product Version: %i\n", ret);
		return;
	}

	ret = mcu_property_out(drvdata->hdev, MCU_CONFIG_DATA, GET_VERSION_DATA,
			       PROTOCOL_VERSION, USB_MCU, NULL, 0,
			       GO_COMMAND_TIMEOUT_MS);
	if (ret < 0) {
		dev_err(&drvdata->hdev->dev,
			"Failed to retrieve USB_MCU Protocol Version: %i\n", ret);
		return;
	}

	ret = mcu_property_out(drvdata->hdev, MCU_CONFIG_DATA, GET_VERSION_DATA,
			       FIRMWARE_VERSION, USB_MCU, NULL, 0,
			       GO_COMMAND_TIMEOUT_MS);
	if (ret < 0) {
		dev_err(&drvdata->hdev->dev,
			"Failed to retrieve USB_MCU Firmware Version: %i\n", ret);
		return;
	}

	ret = mcu_property_out(drvdata->hdev, MCU_CONFIG_DATA, GET_VERSION_DATA,
			       HARDWARE_VERSION, USB_MCU, NULL, 0,
			       GO_COMMAND_TIMEOUT_MS);
	if (ret < 0) {
		dev_err(&drvdata->hdev->dev,
			"Failed to retrieve USB_MCU Hardware Version: %i\n", ret);
		return;
	}

	ret = mcu_property_out(drvdata->hdev, MCU_CONFIG_DATA, GET_VERSION_DATA,
			       HARDWARE_GENERATION, USB_MCU, NULL, 0,
			       GO_COMMAND_TIMEOUT_MS);
	if (ret < 0) {
		dev_err(&drvdata->hdev->dev,
			"Failed to retrieve USB_MCU Hardware Generation: %i\n", ret);
		return;
	}

	/* TX Dongle Version Attrs */
	ret = mcu_property_out(drvdata->hdev, MCU_CONFIG_DATA, GET_VERSION_DATA,
			       PRODUCT_VERSION, TX_DONGLE, NULL, 0,
			       GO_COMMAND_TIMEOUT_MS);
	if (ret < 0) {
		dev_err(&drvdata->hdev->dev,
			"Failed to retrieve TX_DONGLE Product Version: %i\n", ret);
		return;
	}

	ret = mcu_property_out(drvdata->hdev, MCU_CONFIG_DATA, GET_VERSION_DATA,
			       PROTOCOL_VERSION, TX_DONGLE, NULL, 0,
			       GO_COMMAND_TIMEOUT_MS);
	if (ret < 0) {
		dev_err(&drvdata->hdev->dev,
			"Failed to retrieve TX_DONGLE Protocol Version: %i\n", ret);
		return;
	}

	ret = mcu_property_out(drvdata->hdev, MCU_CONFIG_DATA, GET_VERSION_DATA,
			       FIRMWARE_VERSION, TX_DONGLE, NULL, 0,
			       GO_COMMAND_TIMEOUT_MS);
	if (ret < 0) {
		dev_err(&drvdata->hdev->dev,
			"Failed to retrieve TX_DONGLE Firmware Version: %i\n", ret);
		return;
	}

	ret = mcu_property_out(drvdata->hdev, MCU_CONFIG_DATA, GET_VERSION_DATA,
			       HARDWARE_VERSION, TX_DONGLE, NULL, 0,
			       GO_COMMAND_TIMEOUT_MS);
	if (ret < 0) {
		dev_err(&drvdata->hdev->dev,
			"Failed to retrieve TX_DONGLE Hardware Version: %i\n", ret);
		return;
	}

	ret = mcu_property_out(drvdata->hdev, MCU_CONFIG_DATA, GET_VERSION_DATA,
			       HARDWARE_GENERATION, TX_DONGLE, NULL, 0,
			       GO_COMMAND_TIMEOUT_MS);
	if (ret < 0) {
		dev_err(&drvdata->hdev->dev,
			"Failed to retrieve TX_DONGLE Hardware Generation: %i\n", ret);
		return;
	}

	/* Left Handle Version Attrs */
	ret = mcu_property_out(drvdata->hdev, MCU_CONFIG_DATA, GET_VERSION_DATA,
			       PRODUCT_VERSION, LEFT_CONTROLLER, NULL, 0,
			       GO_COMMAND_TIMEOUT_MS);
	if (ret < 0) {
		dev_err(&drvdata->hdev->dev,
			"Failed to retrieve LEFT_CONTROLLER Product Version: %i\n", ret);
		return;
	}

	ret = mcu_property_out(drvdata->hdev, MCU_CONFIG_DATA, GET_VERSION_DATA,
			       PROTOCOL_VERSION, LEFT_CONTROLLER, NULL, 0,
			       GO_COMMAND_TIMEOUT_MS);
	if (ret < 0) {
		dev_err(&drvdata->hdev->dev,
			"Failed to retrieve LEFT_CONTROLLER Protocol Version: %i\n", ret);
		return;
	}

	ret = mcu_property_out(drvdata->hdev, MCU_CONFIG_DATA, GET_VERSION_DATA,
			       FIRMWARE_VERSION, LEFT_CONTROLLER, NULL, 0,
			       GO_COMMAND_TIMEOUT_MS);
	if (ret < 0) {
		dev_err(&drvdata->hdev->dev,
			"Failed to retrieve LEFT_CONTROLLER Firmware Version: %i\n", ret);
		return;
	}

	ret = mcu_property_out(drvdata->hdev, MCU_CONFIG_DATA, GET_VERSION_DATA,
			       HARDWARE_VERSION, LEFT_CONTROLLER, NULL, 0,
			       GO_COMMAND_TIMEOUT_MS);
	if (ret < 0) {
		dev_err(&drvdata->hdev->dev,
			"Failed to retrieve LEFT_CONTROLLER Hardware Version: %i\n", ret);
		return;
	}

	ret = mcu_property_out(drvdata->hdev, MCU_CONFIG_DATA, GET_VERSION_DATA,
			       HARDWARE_GENERATION, LEFT_CONTROLLER, NULL, 0,
			       GO_COMMAND_TIMEOUT_MS);
	if (ret < 0) {
		dev_err(&drvdata->hdev->dev,
			"Failed to retrieve LEFT_CONTROLLER Hardware Generation: %i\n", ret);
		return;
	}

	/* Right Handle Version Attrs */
	ret = mcu_property_out(drvdata->hdev, MCU_CONFIG_DATA, GET_VERSION_DATA,
			       PRODUCT_VERSION, RIGHT_CONTROLLER, NULL, 0,
			       GO_COMMAND_TIMEOUT_MS);
	if (ret < 0) {
		dev_err(&drvdata->hdev->dev,
			"Failed to retrieve RIGHT_CONTROLLER Product Version: %i\n", ret);
		return;
	}

	ret = mcu_property_out(drvdata->hdev, MCU_CONFIG_DATA, GET_VERSION_DATA,
			       PROTOCOL_VERSION, RIGHT_CONTROLLER, NULL, 0,
			       GO_COMMAND_TIMEOUT_MS);
	if (ret < 0) {
		dev_err(&drvdata->hdev->dev,
			"Failed to retrieve RIGHT_CONTROLLER Protocol Version: %i\n", ret);
		return;
	}

	ret = mcu_property_out(drvdata->hdev, MCU_CONFIG_DATA, GET_VERSION_DATA,
			       FIRMWARE_VERSION, RIGHT_CONTROLLER, NULL, 0,
			       GO_COMMAND_TIMEOUT_MS);
	if (ret < 0) {
		dev_err(&drvdata->hdev->dev,
			"Failed to retrieve RIGHT_CONTROLLER Firmware Version: %i\n", ret);
		return;
	}

	ret = mcu_property_out(drvdata->hdev, MCU_CONFIG_DATA, GET_VERSION_DATA,
			       HARDWARE_VERSION, RIGHT_CONTROLLER, NULL, 0,
			       GO_COMMAND_TIMEOUT_MS);
	if (ret < 0) {
		dev_err(&drvdata->hdev->dev,
			"Failed to retrieve RIGHT_CONTROLLER Hardware Version: %i\n", ret);
		return;
	}

	ret = mcu_property_out(drvdata->hdev, MCU_CONFIG_DATA, GET_VERSION_DATA,
			       HARDWARE_GENERATION, RIGHT_CONTROLLER, NULL, 0,
			       GO_COMMAND_TIMEOUT_MS);
	if (ret < 0) {
		dev_err(&drvdata->hdev->dev,
			"Failed to retrieve RIGHT_CONTROLLER Hardware Generation: %i\n", ret);
		return;
	}

try_gp:
	/* Pairs with the smp_store_release later in this function */
	gp_registered = smp_load_acquire(&drvdata->gp_registered);
	if (gp_registered)
		goto try_rgb;

	ret = sysfs_create_groups(&drvdata->hdev->dev.kobj, top_level_attr_groups);
	if (ret) {
		dev_err(&drvdata->hdev->dev,
			"Failed to create gamepad configuration attributes: %i\n", ret);
		drvdata->gp_registered = false;
		goto try_rgb;
	}

	/* Pairs with the smp_load_acquire in attribute show and store paths */
	smp_store_release(&drvdata->gp_registered, true);
	gp_registered = true;

try_rgb:
	/* Pairs with the smp_store_release later in this function */
	rgb_registered = smp_load_acquire(&drvdata->rgb_registered);
	if (rgb_registered)
		goto update_kobjects;

	ret = devm_led_classdev_multicolor_register(&drvdata->hdev->dev,
						    &drvdata->led_mc);
	if (ret) {
		dev_err(&drvdata->hdev->dev, "Failed to create RGB device: %i\n",
			ret);
		goto update_kobjects;
	}

	ret = devm_device_add_group(drvdata->led_mc.led_cdev.dev, &rgb_attr_group);
	if (ret) {
		dev_err(&drvdata->hdev->dev,
			"Failed to create RGB configuration attributes: %i\n", ret);
		goto update_kobjects;
	}

	/* Pairs with the smp_load_acquire in attribute show and store paths */
	smp_store_release(&drvdata->rgb_registered, true);
	rgb_registered = true;

update_kobjects:
	/* Attribute surfaces appeared outside the udev ADD event */
	if (gp_registered)
		kobject_uevent(&drvdata->hdev->dev.kobj, KOBJ_CHANGE);
	if (rgb_registered)
		kobject_uevent(&drvdata->led_mc.led_cdev.dev->kobj, KOBJ_CHANGE);
}

static int hid_go_cfg_probe(struct hid_device *hdev,
			    const struct hid_device_id *_id)
{
	struct go_cfg_drvdata *drvdata = hid_get_drvdata(hdev);

	int ret;

	/* Device is hardwired and name is guaranteed to be unique */
	drvdata->led_mc.led_cdev.name = "go:rgb:joystick_rings";
	drvdata->led_mc.led_cdev.color = LED_COLOR_ID_RGB;
	drvdata->led_mc.led_cdev.brightness = 0x50;
	drvdata->led_mc.led_cdev.max_brightness = 0x64;
	drvdata->led_mc.led_cdev.flags = LED_HW_PLUGGABLE;
	drvdata->led_mc.led_cdev.brightness_set_blocking =
		hid_go_brightness_set_blocking;
	drvdata->led_mc.num_colors = ARRAY_SIZE(go_rgb_subled_info);
	drvdata->led_mc.subled_info = devm_kmemdup(&hdev->dev, go_rgb_subled_info,
						   sizeof(go_rgb_subled_info), GFP_KERNEL);
	if (!drvdata->led_mc.subled_info)
		return -ENOMEM;

	/* Executing calls prior to returning from probe will lock the MCU. Schedule
	 * initial data call after probe has completed and MCU can accept calls.
	 */
	INIT_DELAYED_WORK(&drvdata->go_cfg_setup, &hid_go_cfg_setup);
	ret = schedule_delayed_work(&drvdata->go_cfg_setup, msecs_to_jiffies(2));
	if (!ret) {
		dev_err(&hdev->dev,
			"Failed to schedule startup delayed work\n");
		return -ENODEV;
	}
	return 0;
}

static void hid_go_cfg_remove(struct hid_device *hdev)
{
	struct go_cfg_drvdata *drvdata = hid_get_drvdata(hdev);

	/*
	 * hid_go_cfg_setup is scheduled from hid_go_cfg_probe() with a 2 ms delay
	 * and dereferences drvdata->hdev.  Drain it here before tearing
	 * down so the workqueue cannot run after hid_destroy_device()'s
	 * put_device() has released the underlying hdev and dereference
	 * a stale drvdata->hdev pointer.
	 */
	cancel_delayed_work_sync(&drvdata->go_cfg_setup);

	guard(mutex)(&drvdata->cfg_mutex);

	sysfs_remove_groups(&hdev->dev.kobj, top_level_attr_groups);
	hid_hw_close(hdev);
	hid_hw_stop(hdev);
	hid_set_drvdata(hdev, NULL);
}

static int hid_go_probe(struct hid_device *hdev, const struct hid_device_id *id)
{
	struct go_cfg_drvdata *drvdata;
	int ret;

	if (!hid_is_usb(hdev))
		return -EINVAL;

	hdev->quirks |= HID_QUIRK_INPUT_PER_APP | HID_QUIRK_MULTI_INPUT;

	ret = hid_parse(hdev);
	if (ret) {
		hid_err(hdev, "Parse failed\n");
		return ret;
	}

	if (hid_go_is_config_iface(hdev)) {
		drvdata = devm_kzalloc(&hdev->dev, sizeof(*drvdata), GFP_KERNEL);
		if (!drvdata)
			return -ENOMEM;

		hid_set_drvdata(hdev, drvdata);
		drvdata->gen = (enum go_hw_gen)id->driver_data;
		drvdata->support = drvdata->gen == GO_GEN1 ? SUPPORT_GEN1 :
						       SUPPORT_GEN2;
		drvdata->hdev = hdev;
		mutex_init(&drvdata->cfg_mutex);
		init_completion(&drvdata->cmd.done);
		spin_lock_init(&drvdata->cmd.lock);
		drvdata->cmd.pending = false;
	}

	ret = hid_hw_start(hdev, HID_CONNECT_DEFAULT);
	if (ret) {
		hid_err(hdev, "Failed to start HID device\n");
		return ret;
	}

	ret = hid_hw_open(hdev);
	if (ret) {
		hid_err(hdev, "Failed to open HID device\n");
		hid_hw_stop(hdev);
		return ret;
	}

	if (!hid_get_drvdata(hdev)) {
		dev_dbg(&hdev->dev, "Started interface as generic HID device\n");
		return 0;
	}

	ret = hid_go_cfg_probe(hdev, id);
	if (ret)
		dev_err_probe(&hdev->dev, ret, "Failed to start configuration interface\n");

	dev_dbg(&hdev->dev, "Started Legion Go HID config interface\n");

	return ret;
}

static int hid_go_suspend(struct hid_device *hdev, pm_message_t msg)
{
	struct go_cfg_drvdata *drvdata = hid_get_drvdata(hdev);

	/* Interfaces without config state need no suspend handling */
	if (!drvdata)
		return 0;

	cancel_delayed_work_sync(&drvdata->go_cfg_setup);

	return 0;
}

static int hid_go_resume(struct hid_device *hdev)
{
	struct go_cfg_drvdata *drvdata = hid_get_drvdata(hdev);
	bool gp_registered, rgb_registered;
	int ret;

	/* Interfaces without config state need no resume handling */
	if (!drvdata)
		return 0;

	/* Pairs with smp_store_release in the config setup path */
	gp_registered = smp_load_acquire(&drvdata->gp_registered);
	/* Pairs with smp_store_release in the config setup path */
	rgb_registered = smp_load_acquire(&drvdata->rgb_registered);

	if (gp_registered && rgb_registered)
		return 0;

	/* Setup was interrupted before it could finish; retry it */
	ret = schedule_delayed_work(&drvdata->go_cfg_setup, msecs_to_jiffies(2));
	if (!ret) {
		dev_err(&hdev->dev, "Failed to schedule startup delayed work\n");
		return -ENODEV;
	}

	return 0;
}

static void hid_go_remove(struct hid_device *hdev)
{
	if (hid_get_drvdata(hdev)) {
		hid_go_cfg_remove(hdev);
		return;
	}

	hid_hw_close(hdev);
	hid_hw_stop(hdev);
}

static const struct hid_device_id hid_go_devices[] = {
	{ HID_USB_DEVICE(USB_VENDOR_ID_LENOVO,
			 USB_DEVICE_ID_LENOVO_LEGION_GO_XINPUT),
	  .driver_data = GO_GEN1 },
	{ HID_USB_DEVICE(USB_VENDOR_ID_LENOVO,
			 USB_DEVICE_ID_LENOVO_LEGION_GO_DINPUT),
	  .driver_data = GO_GEN1 },
	{ HID_USB_DEVICE(USB_VENDOR_ID_LENOVO,
			 USB_DEVICE_ID_LENOVO_LEGION_GO_FPS),
	  .driver_data = GO_GEN1 },
	{ HID_USB_DEVICE(USB_VENDOR_ID_LENOVO,
			 USB_DEVICE_ID_LENOVO_LEGION_GO2_XINPUT),
	  .driver_data = GO_GEN2 },
	{ HID_USB_DEVICE(USB_VENDOR_ID_LENOVO,
			 USB_DEVICE_ID_LENOVO_LEGION_GO2_DINPUT),
	  .driver_data = GO_GEN2 },
	{ HID_USB_DEVICE(USB_VENDOR_ID_LENOVO,
			 USB_DEVICE_ID_LENOVO_LEGION_GO2_DUAL_DINPUT),
	  .driver_data = GO_GEN2 },
	{ HID_USB_DEVICE(USB_VENDOR_ID_LENOVO,
			 USB_DEVICE_ID_LENOVO_LEGION_GO2_FPS),
	  .driver_data = GO_GEN2 },
	{}
};
MODULE_DEVICE_TABLE(hid, hid_go_devices);

static struct hid_driver hid_lenovo_go = {
	.name = "hid-lenovo-go",
	.id_table = hid_go_devices,
	.probe = hid_go_probe,
	.remove = hid_go_remove,
	.suspend = pm_ptr(hid_go_suspend),
	.resume = pm_ptr(hid_go_resume),
	.reset_resume = pm_ptr(hid_go_resume),
	.raw_event = hid_go_raw_event,
};
module_hid_driver(hid_lenovo_go);

MODULE_AUTHOR("Derek J. Clark");
MODULE_DESCRIPTION("HID Driver for Lenovo Legion Go Series Gamepads.");
MODULE_LICENSE("GPL");
