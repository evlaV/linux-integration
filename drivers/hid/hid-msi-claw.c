#include <linux/dmi.h>
#include <linux/hid.h>
#include <linux/led-class-multicolor.h>
#include <linux/leds.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/sysfs.h>
#include <linux/usb.h>

#include "hid-ids.h"

#define CLAW_FEATURE_GAMEPAD_REPORT_ID	0x0f

#define CLAW_PACKET_SIZE	64

#define CLAW_GAME_CONTROL_DESC	0x05
#define CLAW_DEVICE_CONTROL_DESC	0x06

/* LED constants */
#define CLAW_LED_ZONES	9
#define CLAW_LED_MAX_FRAMES	8

enum claw_led_effect {
	CLAW_LED_EFFECT_MONOCOLOR,
	CLAW_LED_EFFECT_BREATHE,
	CLAW_LED_EFFECT_CHROMA,
	CLAW_LED_EFFECT_RAINBOW,
	CLAW_LED_EFFECT_FROSTFIRE,

	CLAW_LED_EFFECT_MAX,
};

static const char * const led_effect_names[] = {
	[CLAW_LED_EFFECT_MONOCOLOR] =	"monocolor",
	[CLAW_LED_EFFECT_BREATHE] =	"breathe",
	[CLAW_LED_EFFECT_CHROMA] =	"chroma",
	[CLAW_LED_EFFECT_RAINBOW] =	"rainbow",
	[CLAW_LED_EFFECT_FROSTFIRE] =	"frostfire",
};

enum claw_led_mode {
	CLAW_LED_MODE_DYNAMIC,
	CLAW_LED_MODE_CUSTOM,

	CLAW_LED_MODE_MAX,
};

static const char * const led_mode_names[] = {
	[CLAW_LED_MODE_DYNAMIC] =	"dynamic",
	[CLAW_LED_MODE_CUSTOM] =	"custom",
};

struct claw_rgb_frame {
	u8 zones[CLAW_LED_ZONES][3];  /* 9 zones * RGB */
};

struct claw_rgb_config {
	u8 frame_count;   /* 1-8 */
	u8 speed;         /* 0-20 (0=fastest) */
	struct claw_rgb_frame frames[CLAW_LED_MAX_FRAMES];
};

/* Throttle interval for LED updates (in ms) */
#define CLAW_LED_THROTTLE_MS 50

struct claw_led {
	struct led_classdev_mc *mc_cdev;
	struct hid_device *hdev;

	/* State */
	bool enabled;
	bool config_loaded;  /* true if speed/brightness read from device */
	enum claw_led_mode mode;      /* dynamic or custom */
	enum claw_led_effect effect;  /* preset effect (when mode=dynamic) */
	u8 speed;          /* 0-100 (user value, mapped to 0-20 for device) */

	/* Custom effect keyframes cache */
	u8 custom_frame_count;
	u8 custom_frames[CLAW_LED_MAX_FRAMES][CLAW_LED_ZONES][3];

	/* Throttling for sysfs writes */
	struct delayed_work apply_work;
	unsigned long last_apply_jiffies;
};

enum claw_gamepad_mode {
	CLAW_GAMEPAD_MODE_OFFLINE,
	CLAW_GAMEPAD_MODE_XINPUT,
	CLAW_GAMEPAD_MODE_DINPUT,
	CLAW_GAMEPAD_MODE_MSI,
	CLAW_GAMEPAD_MODE_DESKTOP,
	CLAW_GAMEPAD_MODE_BIOS,
	CLAW_GAMEPAD_MODE_TESTING,

	CLAW_GAMEPAD_MODE_MAX,
};

enum claw_mkeys_function {
	CLAW_MKEY_FUNCTION_MACRO,
	CLAW_MKEY_FUNCTION_COMBINATION,
	CLAW_MKEY_FUNCTION_DISABLED,

	CLAW_MKEY_FUNCTION_MAX,
};

static const bool gamepad_mode_debug = false;

static const struct {
	const char* name;
	const bool available;
} gamepad_mode_map[] = {
	{"offline", gamepad_mode_debug},
	{"xinput", true},
	{"dinput", gamepad_mode_debug},
	{"msi", gamepad_mode_debug},
	{"desktop", true},
	{"bios", gamepad_mode_debug},
	{"testing", gamepad_mode_debug},
};

static const char* mkeys_function_map[] =
{
	"macro",
	"combination",
};

#define CLAW_M_REMAP_MAX_KEYS 5

enum claw_m_key {
	CLAW_M1_KEY = 0,
	CLAW_M2_KEY = 1,

	CLAW_M_KEY_MAX,
};

static const struct {
	const char *name;
	uint8_t code;
} m_remap_key_map[] = {
	/* Gamepad buttons */
	{ "BTN_DPAD_UP", 0x01 },
	{ "BTN_DPAD_DOWN", 0x02 },
	{ "BTN_DPAD_LEFT", 0x03 },
	{ "BTN_DPAD_RIGHT", 0x04 },
	{ "BTN_TL", 0x05 },
	{ "BTN_TR", 0x06 },
	{ "BTN_THUMBL", 0x07 },
	{ "BTN_THUMBR", 0x08 },
	{ "BTN_SOUTH", 0x09 },
	{ "BTN_EAST", 0x0a },
	{ "BTN_NORTH", 0x0b },
	{ "BTN_WEST", 0x0c },
	{ "BTN_MODE", 0x0d },
	{ "BTN_SELECT", 0x0e },
	{ "BTN_START", 0x0f },
	/* Keyboard keys */
	{ "KEY_ESC", 0x32 },
	{ "KEY_F1", 0x33 },
	{ "KEY_F2", 0x34 },
	{ "KEY_F3", 0x35 },
	{ "KEY_F4", 0x36 },
	{ "KEY_F5", 0x37 },
	{ "KEY_F6", 0x38 },
	{ "KEY_F7", 0x39 },
	{ "KEY_F8", 0x3a },
	{ "KEY_F9", 0x3b },
	{ "KEY_F10", 0x3c },
	{ "KEY_F11", 0x3d },
	{ "KEY_F12", 0x3e },
	{ "KEY_GRAVE", 0x3f },
	{ "KEY_1", 0x40 },
	{ "KEY_2", 0x41 },
	{ "KEY_3", 0x42 },
	{ "KEY_4", 0x43 },
	{ "KEY_5", 0x44 },
	{ "KEY_6", 0x45 },
	{ "KEY_7", 0x46 },
	{ "KEY_8", 0x47 },
	{ "KEY_9", 0x48 },
	{ "KEY_0", 0x49 },
	{ "KEY_MINUS", 0x4a },
	{ "KEY_EQUAL", 0x4b },
	{ "KEY_BACKSPACE", 0x4c },
	{ "KEY_TAB", 0x4d },
	{ "KEY_Q", 0x4e },
	{ "KEY_W", 0x4f },
	{ "KEY_E", 0x50 },
	{ "KEY_R", 0x51 },
	{ "KEY_T", 0x52 },
	{ "KEY_Y", 0x53 },
	{ "KEY_U", 0x54 },
	{ "KEY_I", 0x55 },
	{ "KEY_O", 0x56 },
	{ "KEY_P", 0x57 },
	{ "KEY_LEFTBRACE", 0x58 },
	{ "KEY_RIGHTBRACE", 0x59 },
	{ "KEY_BACKSLASH", 0x5a },
	{ "KEY_CAPSLOCK", 0x5b },
	{ "KEY_A", 0x5c },
	{ "KEY_S", 0x5d },
	{ "KEY_D", 0x5e },
	{ "KEY_F", 0x5f },
	{ "KEY_G", 0x60 },
	{ "KEY_H", 0x61 },
	{ "KEY_J", 0x62 },
	{ "KEY_K", 0x63 },
	{ "KEY_L", 0x64 },
	{ "KEY_SEMICOLON", 0x65 },
	{ "KEY_LEFTSHIFT", 0x66 },
	{ "KEY_APOSTROPHE", 0x67 },
	{ "KEY_ENTER", 0x68 },
	{ "KEY_Z", 0x69 },
	{ "KEY_X", 0x6a },
	{ "KEY_C", 0x6b },
	{ "KEY_V", 0x6c },
	{ "KEY_B", 0x6d },
	{ "KEY_N", 0x6e },
	{ "KEY_M", 0x6f },
	{ "KEY_LEFTCTRL", 0x70 },
	{ "KEY_RIGHTSHIFT", 0x71 },
	{ "KEY_COMMA", 0x72 },
	{ "KEY_DOT", 0x73 },
	{ "KEY_SLASH", 0x74 },
	{ "KEY_LEFTALT", 0x75 },
	{ "KEY_LEFTMETA", 0x76 },
	{ "KEY_RIGHTCTRL", 0x77 },
	{ "KEY_RIGHTALT", 0x78 },
	{ "KEY_SPACE", 0x79 },
	{ "KEY_INSERT", 0x7a },
	{ "KEY_HOME", 0x7b },
	{ "KEY_PAGEUP", 0x7c },
	{ "KEY_DELETE", 0x7d },
	{ "KEY_END", 0x7e },
	{ "KEY_PAGEDOWN", 0x7f },
	{ "KEY_KPENTER", 0x8a },
	{ "KEY_KP0", 0x8b },
	{ "KEY_KP1", 0x8c },
	{ "KEY_KP2", 0x8d },
	{ "KEY_KP3", 0x8e },
	{ "KEY_KP4", 0x8f },
	{ "KEY_KP5", 0x90 },
	{ "KEY_KP6", 0x91 },
	{ "KEY_KP7", 0x92 },
	{ "KEY_KP8", 0x93 },
	{ "KEY_KP9", 0x94 },
	/* Disabled */
	{ "disabled", 0xff },
};

static const uint8_t m_remap_addr_old[CLAW_M_KEY_MAX][2] = {
	{0x00, 0x7a},  /* M1 */
	{0x01, 0x1f},  /* M2 */
};

static const uint8_t m_remap_addr_new[CLAW_M_KEY_MAX][2] = {
	{0x00, 0xbb},  /* M1 */
	{0x01, 0x64},  /* M2 */
};

/* RGB LED addresses (firmware version dependent) */
static const uint8_t rgb_addr_old[2] = {0x01, 0xfa};
static const uint8_t rgb_addr_new[2] = {0x02, 0x4a};

enum claw_command_type {
	CLAW_COMMAND_TYPE_ENTER_PROFILE_CONFIG =	0x01,
	CLAW_COMMAND_TYPE_EXIT_PROFILE_CONFIG =		0x02,
	CLAW_COMMAND_TYPE_WRITE_PROFILE =		0x03,
	CLAW_COMMAND_TYPE_READ_PROFILE =		0x04,
	CLAW_COMMAND_TYPE_READ_PROFILE_ACK =		0x05,
	// ACK is read after a WRITE_PROFILE_DATA
	CLAW_COMMAND_TYPE_ACK =				0x06,
	CLAW_COMMAND_TYPE_SWITCH_PROFILE =		0x07,
	CLAW_COMMAND_TYPE_WRITE_PROFILE_TO_EEPROM =	0x08,
	CLAW_COMMAND_TYPE_SYNC_RGB =			0x09,
	CLAW_COMMAND_TYPE_READ_RGB_STATUS_ACK =		0x0a,
	CLAW_COMMAND_TYPE_READ_CURRENT_PROFILE =	0x0b,
	CLAW_COMMAND_TYPE_READ_CURRENT_PROFILE_ACK =	0x0c,
	CLAW_COMMAND_TYPE_READ_RGB_STATUS =		0x0d,
	// Write profile data (M1/M2 remap, RGB settings, etc.)
	CLAW_COMMAND_TYPE_WRITE_PROFILE_DATA =		0x21,
	CLAW_COMMAND_TYPE_SYNC_TO_ROM =			0x22,
	CLAW_COMMAND_TYPE_RESTORE_FROM_ROM =		0x23,
	CLAW_COMMAND_TYPE_SWITCH_MODE =			0x24,
	CLAW_COMMAND_TYPE_READ_GAMEPAD_MODE =		0x26,
	CLAW_COMMAND_TYPE_GAMEPAD_MODE_ACK =		0x27,
	CLAW_COMMAND_TYPE_RESET_DEVICE =		0x28,
	CLAW_COMMAND_TYPE_RGB_CONTROL =			0xe0,
	CLAW_COMMAND_TYPE_CALIBRATION_CONTROL =		0xfd,
	CLAW_COMMAND_TYPE_CALIBRATION_ACK =		0xfe,
};

struct claw_control_status {
	enum claw_gamepad_mode gamepad_mode;
	enum claw_mkeys_function mkeys_function;
};

struct claw_read_data {
	const uint8_t *data;
	int size;

	struct claw_read_data *next;
};

struct claw_drvdata {
	struct hid_device *hdev;

	//struct input_dev *input;

	struct claw_control_status *control;

	struct mutex read_data_mutex;
	struct claw_read_data *read_data;

	/* M key remap support */
	u16 bcd_device;
	bool m_remap_supported;
	const uint8_t (*m_remap_addr)[2];

	/* RGB LED support */
	struct claw_led *led_cfg;
	const uint8_t *rgb_addr;
};

static void claw_flush_queue(struct hid_device *hdev);

static int claw_write_cmd(struct hid_device *hdev, enum claw_command_type cmdtype,
    const uint8_t *const buffer, size_t buffer_len)
{
	int ret;
	uint8_t *dmabuf = NULL;
	struct claw_drvdata *drvdata = hid_get_drvdata(hdev);
	const uint8_t buf[CLAW_PACKET_SIZE] = {
		CLAW_FEATURE_GAMEPAD_REPORT_ID, 0, 0, 0x3c, cmdtype };

	claw_flush_queue(hdev);

	if (!drvdata->control) {
		hid_err(hdev, "hid-msi-claw couldn't find control interface\n");
		ret = -ENODEV;
		goto claw_write_cmd_err;
	}

	if (buffer != NULL)
		memcpy((void *)&buf[5], buffer, buffer_len);
	else
		buffer_len = 0;

	memset((void *)&buf[5 + buffer_len], 0, CLAW_PACKET_SIZE - (5 + buffer_len));
	dmabuf = kmemdup(buf, CLAW_PACKET_SIZE, GFP_KERNEL);
	if (!dmabuf) {
		ret = -ENOMEM;
		hid_err(hdev, "hid-msi-claw failed to alloc dma buf: %d\n", ret);
		goto claw_write_cmd_err;
	}

	ret = hid_hw_output_report(hdev, dmabuf, CLAW_PACKET_SIZE);
	if (ret != CLAW_PACKET_SIZE) {
		hid_err(hdev, "hid-msi-claw failed to switch controller mode: %d\n", ret);
		goto claw_write_cmd_err;
	}

	hid_notice(hdev, "hid-msi-claw sent %d bytes, cmd: 0x%02x\n", ret, dmabuf[4]);

claw_write_cmd_err:
	kfree(dmabuf);

	return ret;
}

static int claw_read(struct hid_device *hdev, uint8_t *const buffer, int size, uint32_t timeout)
{
	struct claw_drvdata *drvdata = hid_get_drvdata(hdev);
	struct claw_read_data *event = NULL;
	int ret = 0;

	if (!drvdata->control) {
		hid_err(hdev, "hid-msi-claw couldn't find control interface\n");
		ret = -ENODEV;
		goto claw_read_err;
	}

	for (uint32_t i = 0; (event == NULL) && (i <= timeout); i++) {
		if (timeout - i)
			msleep(20);

		scoped_guard(mutex, &drvdata->read_data_mutex) {
			event = drvdata->read_data;

			if (event != NULL)
				drvdata->read_data = event->next;
		};
	}

	if (event == NULL) {
		ret = -EIO;
		hid_err(hdev, "hid-msi-claw no answer from device\n");
		goto claw_read_err;
	}

	if (size < event->size) {
		ret = -EINVAL;
		hid_err(hdev, "hid-msi-claw invalid buffer size: too short\n");
		goto claw_read_err;
	}

	memcpy((void *)buffer, (const void *)event->data, event->size);
	ret = event->size;

claw_read_err:
	if (event != NULL) {
		kfree(event->data);
		kfree(event);
	}

	return ret;
}

static void claw_flush_queue(struct hid_device *hdev)
{
	struct claw_drvdata *drvdata = hid_get_drvdata(hdev);
	struct claw_read_data *event, *next;

	scoped_guard(mutex, &drvdata->read_data_mutex) {
		event = drvdata->read_data;
		drvdata->read_data = NULL;

		while (event) {
			next = event->next;
			kfree(event->data);
			kfree(event);
			event = next;
		}
	}
}

static int claw_raw_event_control(struct hid_device *hdev, struct claw_drvdata *drvdata,
	struct hid_report *report, uint8_t *data, int size)
{
	struct claw_read_data **list = NULL;
	struct claw_read_data *node = NULL;
	uint8_t *buffer;
	int ret, i;

	if (size != CLAW_PACKET_SIZE) {
		ret = 0;//-EIO;
		//hid_err(hdev, "hid-msi-claw got unknown %d bytes\n", size);
		goto control_err;
	} else if (data[0] != 0x10) {
		ret = 0;//-EIO;
		hid_err(hdev, "hid-msi-claw unrecognised byte at offset 0: expected 0x10, got 0x%02x\n", data[0]);
		goto control_err;
	} else if (data[1] != 0x00) {
		ret = 0;//-EIO;
		hid_err(hdev, "hid-msi-claw unrecognised byte at offset 1: expected 0x00, got 0x%02x\n", data[1]);
		goto control_err;
	} else if (data[2] != 0x00) {
		ret = 0;//-EIO;
		hid_err(hdev, "hid-msi-claw unrecognised byte at offset 2: expected 0x00, got 0x%02x\n", data[2]);
		goto control_err;
	} else if (data[3] != 0x3c) {
		ret = 0;//-EIO;
		hid_err(hdev, "hid-msi-claw unrecognised byte at offset 3: expected 0x3c, got 0x%02x\n", data[3]);
		goto control_err;
	}

	buffer = kmemdup(data, size, GFP_KERNEL);
	if (buffer == NULL) {
		ret = -ENOMEM;
		hid_err(hdev, "hid-msi-claw failed to alloc %d bytes for read buffer: %d\n", size, ret);
		goto control_err;
	}

	struct claw_read_data evt = {
		.data = buffer,
		.size = size,
		.next = NULL,
	};

	node = kmemdup(&evt, sizeof(evt), GFP_KERNEL);
	if (!node) {
		ret = -ENOMEM;
		kfree(buffer);
		hid_err(hdev, "hid-msi-claw failed to alloc event node: %d\n", ret);
		goto cleanup_buffer;
	}

	scoped_guard(mutex, &drvdata->read_data_mutex) {
		list = &drvdata->read_data;
		for (i = 0; (i < 32) && (*list != NULL); i++)
			list = &(*list)->next;

		if (*list != NULL) {
			ret = -EIO;
			hid_err(hdev, "too many unparsed events: ignoring\n");
			goto cleanup_node;
		}

		*list = node;
	}

	hid_notice(hdev, "hid-msi-claw received %d bytes, cmd: 0x%02x\n", size, buffer[4]);

	return 0;

cleanup_node:
	kfree(node);
cleanup_buffer:
	kfree(buffer);
control_err:
	return ret;
}

static int claw_raw_event(struct hid_device *hdev, struct hid_report *report, uint8_t *data, int size)
{
	struct claw_drvdata *drvdata = hid_get_drvdata(hdev);

	if (!drvdata->control) {
		hid_notice(hdev, "hid-msi-claw event not from control interface: ignoring\n");
		return 0;
	}

	return claw_raw_event_control(hdev, drvdata, report, data, size);
}

static int claw_await_ack(struct hid_device *hdev)
{
	struct claw_drvdata *drvdata = hid_get_drvdata(hdev);
	uint8_t buffer[CLAW_PACKET_SIZE];
	int ret;

	if (!drvdata->control) {
		hid_err(hdev, "hid-msi-claw couldn't find control interface\n");
		return -ENODEV;
	}

	ret = claw_read(hdev, buffer, CLAW_PACKET_SIZE, 1000);
	if (ret < 0) {
		hid_err(hdev, "hid-msi-claw failed to read ack: %d\n", ret);
		return ret;
	} else if (ret != CLAW_PACKET_SIZE) {
		hid_err(hdev, "hid-msi-claw invalid read: expected %d bytes, got %d\n", CLAW_PACKET_SIZE, ret);
		return -EINVAL;
	}

	if (buffer[4] != (uint8_t)CLAW_COMMAND_TYPE_ACK) {
		hid_err(hdev, "hid-msi-claw expected ACK (0x06), got 0x%02x\n", buffer[4]);
		return -EINVAL;
	}

	return 0;
}

static int sync_to_rom(struct hid_device *hdev)
{
	struct claw_drvdata *drvdata = hid_get_drvdata(hdev);
	int ret;

	if (!drvdata->control) {
		hid_err(hdev, "hid-msi-claw couldn't find control interface\n");
		ret = -ENODEV;
		goto sync_to_rom_err;
	}

	ret = claw_write_cmd(hdev, CLAW_COMMAND_TYPE_SYNC_TO_ROM, NULL, 0);
	if (ret < 0) {
		hid_err(hdev, "hid-msi-claw failed to send write request for switch controller mode: %d\n", ret);
		goto sync_to_rom_err;
	} else if (ret != CLAW_PACKET_SIZE) {
		hid_err(hdev, "hid-msi-claw failed to send the sync to rom command: %d\n", ret);
		ret = -EIO;
		goto sync_to_rom_err;
	}

	ret = claw_await_ack(hdev);
	if (ret) {
		hid_err(hdev, "hid-msi-claw failed to await ack: %d\n", ret);
		goto sync_to_rom_err;
	}

	ret = 0;

sync_to_rom_err:
	return ret;
}

/* ========== RGB LED Functions ========== */

/*
 * Convert HSV to RGB
 * h: 0-359, s: 0-255, v: 0-255
 */
static void claw_hsv_to_rgb(u16 h, u8 s, u8 v, u8 *r, u8 *g, u8 *b)
{
	u8 region, remainder, p, q, t;

	if (s == 0) {
		*r = *g = *b = v;
		return;
	}

	region = h / 60;
	remainder = (h - (region * 60)) * 255 / 60;

	p = (v * (255 - s)) >> 8;
	q = (v * (255 - ((s * remainder) >> 8))) >> 8;
	t = (v * (255 - ((s * (255 - remainder)) >> 8))) >> 8;

	switch (region) {
	case 0:
		*r = v; *g = t; *b = p;
		break;
	case 1:
		*r = q; *g = v; *b = p;
		break;
	case 2:
		*r = p; *g = v; *b = t;
		break;
	case 3:
		*r = p; *g = q; *b = v;
		break;
	case 4:
		*r = t; *g = p; *b = v;
		break;
	default:
		*r = v; *g = p; *b = q;
		break;
	}
}

/* Fill all zones with the same color */
static void claw_frame_fill_solid(struct claw_rgb_frame *frame,
				       u8 r, u8 g, u8 b)
{
	int i;

	for (i = 0; i < CLAW_LED_ZONES; i++) {
		frame->zones[i][0] = r;
		frame->zones[i][1] = g;
		frame->zones[i][2] = b;
	}
}

/* Send RGB configuration to device */
static int claw_send_rgb_config(struct hid_device *hdev,
				    struct claw_rgb_config *cfg)
{
	struct claw_drvdata *drvdata = hid_get_drvdata(hdev);
	u8 cmd_buffer[64];  /* 4 byte header + max 55 bytes data + padding */
	int ret, frame, offset, chunk_size;
	u16 base_addr, current_addr;
	u8 *data;
	int data_size;

	if (!drvdata->control) {
		hid_err(hdev, "hid-msi-claw LED: no control interface\n");
		return -ENODEV;
	}

	if (!drvdata->rgb_addr) {
		hid_err(hdev, "hid-msi-claw LED: no RGB address\n");
		return -ENODEV;
	}

	/* Build complete RGB data */
	/* Header: 5 bytes + frames * 27 bytes */
	data_size = 5 + cfg->frame_count * 27;
	data = kzalloc(data_size, GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	/* Header */
	data[0] = 0;  /* index */
	data[1] = cfg->frame_count;
	data[2] = 0x09;  /* effect type */
	data[3] = cfg->speed;  /* Device speed 0-20, 0=fastest */
	data[4] = drvdata->led_cfg->mc_cdev->led_cdev.brightness;

	/* Keyframes */
	for (frame = 0; frame < cfg->frame_count; frame++) {
		u8 *frame_data = &data[5 + frame * 27];
		int zone;

		for (zone = 0; zone < CLAW_LED_ZONES; zone++) {
			frame_data[zone * 3 + 0] = cfg->frames[frame].zones[zone][0];
			frame_data[zone * 3 + 1] = cfg->frames[frame].zones[zone][1];
			frame_data[zone * 3 + 2] = cfg->frames[frame].zones[zone][2];
		}
	}

	/* Calculate base address */
	base_addr = (drvdata->rgb_addr[0] << 8) | drvdata->rgb_addr[1];

	/* Send data in chunks (max 55 bytes per packet) */
	offset = 0;
	while (offset < data_size) {
		chunk_size = min(55, data_size - offset);
		current_addr = base_addr + offset;

		/* Build command buffer */
		cmd_buffer[0] = 0x01;  /* profile */
		cmd_buffer[1] = (current_addr >> 8) & 0xff;
		cmd_buffer[2] = current_addr & 0xff;
		cmd_buffer[3] = chunk_size;
		memcpy(&cmd_buffer[4], &data[offset], chunk_size);

		ret = claw_write_cmd(hdev, CLAW_COMMAND_TYPE_WRITE_PROFILE_DATA,
					  cmd_buffer, 4 + chunk_size);
		if (ret < 0) {
			hid_err(hdev, "hid-msi-claw LED: failed to send RGB data: %d\n", ret);
			goto out;
		}

		ret = claw_await_ack(hdev);
		if (ret) {
			hid_err(hdev, "hid-msi-claw LED: failed to await ack: %d\n", ret);
			goto out;
		}

		offset += chunk_size;
	}

	ret = 0;

out:
	kfree(data);
	return ret;
}

/* Read RGB config from device */
static int claw_read_rgb_config(struct hid_device *hdev,
				    u8 *speed, int *brightness)
{
	struct claw_drvdata *drvdata = hid_get_drvdata(hdev);
	u8 cmd_buffer[4];
	u8 buffer[CLAW_PACKET_SIZE] = {};
	u8 device_speed;
	int ret;

	if (!drvdata->control) {
		hid_err(hdev, "hid-msi-claw LED: no control interface\n");
		return -ENODEV;
	}

	if (!drvdata->rgb_addr) {
		hid_err(hdev, "hid-msi-claw LED: no RGB address\n");
		return -ENODEV;
	}

	cmd_buffer[0] = 0x01;  /* profile */
	cmd_buffer[1] = drvdata->rgb_addr[0];
	cmd_buffer[2] = drvdata->rgb_addr[1];
	cmd_buffer[3] = 5;  /* read 5 bytes (header only) */

	/* Flush any pending data before reading */
	claw_flush_queue(hdev);

	ret = claw_write_cmd(hdev, CLAW_COMMAND_TYPE_READ_PROFILE,
				 cmd_buffer, sizeof(cmd_buffer));
	if (ret < 0) {
		hid_err(hdev, "hid-msi-claw LED: failed to send read request: %d\n", ret);
		return ret;
	}

	ret = claw_read(hdev, buffer, CLAW_PACKET_SIZE, 100);
	if (ret != CLAW_PACKET_SIZE) {
		hid_err(hdev, "hid-msi-claw LED: failed to read config: %d\n", ret);
		return -EINVAL;
	}

	if (buffer[4] != (u8)CLAW_COMMAND_TYPE_READ_PROFILE_ACK) {
		hid_err(hdev, "hid-msi-claw LED: expected READ_PROFILE_ACK (0x05), got 0x%02x\n",
			buffer[4]);
		return -EINVAL;
	}

	/* Debug: print buffer contents */
	hid_info(hdev, "LED read buffer[0..15]: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x\n",
		 buffer[0], buffer[1], buffer[2], buffer[3], buffer[4], buffer[5],
		 buffer[6], buffer[7], buffer[8], buffer[9], buffer[10], buffer[11],
		 buffer[12], buffer[13], buffer[14], buffer[15]);

	/* Data starts at buffer[10] (differs from m_remap which uses buffer[11]) */
	/* buffer[10] = frame_count, buffer[11] = effect_type */
	/* buffer[12] = speed (0-20), buffer[13] = brightness (0-100) */
	device_speed = buffer[12];
	*brightness = buffer[13];

	/* Convert device speed to user speed */
	/* device_speed = (100 - user_speed) * 20 / 100 */
	/* user_speed = 100 - device_speed * 100 / 20 */
	if (device_speed > 20)
		device_speed = 20;
	*speed = 100 - device_speed * 100 / 20;

	hid_info(hdev, "hid-msi-claw LED: read config: device_speed=%d -> speed=%d, brightness=%d\n",
		 device_speed, *speed, *brightness);

	return 0;
}

/* Lazy load config from device on first access */
static void claw_led_lazy_load(struct claw_led *led)
{
	if (led->config_loaded)
		return;

	led->config_loaded = true;

	if (claw_read_rgb_config(led->hdev, &led->speed, &led->mc_cdev->led_cdev.brightness) < 0) {
		hid_warn(led->hdev, "hid-msi-claw LED: lazy load failed, using defaults\n");
			 led->speed = 50; led->mc_cdev->led_cdev.brightness = 100;
	}
}

/*
 * Speed conversion helpers
 * User speed: 0-100 (100 = fastest)
 * Device speed: 0-20 (0 = fastest, acts like frame interval)
 */

/* Convert user speed (0-100, 100=fastest) to device speed (0-20, 0=fastest) */
static inline u8 claw_speed_to_device(u8 user_speed)
{
	return (100 - user_speed) * 20 / 100;
}

/*
 * Map user speed (0-100) to a sub-range, then convert to device speed
 * This allows multi-frame effects to have compensated speed ranges
 *
 * range_slow: output speed when user_speed=0 (user scale 0-100, 0=slowest)
 * range_fast: output speed when user_speed=100 (user scale 0-100, 100=fastest)
 *
 * Example: claw_speed_map(speed, 10, 50)
 *   user=0   -> mapped=10 -> device=18 (slow)
 *   user=100 -> mapped=50 -> device=10 (fast)
 *   user=50  -> mapped=30 -> device=14 (medium)
 */
static inline u8 claw_speed_map(u8 user_speed, u8 range_slow, u8 range_fast)
{
	u8 mapped;

	/* Map user 0-100 to range_slow-range_fast */
	mapped = range_slow + (range_fast - range_slow) * user_speed / 100;
	/* Convert to device speed (invert: 0=fast, 20=slow) */
	return (100 - mapped) * 20 / 100;
}

/* Build solid effect (1 frame, all zones same color) */
static void claw_build_monocolor(struct claw_rgb_config *cfg,
				     struct claw_led *led)
{
	struct mc_subled *color = led->mc_cdev->subled_info;

	cfg->frame_count = 1;
	cfg->speed = 0;  /* Static effect, speed doesn't matter */

	claw_frame_fill_solid(&cfg->frames[0], color[0].brightness,
				  color[1].brightness, color[2].brightness);
}

/* Build breathe effect (2 frames: color -> black) */
static void claw_build_breathe(struct claw_rgb_config *cfg,
				    struct claw_led *led)
{
	struct mc_subled *color = led->mc_cdev->subled_info;

	cfg->frame_count = 2;
	/* breathe (2 frames): map to range */
	cfg->speed = claw_speed_map(led->speed, 5, 100);

	/* Frame 0: main color */
	claw_frame_fill_solid(&cfg->frames[0], color[0].brightness,
				  color[1].brightness, color[2].brightness);
	/* Frame 1: black */
	claw_frame_fill_solid(&cfg->frames[1], 0, 0, 0);
}

/* Build chroma effect (6 frames: rainbow cycle, all zones sync) */
static void claw_build_chroma(struct claw_rgb_config *cfg,
				   struct claw_led *led)
{
	static const u16 hues[] = {0, 60, 120, 180, 240, 300};
	int i;
	u8 r, g, b;

	cfg->frame_count = 6;
	/* chroma (6 frames): map to range for slower animation */
	cfg->speed = claw_speed_map(led->speed, 5, 100);

	for (i = 0; i < 6; i++) {
		claw_hsv_to_rgb(hues[i], 255, 255, &r, &g, &b);
		claw_frame_fill_solid(&cfg->frames[i], r, g, b);
	}
}

/* Build rainbow effect (4 frames: rotating colors around joysticks) */
static void claw_build_rainbow(struct claw_rgb_config *cfg,
				    struct claw_led *led)
{
	static const u16 base_hues[] = {0, 90, 180, 270};
	int frame, zone;
	u8 r, g, b;
	u16 hue;

	cfg->frame_count = 4;
	/* rainbow (4 frames): map to range */
	cfg->speed = claw_speed_map(led->speed, 10, 100);

	for (frame = 0; frame < 4; frame++) {
		/* Right joystick (zones 0-3): rotate */
		for (zone = 0; zone < 4; zone++) {
			hue = base_hues[(zone + frame) % 4];
			claw_hsv_to_rgb(hue, 255, 255, &r, &g, &b);
			cfg->frames[frame].zones[zone][0] = r;
			cfg->frames[frame].zones[zone][1] = g;
			cfg->frames[frame].zones[zone][2] = b;
		}
		/* Left joystick (zones 4-7): rotate */
		for (zone = 0; zone < 4; zone++) {
			hue = base_hues[(zone + frame) % 4];
			claw_hsv_to_rgb(hue, 255, 255, &r, &g, &b);
			cfg->frames[frame].zones[zone + 4][0] = r;
			cfg->frames[frame].zones[zone + 4][1] = g;
			cfg->frames[frame].zones[zone + 4][2] = b;
		}
		/* ABXY (zone 8): cycle */
		hue = base_hues[frame];
		claw_hsv_to_rgb(hue, 255, 255, &r, &g, &b);
		cfg->frames[frame].zones[8][0] = r;
		cfg->frames[frame].zones[8][1] = g;
		cfg->frames[frame].zones[8][2] = b;
	}
}

/*
 * Build frostfire effect (4 frames: fire vs ice rotating)
 * Right joystick: fire red -> dark -> ice blue -> dark (clockwise)
 * Left joystick: ice blue -> dark -> fire red -> dark (counter-clockwise)
 * ABXY: fire red -> dark -> ice blue -> dark
 */
static void claw_build_frostfire(struct claw_rgb_config *cfg,
				     struct claw_led *led)
{
	/* Fire red, dark, ice blue, dark */
	static const u8 right_colors[4][3] = {
		{255, 0, 0},   /* Fire red */
		{0, 0, 0},     /* Dark */
		{0, 0, 255},   /* Ice blue */
		{0, 0, 0},     /* Dark */
	};
	/* Ice blue, dark, fire red, dark (opposite phase) */
	static const u8 left_colors[4][3] = {
		{0, 0, 255},   /* Ice blue */
		{0, 0, 0},     /* Dark */
		{255, 0, 0},   /* Fire red */
		{0, 0, 0},     /* Dark */
	};
	int frame, zone, color_idx;

	cfg->frame_count = 4;
	/* frostfire (4 frames): map to range */
	cfg->speed = claw_speed_map(led->speed, 10, 100);

	for (frame = 0; frame < 4; frame++) {
		/* Right joystick (zones 0-3): clockwise rotation */
		for (zone = 0; zone < 4; zone++) {
			color_idx = (zone + frame) % 4;
			cfg->frames[frame].zones[zone][0] = right_colors[color_idx][0];
			cfg->frames[frame].zones[zone][1] = right_colors[color_idx][1];
			cfg->frames[frame].zones[zone][2] = right_colors[color_idx][2];
		}
		/* Left joystick (zones 4-7): counter-clockwise rotation */
		for (zone = 0; zone < 4; zone++) {
			/* Subtract to rotate counter-clockwise, add 4 to keep positive */
			color_idx = (zone - frame + 4) % 4;
			cfg->frames[frame].zones[zone + 4][0] = left_colors[color_idx][0];
			cfg->frames[frame].zones[zone + 4][1] = left_colors[color_idx][1];
			cfg->frames[frame].zones[zone + 4][2] = left_colors[color_idx][2];
		}
		/* ABXY (zone 8): cycle same as right */
		cfg->frames[frame].zones[8][0] = right_colors[frame][0];
		cfg->frames[frame].zones[8][1] = right_colors[frame][1];
		cfg->frames[frame].zones[8][2] = right_colors[frame][2];
	}
}

/* Build custom effect from cached keyframes */
static void claw_build_custom(struct claw_rgb_config *cfg,
				   struct claw_led *led)
{
	int frame, zone;

	cfg->frame_count = led->custom_frame_count;
	/* Custom: direct conversion, no compensation (user controls frame count) */
	cfg->speed = claw_speed_to_device(led->speed);

	for (frame = 0; frame < led->custom_frame_count; frame++) {
		for (zone = 0; zone < CLAW_LED_ZONES; zone++) {
			cfg->frames[frame].zones[zone][0] = led->custom_frames[frame][zone][0];
			cfg->frames[frame].zones[zone][1] = led->custom_frames[frame][zone][1];
			cfg->frames[frame].zones[zone][2] = led->custom_frames[frame][zone][2];
		}
	}
}

/* Apply current effect to device */
static int claw_apply_effect(struct claw_led *led)
{
	struct mc_subled *color = led->mc_cdev->subled_info;
	struct claw_rgb_config cfg = {};
	int ret;

	hid_dbg(led->hdev,
		"LED apply: enabled=%d mode=%s effect=%s speed=%d brightness=%d color=[%d,%d,%d]\n",
		led->enabled, led_mode_names[led->mode], led_effect_names[led->effect],
		led->speed, led->mc_cdev->led_cdev.brightness, color[0].brightness,
		color[1].brightness, color[2].brightness);

	if (!led->enabled) {
		/* Send black frame with brightness 0 */
		cfg.frame_count = 1;
		cfg.speed = 0;
		claw_frame_fill_solid(&cfg.frames[0], 0, 0, 0);
		hid_info(led->hdev, "LED disabled, sending black frame\n");
		return claw_send_rgb_config(led->hdev, &cfg);
	}

	if (led->mode == CLAW_LED_MODE_CUSTOM) {
		/* Custom mode: use keyframes */
		if (led->custom_frame_count == 0) {
			hid_info(led->hdev, "LED custom: no keyframes, fallback to monocolor\n");
			claw_build_monocolor(&cfg, led);
		} else {
			claw_build_custom(&cfg, led);
		}
	} else {
		/* Dynamic mode: use preset effect */
		switch (led->effect) {
		case CLAW_LED_EFFECT_MONOCOLOR:
			claw_build_monocolor(&cfg, led);
			break;
		case CLAW_LED_EFFECT_BREATHE:
			claw_build_breathe(&cfg, led);
			break;
		case CLAW_LED_EFFECT_CHROMA:
			claw_build_chroma(&cfg, led);
			break;
		case CLAW_LED_EFFECT_RAINBOW:
			claw_build_rainbow(&cfg, led);
			break;
		case CLAW_LED_EFFECT_FROSTFIRE:
			claw_build_frostfire(&cfg, led);
			break;
		default:
			return -EINVAL;
		}
	}

	hid_dbg(led->hdev, "LED sending: frames=%d speed=%d brightness=%d\n",
		cfg.frame_count, cfg.speed, led->mc_cdev->led_cdev.brightness);

	ret = claw_send_rgb_config(led->hdev, &cfg);
	if (ret)
		hid_err(led->hdev, "LED send failed: %d\n", ret);

	return ret;
}

/* Delayed work callback for throttled LED updates */
static void claw_led_apply_work_fn(struct work_struct *work)
{
	struct claw_led *led = container_of(work, struct claw_led,
						apply_work.work);

	led->last_apply_jiffies = jiffies;
	claw_apply_effect(led);
}

/*
 * Throttled apply: limits update rate while ensuring final value is sent.
 * Always uses delayed_work to avoid blocking sysfs writes.
 * This matches the async behavior of the LED subsystem's brightness handling.
 */
static void claw_led_apply_throttled(struct claw_led *led)
{
	unsigned long elapsed_ms;
	unsigned long delay_ms;

	if (!led->enabled)
		return;

	elapsed_ms = jiffies_to_msecs(jiffies - led->last_apply_jiffies);

	/* Cancel any pending work and reschedule */
	cancel_delayed_work(&led->apply_work);

	if (elapsed_ms >= CLAW_LED_THROTTLE_MS) {
		/* Enough time passed, schedule for immediate execution */
		delay_ms = 0;
	} else {
		/* Too soon, delay until throttle interval passes */
		delay_ms = CLAW_LED_THROTTLE_MS - elapsed_ms;
	}

	schedule_delayed_work(&led->apply_work, msecs_to_jiffies(delay_ms));
}

/* ========== LED sysfs attributes ========== */

/* Helper to get claw_led from LED device */
static inline struct claw_led *dev_to_claw_led(struct device *dev)
{
	struct led_classdev *led_cdev = dev_get_drvdata(dev);
	struct led_classdev_mc *mc_cdev = lcdev_to_mccdev(led_cdev);

	return container_of(&mc_cdev, struct claw_led, mc_cdev);
}

static ssize_t enabled_show(struct device *dev,
			    struct device_attribute *attr, char *buf)
{
	struct claw_led *led = dev_to_claw_led(dev);

	return sysfs_emit(buf, "%s\n", led->enabled ? "true" : "false");
}

static ssize_t enabled_store(struct device *dev,
			     struct device_attribute *attr,
			     const char *buf, size_t count)
{
	struct claw_led *led = dev_to_claw_led(dev);
	bool val;
	int ret;

	ret = kstrtobool(buf, &val);
	if (ret)
		return ret;

	hid_info(led->hdev, "LED enabled_store: %s -> %s\n",
		 led->enabled ? "true" : "false", val ? "true" : "false");

	if (val == led->enabled)
		return count;

	led->enabled = val;
	ret = claw_apply_effect(led);

	return ret ? ret : count;
}
static DEVICE_ATTR_RW(enabled);

static ssize_t enabled_index_show(struct device *dev,
				  struct device_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "true false\n");
}
static DEVICE_ATTR_RO(enabled_index);

static ssize_t effect_show(struct device *dev,
			   struct device_attribute *attr, char *buf)
{
	struct claw_led *led = dev_to_claw_led(dev);

	if (led->effect >= CLAW_LED_EFFECT_MAX)
		return -EINVAL;

	return sysfs_emit(buf, "%s\n", led_effect_names[led->effect]);
}

static ssize_t effect_store(struct device *dev,
			    struct device_attribute *attr,
			    const char *buf, size_t count)
{
	struct claw_led *led = dev_to_claw_led(dev);
	int i, ret;
	char effect_name[16];

	if (sscanf(buf, "%15s", effect_name) != 1)
		return -EINVAL;

	hid_info(led->hdev, "LED effect_store: %s -> %s\n",
		 led_effect_names[led->effect], effect_name);

	for (i = 0; i < CLAW_LED_EFFECT_MAX; i++) {
		if (strcmp(effect_name, led_effect_names[i]) == 0) {
			led->effect = i;
			ret = claw_apply_effect(led);
			return ret ? ret : count;
		}
	}

	return -EINVAL;
}
static DEVICE_ATTR_RW(effect);

static ssize_t effect_index_show(struct device *dev,
				 struct device_attribute *attr, char *buf)
{
	int i, len = 0;

	for (i = 0; i < CLAW_LED_EFFECT_MAX; i++) {
		len += sysfs_emit_at(buf, len, "%s ", led_effect_names[i]);
	}
	if (len > 0)
		buf[len - 1] = '\n';

	return len;
}
static DEVICE_ATTR_RO(effect_index);

static ssize_t mode_show(struct device *dev,
			 struct device_attribute *attr, char *buf)
{
	struct claw_led *led = dev_to_claw_led(dev);

	if (led->mode >= CLAW_LED_MODE_MAX)
		return -EINVAL;

	return sysfs_emit(buf, "%s\n", led_mode_names[led->mode]);
}

static ssize_t mode_store(struct device *dev,
			  struct device_attribute *attr,
			  const char *buf, size_t count)
{
	struct claw_led *led = dev_to_claw_led(dev);
	int i, ret;
	char mode_name[16];

	if (sscanf(buf, "%15s", mode_name) != 1)
		return -EINVAL;

	hid_info(led->hdev, "LED mode_store: %s -> %s\n",
		 led_mode_names[led->mode], mode_name);

	for (i = 0; i < CLAW_LED_MODE_MAX; i++) {
		if (strcmp(mode_name, led_mode_names[i]) == 0) {
			led->mode = i;
			ret = claw_apply_effect(led);
			return ret ? ret : count;
		}
	}

	return -EINVAL;
}
static DEVICE_ATTR_RW(mode);

static ssize_t mode_index_show(struct device *dev,
			       struct device_attribute *attr, char *buf)
{
	int i, len = 0;

	for (i = 0; i < CLAW_LED_MODE_MAX; i++) {
		len += sysfs_emit_at(buf, len, "%s ", led_mode_names[i]);
	}
	if (len > 0)
		buf[len - 1] = '\n';

	return len;
}
static DEVICE_ATTR_RO(mode_index);

static ssize_t speed_show(struct device *dev,
			  struct device_attribute *attr, char *buf)
{
	struct claw_led *led = dev_to_claw_led(dev);

	claw_led_lazy_load(led);
	return sysfs_emit(buf, "%d\n", led->speed);
}

static ssize_t speed_store(struct device *dev,
			   struct device_attribute *attr,
			   const char *buf, size_t count)
{
	struct claw_led *led = dev_to_claw_led(dev);
	unsigned int val;
	int ret;

	ret = kstrtouint(buf, 10, &val);
	if (ret)
		return ret;

	if (val > 100)
		return -EINVAL;

	hid_dbg(led->hdev, "LED speed_store: %d -> %d\n", led->speed, val);

	led->speed = val;

	/* Apply with throttling to avoid overwhelming the device */
	claw_led_apply_throttled(led);

	return count;
}
static DEVICE_ATTR_RW(speed);

static ssize_t speed_range_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "0-100\n");
}
static DEVICE_ATTR_RO(speed_range);

static ssize_t keyframes_show(struct device *dev,
			      struct device_attribute *attr, char *buf)
{
	struct claw_led *led = dev_to_claw_led(dev);
	int frame, zone, len = 0;

	if (led->custom_frame_count == 0)
		return sysfs_emit(buf, "(not set)\n");

	for (frame = 0; frame < led->custom_frame_count; frame++) {
		if (frame > 0)
			len += sysfs_emit_at(buf, len, " ");
		for (zone = 0; zone < CLAW_LED_ZONES; zone++) {
			len += sysfs_emit_at(buf, len, "%d,%d,%d%s",
					     led->custom_frames[frame][zone][0],
					     led->custom_frames[frame][zone][1],
					     led->custom_frames[frame][zone][2],
					     (zone < CLAW_LED_ZONES - 1) ? ";" : "");
		}
	}
	len += sysfs_emit_at(buf, len, "\n");

	return len;
}

static ssize_t keyframes_store(struct device *dev,
			       struct device_attribute *attr,
			       const char *buf, size_t count)
{
	struct claw_led *led = dev_to_claw_led(dev);
	u8 frames[CLAW_LED_MAX_FRAMES][CLAW_LED_ZONES][3];
	const char *p = buf;
	int frame, zone, ret;
	unsigned int r, g, b;
	int frame_count = 0;

	/* Count frames by counting spaces + 1 */
	for (const char *s = buf; *s; s++) {
		if (*s == ' ')
			frame_count++;
	}
	frame_count++;  /* frames = spaces + 1 */

	if (frame_count < 1 || frame_count > CLAW_LED_MAX_FRAMES)
		return -EINVAL;

	/* Parse frame data: frame1 frame2 ... (space-separated) */
	/* Each frame: R,G,B;R,G,B;...;R,G,B (9 zones, semicolon-separated) */
	for (frame = 0; frame < frame_count; frame++) {
		/* Skip leading spaces */
		while (*p == ' ')
			p++;

		for (zone = 0; zone < CLAW_LED_ZONES; zone++) {
			if (sscanf(p, "%u,%u,%u", &r, &g, &b) != 3)
				return -EINVAL;

			if (r > 255 || g > 255 || b > 255)
				return -EINVAL;

			frames[frame][zone][0] = r;
			frames[frame][zone][1] = g;
			frames[frame][zone][2] = b;

			/* Move past this zone's data */
			while (*p && *p != ';' && *p != ' ' && *p != '\n')
				p++;

			if (zone < CLAW_LED_ZONES - 1) {
				/* Expect semicolon between zones */
				if (*p != ';')
					return -EINVAL;
				p++;
			}
		}

		/* Move to next frame (skip to space or end) */
		while (*p && *p != ' ' && *p != '\n')
			p++;
	}

	/* Store in cache */
	led->custom_frame_count = frame_count;
	memcpy(led->custom_frames, frames, frame_count * sizeof(frames[0]));

	/* Apply if custom mode is active */
	if (led->mode == CLAW_LED_MODE_CUSTOM && led->enabled) {
		ret = claw_apply_effect(led);
		if (ret)
			return ret;
	}

	return count;
}
static DEVICE_ATTR_RW(keyframes);

/* ========== End LED sysfs attributes ========== */

/* LED multicolor brightness callback */
static int claw_led_brightness_set_blocking(struct led_classdev *cdev,
					    enum led_brightness brightness)
{
	struct led_classdev_mc *mc = lcdev_to_mccdev(cdev);
	struct claw_led *led = container_of(&mc, struct claw_led, mc_cdev);
	struct mc_subled *color = led->mc_cdev->subled_info;

	/* Calculate RGB from multi_intensity and brightness */
	led_mc_calc_color_components(mc, brightness);

	hid_dbg(led->hdev, "LED brightness_set: %d -> %d, color=[%d,%d,%d]\n",
		led->mc_cdev->led_cdev.brightness, brightness, color[0].brightness,
		color[1].brightness, color[2].brightness);

	/* Apply immediately if enabled */
	if (led->enabled)
		return claw_apply_effect(led);

	return 0;
}

/* Determine RGB address based on firmware version */
static const uint8_t *claw_get_rgb_addr(u16 bcd_device)
{
	u8 major_ver = bcd_device >> 8;

	switch (major_ver) {
	case 1:
		return (bcd_device >= 0x0166) ? rgb_addr_new : rgb_addr_old;
	case 2:
		return (bcd_device >= 0x0217) ? rgb_addr_new : rgb_addr_old;
	case 3:
		return rgb_addr_new;
	default:
		return rgb_addr_old;
	}
}

static struct attribute *claw_rgb_attrs[] = {
	&dev_attr_enabled.attr,
	&dev_attr_enabled_index.attr,
	&dev_attr_effect.attr,
	&dev_attr_effect_index.attr,
	&dev_attr_keyframes.attr,
	&dev_attr_mode.attr,
	&dev_attr_mode_index.attr,
	&dev_attr_speed.attr,
	&dev_attr_speed_range.attr,
	NULL,
};

static struct attribute_group rgb_attr_group = {
	.attrs = claw_rgb_attrs,
};

struct mc_subled claw_rgb_subled_info[] = {
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

struct led_classdev_mc claw_cdev_rgb = {
	.led_cdev = {
		.name = "go_s:rgb:joystick_rings",
		.brightness = 0x50,
		.max_brightness = 0x64,
		.brightness_set_blocking = claw_led_brightness_set_blocking,
	},
	.num_colors = ARRAY_SIZE(claw_rgb_subled_info),
	.subled_info = claw_rgb_subled_info,
};

/* Initialize and register LED device */
static int claw_led_init(struct hid_device *hdev)
{
	struct claw_drvdata *drvdata = hid_get_drvdata(hdev);
	struct claw_led *led;
	int ret;

	led = devm_kzalloc(&hdev->dev, sizeof(*led), GFP_KERNEL);
	if (!led)
		return -ENOMEM;

	/* Set rgb_addr first so we can read config */
	drvdata->rgb_addr = claw_get_rgb_addr(drvdata->bcd_device);

	led->mc_cdev = &claw_cdev_rgb;

	led->hdev = hdev;
	led->enabled = true;
	led->mode = CLAW_LED_MODE_DYNAMIC;
	led->effect = CLAW_LED_EFFECT_MONOCOLOR;

	/* Use defaults, lazy load from device on first sysfs access */
	led->config_loaded = false;
	led->speed = 50;

	/* Initialize throttling */
	INIT_DELAYED_WORK(&led->apply_work, claw_led_apply_work_fn);
	led->last_apply_jiffies = jiffies;

	ret = devm_led_classdev_multicolor_register(&hdev->dev, led->mc_cdev);
	if (ret)
		return dev_err_probe(&hdev->dev, ret, "Failed to register LED\n");

	ret = devm_device_add_group(led->mc_cdev->led_cdev.dev, &rgb_attr_group);
	if (ret)
		return dev_err_probe(&hdev->dev, ret,
			      "Failed to create RGB configuratiion attributes\n");

	drvdata->led_cfg = led;

	hid_dbg(hdev, "hid-msi-claw: LED initialized (rgb_addr: 0x%02x%02x)\n",
		 drvdata->rgb_addr[0], drvdata->rgb_addr[1]);

	return 0;

}

/* ========== End LED registration ========== */

static bool claw_fw_remap_supported(u16 bcd_device,
	const uint8_t (**addr)[2])
{
	u8 major = bcd_device >> 8;

	if (major == 1) {
		*addr = (bcd_device >= 0x0166) ?
			m_remap_addr_new : m_remap_addr_old;
		return true;
	}

	if (major == 2 && bcd_device >= 0x0217) {
		*addr = m_remap_addr_new;
		return true;
	}

	if (major >= 3) {
		*addr = m_remap_addr_new;
		return true;
	}

	return false;
}

static int m_remap_name_to_code(const char *name)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(m_remap_key_map); i++) {
		if (!strcmp(name, m_remap_key_map[i].name))
			return m_remap_key_map[i].code;
	}

	return -EINVAL;
}

static const char *m_remap_code_to_name(uint8_t code)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(m_remap_key_map); i++) {
		if (m_remap_key_map[i].code == code)
			return m_remap_key_map[i].name;
	}

	return NULL;
}

static int claw_set_m_remap(struct hid_device *hdev,
	enum claw_m_key m_key, const uint8_t *codes)
{
	struct claw_drvdata *drvdata = hid_get_drvdata(hdev);
	const uint8_t cmd_buffer[] = {
		0x01,
		drvdata->m_remap_addr[m_key][0],
		drvdata->m_remap_addr[m_key][1],
		0x07, 0x04, 0x00,
		codes[0], codes[1], codes[2], codes[3], codes[4],
	};
	int ret;

	if (!drvdata->control) {
		hid_err(hdev, "hid-msi-claw couldn't find control interface\n");
		ret = -ENODEV;
		goto claw_set_m_remap_err;
	}

	ret = claw_write_cmd(hdev, CLAW_COMMAND_TYPE_WRITE_PROFILE_DATA,
		cmd_buffer, sizeof(cmd_buffer));
	if (ret < 0) {
		hid_err(hdev, "hid-msi-claw failed to set m_remap: %d\n", ret);
		goto claw_set_m_remap_err;
	} else if (ret != CLAW_PACKET_SIZE) {
		hid_err(hdev, "hid-msi-claw failed to write m_remap: %d\n", ret);
		ret = -EIO;
		goto claw_set_m_remap_err;
	}

	ret = claw_await_ack(hdev);
	if (ret) {
		hid_err(hdev, "hid-msi-claw failed to await ack for m_remap: %d\n", ret);
		goto claw_set_m_remap_err;
	}

	ret = sync_to_rom(hdev);
	if (ret) {
		hid_err(hdev, "hid-msi-claw failed to sync m_remap to rom: %d\n", ret);
		goto claw_set_m_remap_err;
	}

	return 0;

claw_set_m_remap_err:
	return ret;
}

static int claw_read_m_remap(struct hid_device *hdev,
	enum claw_m_key m_key, uint8_t *codes, int *count)
{
	struct claw_drvdata *drvdata = hid_get_drvdata(hdev);
	const uint8_t cmd_buffer[] = {
		0x01,
		drvdata->m_remap_addr[m_key][0],
		drvdata->m_remap_addr[m_key][1],
		0x07,
	};
	uint8_t buffer[CLAW_PACKET_SIZE] = {};
	int ret, i;

	if (!drvdata->control) {
		hid_err(hdev, "hid-msi-claw couldn't find control interface\n");
		return -ENODEV;
	}

	ret = claw_write_cmd(hdev, CLAW_COMMAND_TYPE_READ_PROFILE,
		cmd_buffer, sizeof(cmd_buffer));
	if (ret < 0) {
		hid_err(hdev, "hid-msi-claw failed to send read m_remap request: %d\n", ret);
		return ret;
	} else if (ret != CLAW_PACKET_SIZE) {
		hid_err(hdev, "hid-msi-claw couldn't send read m_remap request: %d\n", ret);
		return -EIO;
	}

	ret = claw_read(hdev, buffer, CLAW_PACKET_SIZE, 50);
	if (ret != CLAW_PACKET_SIZE) {
		hid_err(hdev, "hid-msi-claw failed to read m_remap: %d\n", ret);
		return -EINVAL;
	}

	if (buffer[4] != (uint8_t)CLAW_COMMAND_TYPE_READ_PROFILE_ACK) {
		hid_err(hdev, "hid-msi-claw expected READ_PROFILE_ACK (0x05), got 0x%02x\n", buffer[4]);
		return -EINVAL;
	}

	/* Extract key codes, filtering out 0xff (disabled/empty) */
	*count = 0;
	for (i = 0; i < CLAW_M_REMAP_MAX_KEYS; i++) {
		if (buffer[11 + i] != 0xff)
			codes[(*count)++] = buffer[11 + i];
	}

	return 0;
}

static int claw_reset_device(struct hid_device *hdev)
{
	struct claw_drvdata *drvdata = hid_get_drvdata(hdev);
	int ret;

	if (!drvdata->control) {
		hid_err(hdev, "hid-msi-claw couldn't find control interface\n");
		ret = -ENODEV;
		goto claw_reset_device_err;
	}

	ret = claw_write_cmd(hdev, CLAW_COMMAND_TYPE_RESET_DEVICE, NULL, 0);
	if (ret < 0) {
		hid_err(hdev, "hid-msi-claw failed to send reset: %d\n", ret);
		goto claw_reset_device_err;
	} else if (ret != CLAW_PACKET_SIZE) {
		hid_err(hdev, "hid-msi-claw couldn't send reset request: %d\n", ret);
		ret = -EIO;
		goto claw_reset_device_err;
	}

	ret = claw_await_ack(hdev);
	if (ret) {
		hid_err(hdev, "hid-msi-claw failed to await ack: %d\n", ret);
		goto claw_reset_device_err;
	}

claw_reset_device_err:
	return ret;
}

static int claw_read_gamepad_mode(struct hid_device *hdev,
	struct claw_control_status *status)
{
	uint8_t buffer[CLAW_PACKET_SIZE] = {};
	int ret;

	ret = claw_write_cmd(hdev, CLAW_COMMAND_TYPE_READ_GAMEPAD_MODE, NULL, 0);
	if (ret < 0) {
		hid_err(hdev, "hid-msi-claw failed to send read request for controller mode: %d\n", ret);
		goto claw_read_gamepad_mode_err;
	} else if (ret != CLAW_PACKET_SIZE) {
		hid_err(hdev, "hid-msi-claw couldn't send request: %d\n", ret);
		ret = -EIO;
		goto claw_read_gamepad_mode_err;
	}

	ret = claw_read(hdev, buffer, CLAW_PACKET_SIZE, 50);
	if (ret != CLAW_PACKET_SIZE) {
		hid_err(hdev, "hid-msi-claw failed to read: %d\n", ret);
		ret = -EINVAL;
		goto claw_read_gamepad_mode_err;
	}

	if (buffer[4] != (uint8_t)CLAW_COMMAND_TYPE_GAMEPAD_MODE_ACK) {
		hid_err(hdev, "hid-msi-claw received invalid response: expected 0x27, got 0x%02x\n", buffer[4]);
		ret = -EINVAL;
		goto claw_read_gamepad_mode_err;
	} else if (buffer[5] >= CLAW_GAMEPAD_MODE_MAX) {
		hid_err(hdev, "hid-msi-claw unknown gamepad mode: 0x%02x\n", buffer[5]);
		ret = -EINVAL;
		goto claw_read_gamepad_mode_err;
	} else if (buffer[6] >= CLAW_MKEY_FUNCTION_MAX) {
		hid_err(hdev, "hid-msi-claw unknown gamepad mode: 0x%02x\n", buffer[6]);
		ret = -EINVAL;
		goto claw_read_gamepad_mode_err;
	}

	status->gamepad_mode = (enum claw_gamepad_mode)buffer[5];
	status->mkeys_function = (enum claw_mkeys_function)buffer[6];

	ret = 0;

claw_read_gamepad_mode_err:
	return ret;
}

static int claw_switch_gamepad_mode(struct hid_device *hdev,
	const struct claw_control_status *status)
{
	struct claw_drvdata *drvdata = hid_get_drvdata(hdev);
	struct claw_control_status check_status;
	const uint8_t cmd_buffer[2] = {(uint8_t)status->gamepad_mode, (uint8_t)status->mkeys_function};
	int ret;

	if (!drvdata->control) {
		hid_err(hdev, "hid-msi-claw couldn't find control interface\n");
		ret = -ENODEV;
		goto claw_switch_gamepad_mode_err;
	}

	ret = claw_write_cmd(hdev, CLAW_COMMAND_TYPE_SWITCH_MODE, cmd_buffer, sizeof(cmd_buffer));
	if (ret < 0) {
		hid_err(hdev, "hid-msi-claw failed to send write request to switch controller mode: %d\n", ret);
		goto claw_switch_gamepad_mode_err;
	} else if (ret != CLAW_PACKET_SIZE) {
		hid_err(hdev, "hid-msi-claw failed to write: %d bytes got written\n", ret);
		ret = -EIO;
		goto claw_switch_gamepad_mode_err;
	}

	ret = claw_await_ack(hdev);
	if (ret) {
		hid_err(hdev, "hid-msi-claw failed to await ack: %d\n", ret);
		goto claw_switch_gamepad_mode_err;
	}

	// check the new mode as official application does
	ret = claw_read_gamepad_mode(hdev, &check_status);
	if (ret) {
		hid_err(hdev, "hid-msi-claw failed to read status: %d\n", ret);
		goto claw_switch_gamepad_mode_err;
	}

	if (memcmp((const void *)&check_status, (const void *)status, sizeof(*status))) {
		hid_err(hdev, "hid-msi-claw current status and target one are different\n");
		ret = -EIO;
		goto claw_switch_gamepad_mode_err;
	}

	// the device now sends back 03 00 00 00 00 00 00 00 00

	// this command is always issued by the windows counterpart after a mode switch
	ret = sync_to_rom(hdev);
	if (ret) {
		hid_err(hdev, "hid-msi-claw failed the sync to rom command: %d\n", ret);
		return ret;
	}

claw_switch_gamepad_mode_err:
	return ret;
}

static ssize_t reset_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t count)
{
	struct hid_device *hdev = to_hid_device(dev);
	int ret;

	ret = claw_reset_device(hdev);
	if (ret < 0) {
		hid_err(hdev, "hid-msi-claw error resetting device: %d\n", ret);
		goto reset_store_err;
	}

	return count;

reset_store_err:
	return ret;
}
static DEVICE_ATTR_WO(reset);

static ssize_t gamepad_mode_index_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	int i, ret = 0;
	int len = ARRAY_SIZE(gamepad_mode_map);

	for (i = 0; i < len; i++) {
		if (!gamepad_mode_map[i].available)
			continue;

		ret += sysfs_emit_at(buf, ret, "%s", gamepad_mode_map[i].name);

		if (i < len-1)
			ret += sysfs_emit_at(buf, ret, " ");
	}
	ret += sysfs_emit_at(buf, ret, "\n");

	return ret;
}
static DEVICE_ATTR_RO(gamepad_mode_index);

static ssize_t gamepad_mode_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct hid_device *hdev = to_hid_device(dev);
	struct claw_control_status status;
	int ret;

	ret = claw_read_gamepad_mode(hdev, &status);
	if (ret) {
		hid_err(hdev, "hid-msi-claw error reaging the gamepad mode: %d\n", ret);
		return ret;
	}

	return sysfs_emit(buf, "%s\n", gamepad_mode_map[(int)status.gamepad_mode].name);
}

static ssize_t gamepad_mode_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t count)
{
	ssize_t ret;
	uint8_t *input;
	struct hid_device *hdev = to_hid_device(dev);
	struct claw_drvdata *drvdata = hid_get_drvdata(hdev);
	enum claw_gamepad_mode new_gamepad_mode = ARRAY_SIZE(gamepad_mode_map);
	struct claw_control_status status = {
		.gamepad_mode = drvdata->control->gamepad_mode,
		.mkeys_function = drvdata->control->mkeys_function,
	};

	if (!count) {
		ret = -EINVAL;
		goto gamepad_mode_store_err;
	}

	input = kmemdup(buf, count+1, GFP_KERNEL);
	if (!input) {
		ret = -ENOMEM;
		goto gamepad_mode_store_err;
	}

	input[count] = '\0';
	if (input[count-1] == '\n')
		input[count-1] = '\0';

	for (size_t i = 0; i < (size_t)new_gamepad_mode; i++)
		if ((!strcmp(input, gamepad_mode_map[i].name)) && (gamepad_mode_map[i].available))
			new_gamepad_mode = (enum claw_gamepad_mode)i;

	kfree(input);

	if (new_gamepad_mode == ARRAY_SIZE(gamepad_mode_map)) {
		hid_err(hdev, "Invalid gamepad mode selected\n");
		ret = -EINVAL;
		goto gamepad_mode_store_err;
	}

	status.gamepad_mode = new_gamepad_mode;
	ret = claw_switch_gamepad_mode(hdev, &status);
	if (ret) {
		hid_err(hdev, "Error changing gamepad mode: %d\n", (int)ret);
		goto gamepad_mode_store_err;
	}

	ret = count;

gamepad_mode_store_err:
	return ret;
}
static DEVICE_ATTR_RW(gamepad_mode);

static ssize_t mkeys_function_index_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	int i, ret = 0;
	int len = ARRAY_SIZE(mkeys_function_map);

	for (i = 0; i < len; i++) {
		ret += sysfs_emit_at(buf, ret, "%s", mkeys_function_map[i]);

		if (i < len-1)
			ret += sysfs_emit_at(buf, ret, " ");
	}
	ret += sysfs_emit_at(buf, ret, "\n");

	return ret;
}
static DEVICE_ATTR_RO(mkeys_function_index);

static ssize_t mkeys_function_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct hid_device *hdev = to_hid_device(dev);
	struct claw_control_status status;
	int ret = claw_read_gamepad_mode(hdev, &status);

	if (ret) {
		hid_err(hdev, "hid-msi-claw error reaging the gamepad mode: %d\n", ret);
		return ret;
	}

	return sysfs_emit(buf, "%s\n", mkeys_function_map[(int)status.mkeys_function]);
}

static ssize_t mkeys_function_store(struct device *dev, struct device_attribute *attr,
					const char *buf, size_t count)
{
	uint8_t *input;
	ssize_t err;
	struct hid_device *hdev = to_hid_device(dev);
	struct claw_drvdata *drvdata = hid_get_drvdata(hdev);
	enum claw_mkeys_function new_mkeys_function = ARRAY_SIZE(mkeys_function_map);
	struct claw_control_status status = {
		.gamepad_mode = drvdata->control->gamepad_mode,
		.mkeys_function = drvdata->control->mkeys_function,
	};

	if (!count)
		return -EINVAL;

	input = kmemdup(buf, count+1, GFP_KERNEL);
	if (!input)
		return -ENOMEM;

	input[count] = '\0';
	if (input[count-1] == '\n')
		input[count-1] = '\0';

	for (size_t i = 0; i < (size_t)new_mkeys_function; i++)
		if (!strcmp(input, mkeys_function_map[i]))
			new_mkeys_function = i;

	kfree(input);

	if (new_mkeys_function == ARRAY_SIZE(mkeys_function_map)) {
		hid_err(hdev, "Invalid mkeys function selected\n");
		return -EINVAL;
	}

	status.mkeys_function = new_mkeys_function;
	err = claw_switch_gamepad_mode(hdev, &status);
	if (err) {
		hid_err(hdev, "Error changing mkeys function: %d\n", (int)err);
		return err;
	}

	return count;
}
static DEVICE_ATTR_RW(mkeys_function);

static ssize_t m_remap_index_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	int i, ret = 0;
	int len = ARRAY_SIZE(m_remap_key_map);

	for (i = 0; i < len; i++) {
		ret += sysfs_emit_at(buf, ret, "%s", m_remap_key_map[i].name);

		if (i < len - 1)
			ret += sysfs_emit_at(buf, ret, " ");
	}
	ret += sysfs_emit_at(buf, ret, "\n");

	return ret;
}
static DEVICE_ATTR_RO(m_remap_index);

static ssize_t m1_remap_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	struct hid_device *hdev = to_hid_device(dev);
	uint8_t codes[CLAW_M_REMAP_MAX_KEYS];
	const char *name;
	int key_count, ret, i;
	ssize_t len = 0;

	ret = claw_read_m_remap(hdev, CLAW_M1_KEY, codes, &key_count);
	if (ret)
		return ret;

	if (key_count == 0)
		return sysfs_emit(buf, "disabled\n");

	for (i = 0; i < key_count; i++) {
		name = m_remap_code_to_name(codes[i]);
		if (name)
			len += sysfs_emit_at(buf, len, "%s", name);
		else
			len += sysfs_emit_at(buf, len, "0x%02x", codes[i]);

		if (i < key_count - 1)
			len += sysfs_emit_at(buf, len, " ");
	}
	len += sysfs_emit_at(buf, len, "\n");

	return len;
}

static ssize_t m1_remap_store(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t count)
{
	struct hid_device *hdev = to_hid_device(dev);
	uint8_t codes[CLAW_M_REMAP_MAX_KEYS];
	char *input, *token, *cur;
	int code, ret, key_count = 0, i;

	if (!count)
		return -EINVAL;

	input = kmemdup(buf, count + 1, GFP_KERNEL);
	if (!input)
		return -ENOMEM;

	input[count] = '\0';
	if (input[count - 1] == '\n')
		input[count - 1] = '\0';

	/* Parse space-separated key names */
	cur = input;
	while ((token = strsep(&cur, " ")) != NULL) {
		if (*token == '\0')
			continue;

		if (key_count >= CLAW_M_REMAP_MAX_KEYS) {
			hid_err(hdev, "hid-msi-claw too many keys (max %d)\n",
				CLAW_M_REMAP_MAX_KEYS);
			kfree(input);
			return -EINVAL;
		}

		code = m_remap_name_to_code(token);
		if (code < 0) {
			hid_err(hdev, "hid-msi-claw invalid key: %s\n", token);
			kfree(input);
			return -EINVAL;
		}

		codes[key_count++] = (uint8_t)code;
	}
	kfree(input);

	if (key_count == 0)
		return -EINVAL;

	/* Fill remaining slots with 0xff (disabled) */
	for (i = key_count; i < CLAW_M_REMAP_MAX_KEYS; i++)
		codes[i] = 0xff;

	ret = claw_set_m_remap(hdev, CLAW_M1_KEY, codes);
	if (ret)
		return ret;

	return count;
}
static DEVICE_ATTR_RW(m1_remap);

static ssize_t m2_remap_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	struct hid_device *hdev = to_hid_device(dev);
	uint8_t codes[CLAW_M_REMAP_MAX_KEYS];
	const char *name;
	int key_count, ret, i;
	ssize_t len = 0;

	ret = claw_read_m_remap(hdev, CLAW_M2_KEY, codes, &key_count);
	if (ret)
		return ret;

	if (key_count == 0)
		return sysfs_emit(buf, "disabled\n");

	for (i = 0; i < key_count; i++) {
		name = m_remap_code_to_name(codes[i]);
		if (name)
			len += sysfs_emit_at(buf, len, "%s", name);
		else
			len += sysfs_emit_at(buf, len, "0x%02x", codes[i]);

		if (i < key_count - 1)
			len += sysfs_emit_at(buf, len, " ");
	}
	len += sysfs_emit_at(buf, len, "\n");

	return len;
}

static ssize_t m2_remap_store(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t count)
{
	struct hid_device *hdev = to_hid_device(dev);
	uint8_t codes[CLAW_M_REMAP_MAX_KEYS];
	char *input, *token, *cur;
	int code, ret, key_count = 0, i;

	if (!count)
		return -EINVAL;

	input = kmemdup(buf, count + 1, GFP_KERNEL);
	if (!input)
		return -ENOMEM;

	input[count] = '\0';
	if (input[count - 1] == '\n')
		input[count - 1] = '\0';

	/* Parse space-separated key names */
	cur = input;
	while ((token = strsep(&cur, " ")) != NULL) {
		if (*token == '\0')
			continue;

		if (key_count >= CLAW_M_REMAP_MAX_KEYS) {
			hid_err(hdev, "hid-msi-claw too many keys (max %d)\n",
				CLAW_M_REMAP_MAX_KEYS);
			kfree(input);
			return -EINVAL;
		}

		code = m_remap_name_to_code(token);
		if (code < 0) {
			hid_err(hdev, "hid-msi-claw invalid key: %s\n", token);
			kfree(input);
			return -EINVAL;
		}

		codes[key_count++] = (uint8_t)code;
	}
	kfree(input);

	if (key_count == 0)
		return -EINVAL;

	/* Fill remaining slots with 0xff (disabled) */
	for (i = key_count; i < CLAW_M_REMAP_MAX_KEYS; i++)
		codes[i] = 0xff;

	ret = claw_set_m_remap(hdev, CLAW_M2_KEY, codes);
	if (ret)
		return ret;

	return count;
}
static DEVICE_ATTR_RW(m2_remap);


static struct attribute *claw_gamepad_attrs[] = {
	&dev_attr_gamepad_mode.attr,
	&dev_attr_gamepad_mode_index.attr,
	&dev_attr_m1_remap.attr,
	&dev_attr_m2_remap.attr,
	&dev_attr_m_remap_index.attr,
	&dev_attr_mkeys_function.attr,
	&dev_attr_mkeys_function_index.attr,
	&dev_attr_reset.attr,
	NULL,
};

static const struct attribute_group claw_gamepad_attr_group = {
	.name = "touchpad",
	.attrs = claw_gamepad_attrs,
};


static int __maybe_unused claw_resume(struct hid_device *hdev)
{
	int ret;
	struct claw_drvdata *drvdata = hid_get_drvdata(hdev);
	struct claw_control_status status = {
		.gamepad_mode = drvdata->control->gamepad_mode,
		.mkeys_function = drvdata->control->mkeys_function,
	};

	// TODO: clear out events list here (or in suspend?)

	// wait for device to be ready
	msleep(500);

	ret = claw_switch_gamepad_mode(hdev, &status);
	if (ret) {
		hid_err(hdev, "Error changing gamepad mode: %d\n", (int)ret);
		goto claw_resume_err;
	}

	// TODO: retry until this works?

	return 0;

claw_resume_err:
	return ret;
}

static int claw_probe(struct hid_device *hdev, const struct hid_device_id *id)
{
	int ret;
	struct claw_drvdata *drvdata;

	if (!hid_is_usb(hdev)) {
		ret = -ENODEV;
		goto err_probe;
	}

	drvdata = devm_kzalloc(&hdev->dev, sizeof(*drvdata), GFP_KERNEL);
	if (drvdata == NULL) {
		ret = -ENOMEM;
		goto err_probe;
	}

	mutex_init(&drvdata->read_data_mutex);
	drvdata->read_data = NULL;
	drvdata->control = NULL;

	hid_set_drvdata(hdev, drvdata);

	ret = hid_parse(hdev);
	if (ret)
		goto err_probe;

	/*
	 * For non-control interfaces (keyboard/mouse), use HID_CONNECT_DEFAULT
	 * with INPUT_PER_APP quirk to create separate keyboard/mouse devices.
	 * This matches hid-generic behavior and allows userspace to grab the devices.
	 */
	if (hdev->rdesc[0] != CLAW_DEVICE_CONTROL_DESC) {
		hid_dbg(hdev, "non-control interface (0x%02x), using HID_CONNECT_DEFAULT\n",
			hdev->rdesc[0]);
		/* Set quirk to create separate input devices per HID application */
		hdev->quirks |= HID_QUIRK_INPUT_PER_APP;
		ret = hid_hw_start(hdev, HID_CONNECT_DEFAULT);
		if (ret)
			goto err_probe;

		/* Don't call hid_hw_open() - let userspace grab the device */
		return 0;
	}

	/*
	 * For control interface: use HID_CONNECT_HIDRAW only (no input devices)
	 * and open the HID transport for sending commands.
	 */
	ret = hid_hw_start(hdev, HID_CONNECT_HIDRAW | HID_CONNECT_DRIVER);
	if (ret)
		goto err_probe;

	ret = hid_hw_open(hdev);
	if (ret)
		goto err_stop_hw;

	/* Get firmware version for m_remap support */
	{
		struct usb_interface *intf = to_usb_interface(hdev->dev.parent);
		struct usb_device *udev = interface_to_usbdev(intf);
		drvdata->bcd_device = le16_to_cpu(udev->descriptor.bcdDevice);
	}

	//TODO: Add is_visible instead
	//if (!drvdata->m_remap_supported)
	//	hid_warn(hdev, "hid-msi-claw firmware 0x%04x not supported for m_remap\n",
	//		drvdata->bcd_device);

	if (hdev->rdesc[0] != CLAW_DEVICE_CONTROL_DESC)
		goto skip_attrs;

	drvdata->m_remap_supported = claw_fw_remap_supported(drvdata->bcd_device,
							     &drvdata->m_remap_addr);

	drvdata->control = devm_kzalloc(&hdev->dev, sizeof(*(drvdata->control)), GFP_KERNEL);
	if (drvdata->control == NULL) {
		ret = -ENOMEM;
		goto err_close;
	}

	drvdata->control->gamepad_mode = CLAW_GAMEPAD_MODE_XINPUT;
	drvdata->control->mkeys_function = CLAW_MKEY_FUNCTION_MACRO;

	ret = sysfs_create_group(&hdev->dev.kobj, &claw_gamepad_attr_group);
	if (ret)
		goto err_close;

	/* Initialize RGB LED */
	ret = claw_led_init(hdev);
	if (ret) {
		hid_warn(hdev, "hid-msi-claw: LED init failed: %d (continuing)\n", ret);
		/* LED failure is not fatal, continue */
	}

skip_attrs:
	return 0;

err_close:
	hid_hw_close(hdev);
err_stop_hw:
	hid_hw_stop(hdev);
err_probe:
	return dev_err_probe(&hdev->dev, ret, "Failed to init configuration device\n");
}

static void claw_remove(struct hid_device *hdev)
{
	struct claw_drvdata *drvdata = hid_get_drvdata(hdev);

	if (drvdata->control) {
		cancel_delayed_work_sync(&drvdata->led_cfg->apply_work);
		sysfs_remove_group(&hdev->dev.kobj, &claw_gamepad_attr_group);

		/* Only close if we opened (control interface only) */
		hid_hw_close(hdev);
	}

	hid_hw_stop(hdev);
}

static const struct hid_device_id claw_devices[] = {
	{ HID_USB_DEVICE(USB_VENDOR_ID_MSI_NEW, USB_DEVICE_ID_MSI_CLAW_XINPUT) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_MSI_NEW, USB_DEVICE_ID_MSI_CLAW_DINPUT) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_MSI_NEW, USB_DEVICE_ID_MSI_CLAW_DESKTOP) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_MSI_NEW, USB_DEVICE_ID_MSI_CLAW_BIOS) },
	{ }
};
MODULE_DEVICE_TABLE(hid, claw_devices);

static struct hid_driver claw_driver = {
	.name		= "hid-msi-claw",
	.id_table	= claw_devices,
	.raw_event	= claw_raw_event,
	.probe		= claw_probe,
	.remove		= claw_remove,
 #ifdef CONFIG_PM
	.resume		= claw_resume,
 #endif
};
module_hid_driver(claw_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Denis Benato <benato.denis96@gmail.com>");
MODULE_AUTHOR("Deerek J. Clark <derekjohn.clark@gmail.com>");
MODULE_DESCRIPTION("Manage MSI Claw gamepad device");
