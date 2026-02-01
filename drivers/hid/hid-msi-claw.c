#include <linux/dmi.h>
#include <linux/hid.h>
#include <linux/module.h>
#include <linux/usb.h>
#include <linux/mutex.h>
#include <linux/leds.h>
#include <linux/led-class-multicolor.h>

#include "hid-ids.h"

#define MSI_CLAW_FEATURE_GAMEPAD_REPORT_ID 0x0f

#define MSI_CLAW_READ_SIZE 64
#define MSI_CLAW_WRITE_SIZE 64

#define MSI_CLAW_GAME_CONTROL_DESC   0x05
#define MSI_CLAW_DEVICE_CONTROL_DESC 0x06

/* LED constants */
#define MSI_CLAW_LED_ZONES 9
#define MSI_CLAW_LED_MAX_FRAMES 8
#define MSI_CLAW_LED_NAME "msi_claw:rgb:joystick_rings"

enum msi_claw_led_effect {
	MSI_CLAW_LED_EFFECT_MONOCOLOR = 0,
	MSI_CLAW_LED_EFFECT_BREATHE,
	MSI_CLAW_LED_EFFECT_CHROMA,
	MSI_CLAW_LED_EFFECT_RAINBOW,
	MSI_CLAW_LED_EFFECT_FROSTFIRE,

	MSI_CLAW_LED_EFFECT_MAX,
};

static const char * const led_effect_names[] = {
	[MSI_CLAW_LED_EFFECT_MONOCOLOR] = "monocolor",
	[MSI_CLAW_LED_EFFECT_BREATHE] = "breathe",
	[MSI_CLAW_LED_EFFECT_CHROMA] = "chroma",
	[MSI_CLAW_LED_EFFECT_RAINBOW] = "rainbow",
	[MSI_CLAW_LED_EFFECT_FROSTFIRE] = "frostfire",
};

enum msi_claw_led_mode {
	MSI_CLAW_LED_MODE_DYNAMIC = 0,
	MSI_CLAW_LED_MODE_CUSTOM,

	MSI_CLAW_LED_MODE_MAX,
};

static const char * const led_mode_names[] = {
	[MSI_CLAW_LED_MODE_DYNAMIC] = "dynamic",
	[MSI_CLAW_LED_MODE_CUSTOM] = "custom",
};

struct msi_claw_rgb_frame {
	u8 zones[MSI_CLAW_LED_ZONES][3];  /* 9 zones * RGB */
};

struct msi_claw_rgb_config {
	u8 frame_count;   /* 1-8 */
	u8 speed;         /* 0-20 (0=fastest) */
	u8 brightness;    /* 0-100 */
	struct msi_claw_rgb_frame frames[MSI_CLAW_LED_MAX_FRAMES];
};

/* Throttle interval for LED updates (in ms) */
#define MSI_CLAW_LED_THROTTLE_MS 50

struct msi_claw_led {
	struct led_classdev_mc mc_cdev;
	struct mc_subled subled_info[3];
	struct hid_device *hdev;

	/* State */
	bool enabled;
	bool config_loaded;  /* true if speed/brightness read from device */
	enum msi_claw_led_mode mode;      /* dynamic or custom */
	enum msi_claw_led_effect effect;  /* preset effect (when mode=dynamic) */
	u8 speed;          /* 0-100 (user value, mapped to 0-20 for device) */
	u8 brightness;     /* 0-100 */

	/* Monocolor effect color (from multi_intensity) */
	u8 color[3];

	/* Custom effect keyframes cache */
	u8 custom_frame_count;
	u8 custom_frames[MSI_CLAW_LED_MAX_FRAMES][MSI_CLAW_LED_ZONES][3];

	/* Throttling for sysfs writes */
	struct delayed_work apply_work;
	unsigned long last_apply_jiffies;
};

enum msi_claw_gamepad_mode {
	MSI_CLAW_GAMEPAD_MODE_OFFLINE = 0x00,
	MSI_CLAW_GAMEPAD_MODE_XINPUT = 0x01,
	MSI_CLAW_GAMEPAD_MODE_DINPUT = 0x02,
	MSI_CLAW_GAMEPAD_MODE_MSI = 0x03,
	MSI_CLAW_GAMEPAD_MODE_DESKTOP = 0x04,
	MSI_CLAW_GAMEPAD_MODE_BIOS = 0x05,
	MSI_CLAW_GAMEPAD_MODE_TESTING = 0x06,

	MSI_CLAW_GAMEPAD_MODE_MAX,
};

enum msi_claw_mkeys_function {
	MSI_CLAW_MKEY_FUNCTION_MACRO = 0x00,
	MSI_CLAW_MKEY_FUNCTION_COMBINATION = 0x01,
	MSI_CLAW_MKEY_FUNCTION_DISABLED = 0x02,

	MSI_CLAW_MKEY_FUNCTION_MAX,
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

#define MSI_CLAW_M_REMAP_MAX_KEYS 5

enum msi_claw_m_key {
	MSI_CLAW_M1_KEY = 0,
	MSI_CLAW_M2_KEY = 1,

	MSI_CLAW_M_KEY_MAX,
};

static const struct {
	const char *name;
	uint8_t code;
} m_remap_key_map[] = {
	/* Gamepad buttons */
	{"BTN_DPAD_UP", 0x01},
	{"BTN_DPAD_DOWN", 0x02},
	{"BTN_DPAD_LEFT", 0x03},
	{"BTN_DPAD_RIGHT", 0x04},
	{"BTN_TL", 0x05},
	{"BTN_TR", 0x06},
	{"BTN_THUMBL", 0x07},
	{"BTN_THUMBR", 0x08},
	{"BTN_SOUTH", 0x09},
	{"BTN_EAST", 0x0a},
	{"BTN_NORTH", 0x0b},
	{"BTN_WEST", 0x0c},
	{"BTN_MODE", 0x0d},
	{"BTN_SELECT", 0x0e},
	{"BTN_START", 0x0f},
	/* Keyboard keys */
	{"KEY_ESC", 0x32},
	{"KEY_F1", 0x33},
	{"KEY_F2", 0x34},
	{"KEY_F3", 0x35},
	{"KEY_F4", 0x36},
	{"KEY_F5", 0x37},
	{"KEY_F6", 0x38},
	{"KEY_F7", 0x39},
	{"KEY_F8", 0x3a},
	{"KEY_F9", 0x3b},
	{"KEY_F10", 0x3c},
	{"KEY_F11", 0x3d},
	{"KEY_F12", 0x3e},
	{"KEY_GRAVE", 0x3f},
	{"KEY_1", 0x40},
	{"KEY_2", 0x41},
	{"KEY_3", 0x42},
	{"KEY_4", 0x43},
	{"KEY_5", 0x44},
	{"KEY_6", 0x45},
	{"KEY_7", 0x46},
	{"KEY_8", 0x47},
	{"KEY_9", 0x48},
	{"KEY_0", 0x49},
	{"KEY_MINUS", 0x4a},
	{"KEY_EQUAL", 0x4b},
	{"KEY_BACKSPACE", 0x4c},
	{"KEY_TAB", 0x4d},
	{"KEY_Q", 0x4e},
	{"KEY_W", 0x4f},
	{"KEY_E", 0x50},
	{"KEY_R", 0x51},
	{"KEY_T", 0x52},
	{"KEY_Y", 0x53},
	{"KEY_U", 0x54},
	{"KEY_I", 0x55},
	{"KEY_O", 0x56},
	{"KEY_P", 0x57},
	{"KEY_LEFTBRACE", 0x58},
	{"KEY_RIGHTBRACE", 0x59},
	{"KEY_BACKSLASH", 0x5a},
	{"KEY_CAPSLOCK", 0x5b},
	{"KEY_A", 0x5c},
	{"KEY_S", 0x5d},
	{"KEY_D", 0x5e},
	{"KEY_F", 0x5f},
	{"KEY_G", 0x60},
	{"KEY_H", 0x61},
	{"KEY_J", 0x62},
	{"KEY_K", 0x63},
	{"KEY_L", 0x64},
	{"KEY_SEMICOLON", 0x65},
	{"KEY_LEFTSHIFT", 0x66},
	{"KEY_APOSTROPHE", 0x67},
	{"KEY_ENTER", 0x68},
	{"KEY_Z", 0x69},
	{"KEY_X", 0x6a},
	{"KEY_C", 0x6b},
	{"KEY_V", 0x6c},
	{"KEY_B", 0x6d},
	{"KEY_N", 0x6e},
	{"KEY_M", 0x6f},
	{"KEY_LEFTCTRL", 0x70},
	{"KEY_RIGHTSHIFT", 0x71},
	{"KEY_COMMA", 0x72},
	{"KEY_DOT", 0x73},
	{"KEY_SLASH", 0x74},
	{"KEY_LEFTALT", 0x75},
	{"KEY_LEFTMETA", 0x76},
	{"KEY_RIGHTCTRL", 0x77},
	{"KEY_RIGHTALT", 0x78},
	{"KEY_SPACE", 0x79},
	{"KEY_INSERT", 0x7a},
	{"KEY_HOME", 0x7b},
	{"KEY_PAGEUP", 0x7c},
	{"KEY_DELETE", 0x7d},
	{"KEY_END", 0x7e},
	{"KEY_PAGEDOWN", 0x7f},
	{"KEY_KPENTER", 0x8a},
	{"KEY_KP0", 0x8b},
	{"KEY_KP1", 0x8c},
	{"KEY_KP2", 0x8d},
	{"KEY_KP3", 0x8e},
	{"KEY_KP4", 0x8f},
	{"KEY_KP5", 0x90},
	{"KEY_KP6", 0x91},
	{"KEY_KP7", 0x92},
	{"KEY_KP8", 0x93},
	{"KEY_KP9", 0x94},
	/* Disabled */
	{"disabled", 0xff},
};

static const uint8_t m_remap_addr_old[MSI_CLAW_M_KEY_MAX][2] = {
	{0x00, 0x7a},  /* M1 */
	{0x01, 0x1f},  /* M2 */
};

static const uint8_t m_remap_addr_new[MSI_CLAW_M_KEY_MAX][2] = {
	{0x00, 0xbb},  /* M1 */
	{0x01, 0x64},  /* M2 */
};

/* RGB LED addresses (firmware version dependent) */
static const uint8_t rgb_addr_old[2] = {0x01, 0xfa};
static const uint8_t rgb_addr_new[2] = {0x02, 0x4a};

enum msi_claw_command_type {
	MSI_CLAW_COMMAND_TYPE_ENTER_PROFILE_CONFIG = 0x01,
	MSI_CLAW_COMMAND_TYPE_EXIT_PROFILE_CONFIG = 0x02,
	MSI_CLAW_COMMAND_TYPE_WRITE_PROFILE = 0x03,
	MSI_CLAW_COMMAND_TYPE_READ_PROFILE = 0x04,
	MSI_CLAW_COMMAND_TYPE_READ_PROFILE_ACK = 0x05,
	// ACK is read after a WRITE_PROFILE_DATA
	MSI_CLAW_COMMAND_TYPE_ACK = 0x06,
	MSI_CLAW_COMMAND_TYPE_SWITCH_PROFILE = 0x07,
	MSI_CLAW_COMMAND_TYPE_WRITE_PROFILE_TO_EEPROM = 0x08,
	MSI_CLAW_COMMAND_TYPE_SYNC_RGB = 0x09,
	MSI_CLAW_COMMAND_TYPE_READ_RGB_STATUS_ACK = 0x0a,
	MSI_CLAW_COMMAND_TYPE_READ_CURRENT_PROFILE = 0x0b,
	MSI_CLAW_COMMAND_TYPE_READ_CURRENT_PROFILE_ACK = 0x0c,
	MSI_CLAW_COMMAND_TYPE_READ_RGB_STATUS = 0x0d,
	// Write profile data (M1/M2 remap, RGB settings, etc.)
	MSI_CLAW_COMMAND_TYPE_WRITE_PROFILE_DATA = 0x21,
	MSI_CLAW_COMMAND_TYPE_SYNC_TO_ROM = 0x22,
	MSI_CLAW_COMMAND_TYPE_RESTORE_FROM_ROM = 0x23,
	MSI_CLAW_COMMAND_TYPE_SWITCH_MODE = 0x24,
	MSI_CLAW_COMMAND_TYPE_READ_GAMEPAD_MODE = 0x26,
	MSI_CLAW_COMMAND_TYPE_GAMEPAD_MODE_ACK = 0x27,
	MSI_CLAW_COMMAND_TYPE_RESET_DEVICE = 0x28,
	MSI_CLAW_COMMAND_TYPE_RGB_CONTROL = 0xe0,
	MSI_CLAW_COMMAND_TYPE_CALIBRATION_CONTROL = 0xfd,
	MSI_CLAW_COMMAND_TYPE_CALIBRATION_ACK = 0xfe,
};

struct msi_claw_control_status {
	enum msi_claw_gamepad_mode gamepad_mode;
	enum msi_claw_mkeys_function mkeys_function;
};

struct msi_claw_read_data {
	const uint8_t *data;
	int size;

	struct msi_claw_read_data *next;
};

struct msi_claw_drvdata {
	struct hid_device *hdev;

	//struct input_dev *input;

	struct msi_claw_control_status *control;

	struct mutex read_data_mutex;
	struct msi_claw_read_data *read_data;

	/* M key remap support */
	u16 bcd_device;
	bool m_remap_supported;
	const uint8_t (*m_remap_addr)[2];

	/* RGB LED support */
	struct msi_claw_led *led;
	const uint8_t *rgb_addr;
};

static void msi_claw_flush_queue(struct hid_device *hdev);

static int msi_claw_write_cmd(struct hid_device *hdev, enum msi_claw_command_type cmdtype,
    const uint8_t *const buffer, size_t buffer_len)
{
	int ret;
	uint8_t *dmabuf = NULL;
	struct msi_claw_drvdata *drvdata = hid_get_drvdata(hdev);
	const uint8_t buf[MSI_CLAW_WRITE_SIZE] = {
		MSI_CLAW_FEATURE_GAMEPAD_REPORT_ID, 0, 0, 0x3c, cmdtype };

	msi_claw_flush_queue(hdev);

	if (!drvdata->control) {
		hid_err(hdev, "hid-msi-claw couldn't find control interface\n");
		ret = -ENODEV;
		goto msi_claw_write_cmd_err;
	}

	if (buffer != NULL)
		memcpy((void *)&buf[5], buffer, buffer_len);
	else
		buffer_len = 0;

	memset((void *)&buf[5 + buffer_len], 0, MSI_CLAW_WRITE_SIZE - (5 + buffer_len));
	dmabuf = kmemdup(buf, MSI_CLAW_WRITE_SIZE, GFP_KERNEL);
	if (!dmabuf) {
		ret = -ENOMEM;
		hid_err(hdev, "hid-msi-claw failed to alloc dma buf: %d\n", ret);
		goto msi_claw_write_cmd_err;
	}

	ret = hid_hw_output_report(hdev, dmabuf, MSI_CLAW_WRITE_SIZE);
	if (ret != MSI_CLAW_WRITE_SIZE) {
		hid_err(hdev, "hid-msi-claw failed to switch controller mode: %d\n", ret);
		goto msi_claw_write_cmd_err;
	}

	hid_notice(hdev, "hid-msi-claw sent %d bytes, cmd: 0x%02x\n", ret, dmabuf[4]);

msi_claw_write_cmd_err:
	kfree(dmabuf);

	return ret;
}

static int msi_claw_read(struct hid_device *hdev, uint8_t *const buffer, int size, uint32_t timeout)
{
	struct msi_claw_drvdata *drvdata = hid_get_drvdata(hdev);
	struct msi_claw_read_data *event = NULL;
	int ret = 0;

	if (!drvdata->control) {
		hid_err(hdev, "hid-msi-claw couldn't find control interface\n");
		ret = -ENODEV;
		goto msi_claw_read_err;
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
		goto msi_claw_read_err;
	}

	if (size < event->size) {
		ret = -EINVAL;
		hid_err(hdev, "hid-msi-claw invalid buffer size: too short\n");
		goto msi_claw_read_err;
	}

	memcpy((void *)buffer, (const void *)event->data, event->size);
	ret = event->size;

msi_claw_read_err:
	if (event != NULL) {
		kfree(event->data);
		kfree(event);
	}

	return ret;
}

static void msi_claw_flush_queue(struct hid_device *hdev)
{
	struct msi_claw_drvdata *drvdata = hid_get_drvdata(hdev);
	struct msi_claw_read_data *event, *next;

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

static int msi_claw_raw_event_control(struct hid_device *hdev, struct msi_claw_drvdata *drvdata,
	struct hid_report *report, uint8_t *data, int size)
{
	struct msi_claw_read_data **list = NULL;
	struct msi_claw_read_data *node = NULL;
	uint8_t *buffer;
	int ret, i;

	if (size != MSI_CLAW_READ_SIZE) {
		//hid_err(hdev, "hid-msi-claw got unknown %d bytes\n", size);
		goto msi_claw_raw_event_control_err;
	} else if (data[0] != 0x10) {
		hid_err(hdev, "hid-msi-claw unrecognised byte at offset 0: expected 0x10, got 0x%02x\n", data[0]);
		goto msi_claw_raw_event_control_err;
	} else if (data[1] != 0x00) {
		hid_err(hdev, "hid-msi-claw unrecognised byte at offset 1: expected 0x00, got 0x%02x\n", data[1]);
		goto msi_claw_raw_event_control_err;
	} else if (data[2] != 0x00) {
		hid_err(hdev, "hid-msi-claw unrecognised byte at offset 2: expected 0x00, got 0x%02x\n", data[2]);
		goto msi_claw_raw_event_control_err;
	} else if (data[3] != 0x3c) {
		hid_err(hdev, "hid-msi-claw unrecognised byte at offset 3: expected 0x3c, got 0x%02x\n", data[3]);
		goto msi_claw_raw_event_control_err;
	}

	buffer = kmemdup(data, size, GFP_KERNEL);
	if (buffer == NULL) {
		ret = -ENOMEM;
		hid_err(hdev, "hid-msi-claw failed to alloc %d bytes for read buffer: %d\n", size, ret);
		goto msi_claw_raw_event_control_err;
	}

	struct msi_claw_read_data evt = {
		.data = buffer,
		.size = size,
		.next = NULL,
	};

	node = kmemdup(&evt, sizeof(evt), GFP_KERNEL);
	if (!node) {
		ret = -ENOMEM;
		kfree(buffer);
		hid_err(hdev, "hid-msi-claw failed to alloc event node: %d\n", ret);
		goto msi_claw_raw_event_control_err;
	}

	scoped_guard(mutex, &drvdata->read_data_mutex) {
		list = &drvdata->read_data;
		for (i = 0; (i < 32) && (*list != NULL); i++)
			list = &(*list)->next;

		if (*list != NULL) {
			ret = -EIO;
			hid_err(hdev, "too many unparsed events: ignoring\n");
			goto msi_claw_raw_event_control_err;
		}

		*list = node;
	}

	hid_notice(hdev, "hid-msi-claw received %d bytes, cmd: 0x%02x\n", size, buffer[4]);

	return 0;

msi_claw_raw_event_control_err:
	if (buffer != NULL)
		kfree(buffer);

	if (node != NULL)
		kfree(node);

	return ret;
}

static int msi_claw_raw_event(struct hid_device *hdev, struct hid_report *report, uint8_t *data, int size)
{
	struct msi_claw_drvdata *drvdata = hid_get_drvdata(hdev);

	if (!drvdata->control) {
		hid_notice(hdev, "hid-msi-claw event not from control interface: ignoring\n");
		return 0;
	}

	return msi_claw_raw_event_control(hdev, drvdata, report, data, size);
}

static int msi_claw_await_ack(struct hid_device *hdev)
{
	struct msi_claw_drvdata *drvdata = hid_get_drvdata(hdev);
	uint8_t buffer[MSI_CLAW_READ_SIZE];
	int ret;

	if (!drvdata->control) {
		hid_err(hdev, "hid-msi-claw couldn't find control interface\n");
		return -ENODEV;
	}

	ret = msi_claw_read(hdev, buffer, MSI_CLAW_READ_SIZE, 1000);
	if (ret < 0) {
		hid_err(hdev, "hid-msi-claw failed to read ack: %d\n", ret);
		return ret;
	} else if (ret != MSI_CLAW_READ_SIZE) {
		hid_err(hdev, "hid-msi-claw invalid read: expected %d bytes, got %d\n", MSI_CLAW_READ_SIZE, ret);
		return -EINVAL;
	}

	if (buffer[4] != (uint8_t)MSI_CLAW_COMMAND_TYPE_ACK) {
		hid_err(hdev, "hid-msi-claw expected ACK (0x06), got 0x%02x\n", buffer[4]);
		return -EINVAL;
	}

	return 0;
}

static int sync_to_rom(struct hid_device *hdev)
{
	struct msi_claw_drvdata *drvdata = hid_get_drvdata(hdev);
	int ret;

	if (!drvdata->control) {
		hid_err(hdev, "hid-msi-claw couldn't find control interface\n");
		ret = -ENODEV;
		goto sync_to_rom_err;
	}

	ret = msi_claw_write_cmd(hdev, MSI_CLAW_COMMAND_TYPE_SYNC_TO_ROM, NULL, 0);
	if (ret < 0) {
		hid_err(hdev, "hid-msi-claw failed to send write request for switch controller mode: %d\n", ret);
		goto sync_to_rom_err;
	} else if (ret != MSI_CLAW_WRITE_SIZE) {
		hid_err(hdev, "hid-msi-claw failed to send the sync to rom command: %d\n", ret);
		ret = -EIO;
		goto sync_to_rom_err;
	}

	ret = msi_claw_await_ack(hdev);
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
static void msi_claw_hsv_to_rgb(u16 h, u8 s, u8 v, u8 *r, u8 *g, u8 *b)
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
static void msi_claw_frame_fill_solid(struct msi_claw_rgb_frame *frame,
				       u8 r, u8 g, u8 b)
{
	int i;

	for (i = 0; i < MSI_CLAW_LED_ZONES; i++) {
		frame->zones[i][0] = r;
		frame->zones[i][1] = g;
		frame->zones[i][2] = b;
	}
}

/* Send RGB configuration to device */
static int msi_claw_send_rgb_config(struct hid_device *hdev,
				     struct msi_claw_rgb_config *cfg)
{
	struct msi_claw_drvdata *drvdata = hid_get_drvdata(hdev);
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
	data[4] = cfg->brightness;

	/* Keyframes */
	for (frame = 0; frame < cfg->frame_count; frame++) {
		u8 *frame_data = &data[5 + frame * 27];
		int zone;

		for (zone = 0; zone < MSI_CLAW_LED_ZONES; zone++) {
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

		ret = msi_claw_write_cmd(hdev, MSI_CLAW_COMMAND_TYPE_WRITE_PROFILE_DATA,
					  cmd_buffer, 4 + chunk_size);
		if (ret < 0) {
			hid_err(hdev, "hid-msi-claw LED: failed to send RGB data: %d\n", ret);
			goto out;
		}

		ret = msi_claw_await_ack(hdev);
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
static int msi_claw_read_rgb_config(struct hid_device *hdev,
				    u8 *speed, u8 *brightness)
{
	struct msi_claw_drvdata *drvdata = hid_get_drvdata(hdev);
	u8 cmd_buffer[4];
	u8 buffer[MSI_CLAW_READ_SIZE] = {};
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
	msi_claw_flush_queue(hdev);

	ret = msi_claw_write_cmd(hdev, MSI_CLAW_COMMAND_TYPE_READ_PROFILE,
				 cmd_buffer, sizeof(cmd_buffer));
	if (ret < 0) {
		hid_err(hdev, "hid-msi-claw LED: failed to send read request: %d\n", ret);
		return ret;
	}

	ret = msi_claw_read(hdev, buffer, MSI_CLAW_READ_SIZE, 100);
	if (ret != MSI_CLAW_READ_SIZE) {
		hid_err(hdev, "hid-msi-claw LED: failed to read config: %d\n", ret);
		return -EINVAL;
	}

	if (buffer[4] != (u8)MSI_CLAW_COMMAND_TYPE_READ_PROFILE_ACK) {
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
static void msi_claw_led_lazy_load(struct msi_claw_led *led)
{
	if (led->config_loaded)
		return;

	led->config_loaded = true;

	if (msi_claw_read_rgb_config(led->hdev, &led->speed, &led->brightness) < 0) {
		hid_warn(led->hdev, "hid-msi-claw LED: lazy load failed, using defaults\n");
		led->speed = 50;
		led->brightness = 100;
	}

	/* Sync brightness to LED class device */
	led->mc_cdev.led_cdev.brightness = led->brightness;
}

/*
 * Speed conversion helpers
 * User speed: 0-100 (100 = fastest)
 * Device speed: 0-20 (0 = fastest, acts like frame interval)
 */

/* Convert user speed (0-100, 100=fastest) to device speed (0-20, 0=fastest) */
static inline u8 msi_claw_speed_to_device(u8 user_speed)
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
 * Example: msi_claw_speed_map(speed, 10, 50)
 *   user=0   -> mapped=10 -> device=18 (slow)
 *   user=100 -> mapped=50 -> device=10 (fast)
 *   user=50  -> mapped=30 -> device=14 (medium)
 */
static inline u8 msi_claw_speed_map(u8 user_speed, u8 range_slow, u8 range_fast)
{
	u8 mapped;

	/* Map user 0-100 to range_slow-range_fast */
	mapped = range_slow + (range_fast - range_slow) * user_speed / 100;
	/* Convert to device speed (invert: 0=fast, 20=slow) */
	return (100 - mapped) * 20 / 100;
}

/* Build solid effect (1 frame, all zones same color) */
static void msi_claw_build_monocolor(struct msi_claw_rgb_config *cfg,
				      struct msi_claw_led *led)
{
	cfg->frame_count = 1;
	cfg->speed = 0;  /* Static effect, speed doesn't matter */
	cfg->brightness = led->brightness;

	msi_claw_frame_fill_solid(&cfg->frames[0],
				   led->color[0], led->color[1], led->color[2]);
}

/* Build breathe effect (2 frames: color -> black) */
static void msi_claw_build_breathe(struct msi_claw_rgb_config *cfg,
				    struct msi_claw_led *led)
{
	cfg->frame_count = 2;
	/* breathe (2 frames): map to range */
	cfg->speed = msi_claw_speed_map(led->speed, 5, 100);
	cfg->brightness = led->brightness;

	/* Frame 0: main color */
	msi_claw_frame_fill_solid(&cfg->frames[0],
				   led->color[0], led->color[1], led->color[2]);
	/* Frame 1: black */
	msi_claw_frame_fill_solid(&cfg->frames[1], 0, 0, 0);
}

/* Build chroma effect (6 frames: rainbow cycle, all zones sync) */
static void msi_claw_build_chroma(struct msi_claw_rgb_config *cfg,
				   struct msi_claw_led *led)
{
	static const u16 hues[] = {0, 60, 120, 180, 240, 300};
	int i;
	u8 r, g, b;

	cfg->frame_count = 6;
	/* chroma (6 frames): map to range for slower animation */
	cfg->speed = msi_claw_speed_map(led->speed, 5, 100);
	cfg->brightness = led->brightness;

	for (i = 0; i < 6; i++) {
		msi_claw_hsv_to_rgb(hues[i], 255, 255, &r, &g, &b);
		msi_claw_frame_fill_solid(&cfg->frames[i], r, g, b);
	}
}

/* Build rainbow effect (4 frames: rotating colors around joysticks) */
static void msi_claw_build_rainbow(struct msi_claw_rgb_config *cfg,
				    struct msi_claw_led *led)
{
	static const u16 base_hues[] = {0, 90, 180, 270};
	int frame, zone;
	u8 r, g, b;
	u16 hue;

	cfg->frame_count = 4;
	/* rainbow (4 frames): map to range */
	cfg->speed = msi_claw_speed_map(led->speed, 10, 100);
	cfg->brightness = led->brightness;

	for (frame = 0; frame < 4; frame++) {
		/* Right joystick (zones 0-3): rotate */
		for (zone = 0; zone < 4; zone++) {
			hue = base_hues[(zone + frame) % 4];
			msi_claw_hsv_to_rgb(hue, 255, 255, &r, &g, &b);
			cfg->frames[frame].zones[zone][0] = r;
			cfg->frames[frame].zones[zone][1] = g;
			cfg->frames[frame].zones[zone][2] = b;
		}
		/* Left joystick (zones 4-7): rotate */
		for (zone = 0; zone < 4; zone++) {
			hue = base_hues[(zone + frame) % 4];
			msi_claw_hsv_to_rgb(hue, 255, 255, &r, &g, &b);
			cfg->frames[frame].zones[zone + 4][0] = r;
			cfg->frames[frame].zones[zone + 4][1] = g;
			cfg->frames[frame].zones[zone + 4][2] = b;
		}
		/* ABXY (zone 8): cycle */
		hue = base_hues[frame];
		msi_claw_hsv_to_rgb(hue, 255, 255, &r, &g, &b);
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
static void msi_claw_build_frostfire(struct msi_claw_rgb_config *cfg,
				     struct msi_claw_led *led)
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
	cfg->speed = msi_claw_speed_map(led->speed, 10, 100);
	cfg->brightness = led->brightness;

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
static void msi_claw_build_custom(struct msi_claw_rgb_config *cfg,
				   struct msi_claw_led *led)
{
	int frame, zone;

	cfg->frame_count = led->custom_frame_count;
	/* Custom: direct conversion, no compensation (user controls frame count) */
	cfg->speed = msi_claw_speed_to_device(led->speed);
	cfg->brightness = led->brightness;

	for (frame = 0; frame < led->custom_frame_count; frame++) {
		for (zone = 0; zone < MSI_CLAW_LED_ZONES; zone++) {
			cfg->frames[frame].zones[zone][0] = led->custom_frames[frame][zone][0];
			cfg->frames[frame].zones[zone][1] = led->custom_frames[frame][zone][1];
			cfg->frames[frame].zones[zone][2] = led->custom_frames[frame][zone][2];
		}
	}
}

/* Apply current effect to device */
static int msi_claw_apply_effect(struct msi_claw_led *led)
{
	struct msi_claw_rgb_config cfg = {};
	int ret;

	hid_dbg(led->hdev, "LED apply: enabled=%d mode=%s effect=%s speed=%d brightness=%d color=[%d,%d,%d]\n",
		led->enabled, led_mode_names[led->mode], led_effect_names[led->effect],
		led->speed, led->brightness,
		led->color[0], led->color[1], led->color[2]);

	if (!led->enabled) {
		/* Send black frame with brightness 0 */
		cfg.frame_count = 1;
		cfg.speed = 0;
		cfg.brightness = 0;
		msi_claw_frame_fill_solid(&cfg.frames[0], 0, 0, 0);
		hid_info(led->hdev, "LED disabled, sending black frame\n");
		return msi_claw_send_rgb_config(led->hdev, &cfg);
	}

	if (led->mode == MSI_CLAW_LED_MODE_CUSTOM) {
		/* Custom mode: use keyframes */
		if (led->custom_frame_count == 0) {
			hid_info(led->hdev, "LED custom: no keyframes, fallback to monocolor\n");
			msi_claw_build_monocolor(&cfg, led);
		} else {
			msi_claw_build_custom(&cfg, led);
		}
	} else {
		/* Dynamic mode: use preset effect */
		switch (led->effect) {
		case MSI_CLAW_LED_EFFECT_MONOCOLOR:
			msi_claw_build_monocolor(&cfg, led);
			break;
		case MSI_CLAW_LED_EFFECT_BREATHE:
			msi_claw_build_breathe(&cfg, led);
			break;
		case MSI_CLAW_LED_EFFECT_CHROMA:
			msi_claw_build_chroma(&cfg, led);
			break;
		case MSI_CLAW_LED_EFFECT_RAINBOW:
			msi_claw_build_rainbow(&cfg, led);
			break;
		case MSI_CLAW_LED_EFFECT_FROSTFIRE:
			msi_claw_build_frostfire(&cfg, led);
			break;
		default:
			return -EINVAL;
		}
	}

	hid_dbg(led->hdev, "LED sending: frames=%d speed=%d brightness=%d\n",
		cfg.frame_count, cfg.speed, cfg.brightness);

	ret = msi_claw_send_rgb_config(led->hdev, &cfg);
	if (ret)
		hid_err(led->hdev, "LED send failed: %d\n", ret);

	return ret;
}

/* Delayed work callback for throttled LED updates */
static void msi_claw_led_apply_work_fn(struct work_struct *work)
{
	struct msi_claw_led *led = container_of(work, struct msi_claw_led,
						apply_work.work);

	led->last_apply_jiffies = jiffies;
	msi_claw_apply_effect(led);
}

/*
 * Throttled apply: limits update rate while ensuring final value is sent.
 * Always uses delayed_work to avoid blocking sysfs writes.
 * This matches the async behavior of the LED subsystem's brightness handling.
 */
static void msi_claw_led_apply_throttled(struct msi_claw_led *led)
{
	unsigned long elapsed_ms;
	unsigned long delay_ms;

	if (!led->enabled)
		return;

	elapsed_ms = jiffies_to_msecs(jiffies - led->last_apply_jiffies);

	/* Cancel any pending work and reschedule */
	cancel_delayed_work(&led->apply_work);

	if (elapsed_ms >= MSI_CLAW_LED_THROTTLE_MS) {
		/* Enough time passed, schedule for immediate execution */
		delay_ms = 0;
	} else {
		/* Too soon, delay until throttle interval passes */
		delay_ms = MSI_CLAW_LED_THROTTLE_MS - elapsed_ms;
	}

	schedule_delayed_work(&led->apply_work, msecs_to_jiffies(delay_ms));
}

/* ========== LED sysfs attributes ========== */

/* Helper to get msi_claw_led from LED device */
static inline struct msi_claw_led *dev_to_msi_claw_led(struct device *dev)
{
	struct led_classdev *led_cdev = dev_get_drvdata(dev);
	struct led_classdev_mc *mc_cdev = lcdev_to_mccdev(led_cdev);

	return container_of(mc_cdev, struct msi_claw_led, mc_cdev);
}

static ssize_t enabled_show(struct device *dev,
			    struct device_attribute *attr, char *buf)
{
	struct msi_claw_led *led = dev_to_msi_claw_led(dev);

	return sysfs_emit(buf, "%s\n", led->enabled ? "true" : "false");
}

static ssize_t enabled_store(struct device *dev,
			     struct device_attribute *attr,
			     const char *buf, size_t count)
{
	struct msi_claw_led *led = dev_to_msi_claw_led(dev);
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
	ret = msi_claw_apply_effect(led);

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
	struct msi_claw_led *led = dev_to_msi_claw_led(dev);

	if (led->effect >= MSI_CLAW_LED_EFFECT_MAX)
		return -EINVAL;

	return sysfs_emit(buf, "%s\n", led_effect_names[led->effect]);
}

static ssize_t effect_store(struct device *dev,
			    struct device_attribute *attr,
			    const char *buf, size_t count)
{
	struct msi_claw_led *led = dev_to_msi_claw_led(dev);
	int i, ret;
	char effect_name[16];

	if (sscanf(buf, "%15s", effect_name) != 1)
		return -EINVAL;

	hid_info(led->hdev, "LED effect_store: %s -> %s\n",
		 led_effect_names[led->effect], effect_name);

	for (i = 0; i < MSI_CLAW_LED_EFFECT_MAX; i++) {
		if (strcmp(effect_name, led_effect_names[i]) == 0) {
			led->effect = i;
			ret = msi_claw_apply_effect(led);
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

	for (i = 0; i < MSI_CLAW_LED_EFFECT_MAX; i++) {
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
	struct msi_claw_led *led = dev_to_msi_claw_led(dev);

	if (led->mode >= MSI_CLAW_LED_MODE_MAX)
		return -EINVAL;

	return sysfs_emit(buf, "%s\n", led_mode_names[led->mode]);
}

static ssize_t mode_store(struct device *dev,
			  struct device_attribute *attr,
			  const char *buf, size_t count)
{
	struct msi_claw_led *led = dev_to_msi_claw_led(dev);
	int i, ret;
	char mode_name[16];

	if (sscanf(buf, "%15s", mode_name) != 1)
		return -EINVAL;

	hid_info(led->hdev, "LED mode_store: %s -> %s\n",
		 led_mode_names[led->mode], mode_name);

	for (i = 0; i < MSI_CLAW_LED_MODE_MAX; i++) {
		if (strcmp(mode_name, led_mode_names[i]) == 0) {
			led->mode = i;
			ret = msi_claw_apply_effect(led);
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

	for (i = 0; i < MSI_CLAW_LED_MODE_MAX; i++) {
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
	struct msi_claw_led *led = dev_to_msi_claw_led(dev);

	msi_claw_led_lazy_load(led);
	return sysfs_emit(buf, "%d\n", led->speed);
}

static ssize_t speed_store(struct device *dev,
			   struct device_attribute *attr,
			   const char *buf, size_t count)
{
	struct msi_claw_led *led = dev_to_msi_claw_led(dev);
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
	msi_claw_led_apply_throttled(led);

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
	struct msi_claw_led *led = dev_to_msi_claw_led(dev);
	int frame, zone, len = 0;

	if (led->custom_frame_count == 0)
		return sysfs_emit(buf, "(not set)\n");

	for (frame = 0; frame < led->custom_frame_count; frame++) {
		if (frame > 0)
			len += sysfs_emit_at(buf, len, " ");
		for (zone = 0; zone < MSI_CLAW_LED_ZONES; zone++) {
			len += sysfs_emit_at(buf, len, "%d,%d,%d%s",
					     led->custom_frames[frame][zone][0],
					     led->custom_frames[frame][zone][1],
					     led->custom_frames[frame][zone][2],
					     (zone < MSI_CLAW_LED_ZONES - 1) ? ";" : "");
		}
	}
	len += sysfs_emit_at(buf, len, "\n");

	return len;
}

static ssize_t keyframes_store(struct device *dev,
			       struct device_attribute *attr,
			       const char *buf, size_t count)
{
	struct msi_claw_led *led = dev_to_msi_claw_led(dev);
	u8 frames[MSI_CLAW_LED_MAX_FRAMES][MSI_CLAW_LED_ZONES][3];
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

	if (frame_count < 1 || frame_count > MSI_CLAW_LED_MAX_FRAMES)
		return -EINVAL;

	/* Parse frame data: frame1 frame2 ... (space-separated) */
	/* Each frame: R,G,B;R,G,B;...;R,G,B (9 zones, semicolon-separated) */
	for (frame = 0; frame < frame_count; frame++) {
		/* Skip leading spaces */
		while (*p == ' ')
			p++;

		for (zone = 0; zone < MSI_CLAW_LED_ZONES; zone++) {
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

			if (zone < MSI_CLAW_LED_ZONES - 1) {
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
	if (led->mode == MSI_CLAW_LED_MODE_CUSTOM && led->enabled) {
		ret = msi_claw_apply_effect(led);
		if (ret)
			return ret;
	}

	return count;
}
static DEVICE_ATTR_RW(keyframes);

/* ========== End LED sysfs attributes ========== */

/* LED multicolor brightness callback */
static int msi_claw_led_brightness_set(struct led_classdev *cdev,
				       enum led_brightness brightness)
{
	struct led_classdev_mc *mc = lcdev_to_mccdev(cdev);
	struct msi_claw_led *led = container_of(mc, struct msi_claw_led, mc_cdev);

	/* Calculate RGB from multi_intensity and brightness */
	led_mc_calc_color_components(mc, brightness);

	/* Store color values (scaled by brightness) */
	led->color[0] = mc->subled_info[0].brightness;
	led->color[1] = mc->subled_info[1].brightness;
	led->color[2] = mc->subled_info[2].brightness;

	hid_dbg(led->hdev, "LED brightness_set: %d -> %d, color=[%d,%d,%d]\n",
		led->brightness, brightness,
		led->color[0], led->color[1], led->color[2]);

	/* Store brightness directly (0-100) */
	led->brightness = brightness;

	/* Apply immediately if enabled */
	if (led->enabled)
		return msi_claw_apply_effect(led);

	return 0;
}

/* Determine RGB address based on firmware version */
static const uint8_t *msi_claw_get_rgb_addr(u16 bcd_device)
{
	u8 major = bcd_device >> 8;

	if (major == 1)
		return (bcd_device >= 0x0166) ? rgb_addr_new : rgb_addr_old;
	if (major == 2)
		return (bcd_device >= 0x0217) ? rgb_addr_new : rgb_addr_old;
	if (major >= 3)
		return rgb_addr_new;

	return rgb_addr_old;
}

/* Initialize and register LED device */
static int msi_claw_led_init(struct hid_device *hdev)
{
	struct msi_claw_drvdata *drvdata = hid_get_drvdata(hdev);
	struct msi_claw_led *led;
	int ret;

	led = devm_kzalloc(&hdev->dev, sizeof(*led), GFP_KERNEL);
	if (!led)
		return -ENOMEM;

	/* Set rgb_addr first so we can read config */
	drvdata->rgb_addr = msi_claw_get_rgb_addr(drvdata->bcd_device);

	led->hdev = hdev;
	led->enabled = true;
	led->mode = MSI_CLAW_LED_MODE_DYNAMIC;
	led->effect = MSI_CLAW_LED_EFFECT_MONOCOLOR;
	led->color[0] = 255;
	led->color[1] = 255;
	led->color[2] = 255;

	/* Use defaults, lazy load from device on first sysfs access */
	led->config_loaded = false;
	led->speed = 50;
	led->brightness = 100;

	/* Initialize throttling */
	INIT_DELAYED_WORK(&led->apply_work, msi_claw_led_apply_work_fn);
	led->last_apply_jiffies = jiffies;

	/* Setup multicolor LED */
	led->subled_info[0].color_index = LED_COLOR_ID_RED;
	led->subled_info[0].intensity = 255;
	led->subled_info[1].color_index = LED_COLOR_ID_GREEN;
	led->subled_info[1].intensity = 255;
	led->subled_info[2].color_index = LED_COLOR_ID_BLUE;
	led->subled_info[2].intensity = 255;

	led->mc_cdev.led_cdev.name = MSI_CLAW_LED_NAME;
	led->mc_cdev.led_cdev.brightness = led->brightness;
	led->mc_cdev.led_cdev.max_brightness = 100;
	led->mc_cdev.led_cdev.brightness_set_blocking = msi_claw_led_brightness_set;
	led->mc_cdev.num_colors = 3;
	led->mc_cdev.subled_info = led->subled_info;

	ret = devm_led_classdev_multicolor_register(&hdev->dev, &led->mc_cdev);
	if (ret) {
		hid_err(hdev, "hid-msi-claw: failed to register LED: %d\n", ret);
		return ret;
	}

	/* Create custom sysfs attributes on LED device */
	ret = sysfs_create_file(&led->mc_cdev.led_cdev.dev->kobj,
				&dev_attr_enabled.attr);
	if (ret)
		goto err_enabled;

	ret = sysfs_create_file(&led->mc_cdev.led_cdev.dev->kobj,
				&dev_attr_enabled_index.attr);
	if (ret)
		goto err_enabled_index;

	ret = sysfs_create_file(&led->mc_cdev.led_cdev.dev->kobj,
				&dev_attr_effect.attr);
	if (ret)
		goto err_effect;

	ret = sysfs_create_file(&led->mc_cdev.led_cdev.dev->kobj,
				&dev_attr_effect_index.attr);
	if (ret)
		goto err_effect_index;

	ret = sysfs_create_file(&led->mc_cdev.led_cdev.dev->kobj,
				&dev_attr_mode.attr);
	if (ret)
		goto err_mode;

	ret = sysfs_create_file(&led->mc_cdev.led_cdev.dev->kobj,
				&dev_attr_mode_index.attr);
	if (ret)
		goto err_mode_index;

	ret = sysfs_create_file(&led->mc_cdev.led_cdev.dev->kobj,
				&dev_attr_speed.attr);
	if (ret)
		goto err_speed;

	ret = sysfs_create_file(&led->mc_cdev.led_cdev.dev->kobj,
				&dev_attr_speed_range.attr);
	if (ret)
		goto err_speed_range;

	ret = sysfs_create_file(&led->mc_cdev.led_cdev.dev->kobj,
				&dev_attr_keyframes.attr);
	if (ret)
		goto err_keyframes;

	drvdata->led = led;

	hid_info(hdev, "hid-msi-claw: LED initialized (rgb_addr: 0x%02x%02x)\n",
		 drvdata->rgb_addr[0], drvdata->rgb_addr[1]);

	return 0;

err_keyframes:
	sysfs_remove_file(&led->mc_cdev.led_cdev.dev->kobj, &dev_attr_speed_range.attr);
err_speed_range:
	sysfs_remove_file(&led->mc_cdev.led_cdev.dev->kobj, &dev_attr_speed.attr);
err_speed:
	sysfs_remove_file(&led->mc_cdev.led_cdev.dev->kobj, &dev_attr_mode_index.attr);
err_mode_index:
	sysfs_remove_file(&led->mc_cdev.led_cdev.dev->kobj, &dev_attr_mode.attr);
err_mode:
	sysfs_remove_file(&led->mc_cdev.led_cdev.dev->kobj, &dev_attr_effect_index.attr);
err_effect_index:
	sysfs_remove_file(&led->mc_cdev.led_cdev.dev->kobj, &dev_attr_effect.attr);
err_effect:
	sysfs_remove_file(&led->mc_cdev.led_cdev.dev->kobj, &dev_attr_enabled_index.attr);
err_enabled_index:
	sysfs_remove_file(&led->mc_cdev.led_cdev.dev->kobj, &dev_attr_enabled.attr);
err_enabled:
	return ret;
}

/* Cleanup LED device */
static void msi_claw_led_exit(struct hid_device *hdev)
{
	struct msi_claw_drvdata *drvdata = hid_get_drvdata(hdev);
	struct kobject *kobj;

	if (!drvdata->led)
		return;

	/* Cancel any pending throttled work */
	cancel_delayed_work_sync(&drvdata->led->apply_work);

	kobj = &drvdata->led->mc_cdev.led_cdev.dev->kobj;

	sysfs_remove_file(kobj, &dev_attr_keyframes.attr);
	sysfs_remove_file(kobj, &dev_attr_speed_range.attr);
	sysfs_remove_file(kobj, &dev_attr_speed.attr);
	sysfs_remove_file(kobj, &dev_attr_mode_index.attr);
	sysfs_remove_file(kobj, &dev_attr_mode.attr);
	sysfs_remove_file(kobj, &dev_attr_effect_index.attr);
	sysfs_remove_file(kobj, &dev_attr_effect.attr);
	sysfs_remove_file(kobj, &dev_attr_enabled_index.attr);
	sysfs_remove_file(kobj, &dev_attr_enabled.attr);

	/* LED classdev is devm-managed, no need to unregister */
	drvdata->led = NULL;
}

/* ========== End LED registration ========== */

static bool msi_claw_fw_supports_m_remap(u16 bcd_device,
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

static int msi_claw_set_m_remap(struct hid_device *hdev,
	enum msi_claw_m_key m_key, const uint8_t *codes)
{
	struct msi_claw_drvdata *drvdata = hid_get_drvdata(hdev);
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
		goto msi_claw_set_m_remap_err;
	}

	ret = msi_claw_write_cmd(hdev, MSI_CLAW_COMMAND_TYPE_WRITE_PROFILE_DATA,
		cmd_buffer, sizeof(cmd_buffer));
	if (ret < 0) {
		hid_err(hdev, "hid-msi-claw failed to set m_remap: %d\n", ret);
		goto msi_claw_set_m_remap_err;
	} else if (ret != MSI_CLAW_WRITE_SIZE) {
		hid_err(hdev, "hid-msi-claw failed to write m_remap: %d\n", ret);
		ret = -EIO;
		goto msi_claw_set_m_remap_err;
	}

	ret = msi_claw_await_ack(hdev);
	if (ret) {
		hid_err(hdev, "hid-msi-claw failed to await ack for m_remap: %d\n", ret);
		goto msi_claw_set_m_remap_err;
	}

	ret = sync_to_rom(hdev);
	if (ret) {
		hid_err(hdev, "hid-msi-claw failed to sync m_remap to rom: %d\n", ret);
		goto msi_claw_set_m_remap_err;
	}

	return 0;

msi_claw_set_m_remap_err:
	return ret;
}

static int msi_claw_read_m_remap(struct hid_device *hdev,
	enum msi_claw_m_key m_key, uint8_t *codes, int *count)
{
	struct msi_claw_drvdata *drvdata = hid_get_drvdata(hdev);
	const uint8_t cmd_buffer[] = {
		0x01,
		drvdata->m_remap_addr[m_key][0],
		drvdata->m_remap_addr[m_key][1],
		0x07,
	};
	uint8_t buffer[MSI_CLAW_READ_SIZE] = {};
	int ret, i;

	if (!drvdata->control) {
		hid_err(hdev, "hid-msi-claw couldn't find control interface\n");
		return -ENODEV;
	}

	ret = msi_claw_write_cmd(hdev, MSI_CLAW_COMMAND_TYPE_READ_PROFILE,
		cmd_buffer, sizeof(cmd_buffer));
	if (ret < 0) {
		hid_err(hdev, "hid-msi-claw failed to send read m_remap request: %d\n", ret);
		return ret;
	} else if (ret != MSI_CLAW_WRITE_SIZE) {
		hid_err(hdev, "hid-msi-claw couldn't send read m_remap request: %d\n", ret);
		return -EIO;
	}

	ret = msi_claw_read(hdev, buffer, MSI_CLAW_READ_SIZE, 50);
	if (ret != MSI_CLAW_READ_SIZE) {
		hid_err(hdev, "hid-msi-claw failed to read m_remap: %d\n", ret);
		return -EINVAL;
	}

	if (buffer[4] != (uint8_t)MSI_CLAW_COMMAND_TYPE_READ_PROFILE_ACK) {
		hid_err(hdev, "hid-msi-claw expected READ_PROFILE_ACK (0x05), got 0x%02x\n", buffer[4]);
		return -EINVAL;
	}

	/* Extract key codes, filtering out 0xff (disabled/empty) */
	*count = 0;
	for (i = 0; i < MSI_CLAW_M_REMAP_MAX_KEYS; i++) {
		if (buffer[11 + i] != 0xff)
			codes[(*count)++] = buffer[11 + i];
	}

	return 0;
}

static int msi_claw_reset_device(struct hid_device *hdev)
{
	struct msi_claw_drvdata *drvdata = hid_get_drvdata(hdev);
	int ret;

	if (!drvdata->control) {
		hid_err(hdev, "hid-msi-claw couldn't find control interface\n");
		ret = -ENODEV;
		goto msi_claw_reset_device_err;
	}

	ret = msi_claw_write_cmd(hdev, MSI_CLAW_COMMAND_TYPE_RESET_DEVICE, NULL, 0);
	if (ret < 0) {
		hid_err(hdev, "hid-msi-claw failed to send reset: %d\n", ret);
		goto msi_claw_reset_device_err;
	} else if (ret != MSI_CLAW_WRITE_SIZE) {
		hid_err(hdev, "hid-msi-claw couldn't send reset request: %d\n", ret);
		ret = -EIO;
		goto msi_claw_reset_device_err;
	}

	ret = msi_claw_await_ack(hdev);
	if (ret) {
		hid_err(hdev, "hid-msi-claw failed to await ack: %d\n", ret);
		goto msi_claw_reset_device_err;
	}

msi_claw_reset_device_err:
	return ret;
}

static int msi_claw_read_gamepad_mode(struct hid_device *hdev,
	struct msi_claw_control_status *status)
{
	uint8_t buffer[MSI_CLAW_READ_SIZE] = {};
	int ret;

	ret = msi_claw_write_cmd(hdev, MSI_CLAW_COMMAND_TYPE_READ_GAMEPAD_MODE, NULL, 0);
	if (ret < 0) {
		hid_err(hdev, "hid-msi-claw failed to send read request for controller mode: %d\n", ret);
		goto msi_claw_read_gamepad_mode_err;
	} else if (ret != MSI_CLAW_WRITE_SIZE) {
		hid_err(hdev, "hid-msi-claw couldn't send request: %d\n", ret);
		ret = -EIO;
		goto msi_claw_read_gamepad_mode_err;
	}

	ret = msi_claw_read(hdev, buffer, MSI_CLAW_READ_SIZE, 50);
	if (ret != MSI_CLAW_READ_SIZE) {
		hid_err(hdev, "hid-msi-claw failed to read: %d\n", ret);
		ret = -EINVAL;
		goto msi_claw_read_gamepad_mode_err;
	}

	if (buffer[4] != (uint8_t)MSI_CLAW_COMMAND_TYPE_GAMEPAD_MODE_ACK) {
		hid_err(hdev, "hid-msi-claw received invalid response: expected 0x27, got 0x%02x\n", buffer[4]);
		ret = -EINVAL;
		goto msi_claw_read_gamepad_mode_err;
	} else if (buffer[5] >= MSI_CLAW_GAMEPAD_MODE_MAX) {
		hid_err(hdev, "hid-msi-claw unknown gamepad mode: 0x%02x\n", buffer[5]);
		ret = -EINVAL;
		goto msi_claw_read_gamepad_mode_err;
	} else if (buffer[6] >= MSI_CLAW_MKEY_FUNCTION_MAX) {
		hid_err(hdev, "hid-msi-claw unknown gamepad mode: 0x%02x\n", buffer[6]);
		ret = -EINVAL;
		goto msi_claw_read_gamepad_mode_err;
	}

	status->gamepad_mode = (enum msi_claw_gamepad_mode)buffer[5];
	status->mkeys_function = (enum msi_claw_mkeys_function)buffer[6];

	ret = 0;

msi_claw_read_gamepad_mode_err:
	return ret;
}

static int msi_claw_switch_gamepad_mode(struct hid_device *hdev,
	const struct msi_claw_control_status *status)
{
	struct msi_claw_drvdata *drvdata = hid_get_drvdata(hdev);
	struct msi_claw_control_status check_status;
	const uint8_t cmd_buffer[2] = {(uint8_t)status->gamepad_mode, (uint8_t)status->mkeys_function};
	int ret;

	if (!drvdata->control) {
		hid_err(hdev, "hid-msi-claw couldn't find control interface\n");
		ret = -ENODEV;
		goto msi_claw_switch_gamepad_mode_err;
	}

	ret = msi_claw_write_cmd(hdev, MSI_CLAW_COMMAND_TYPE_SWITCH_MODE, cmd_buffer, sizeof(cmd_buffer));
	if (ret < 0) {
		hid_err(hdev, "hid-msi-claw failed to send write request to switch controller mode: %d\n", ret);
		goto msi_claw_switch_gamepad_mode_err;
	} else if (ret != MSI_CLAW_WRITE_SIZE) {
		hid_err(hdev, "hid-msi-claw failed to write: %d bytes got written\n", ret);
		ret = -EIO;
		goto msi_claw_switch_gamepad_mode_err;
	}

	ret = msi_claw_await_ack(hdev);
	if (ret) {
		hid_err(hdev, "hid-msi-claw failed to await ack: %d\n", ret);
		goto msi_claw_switch_gamepad_mode_err;
	}

	// check the new mode as official application does
	ret = msi_claw_read_gamepad_mode(hdev, &check_status);
	if (ret) {
		hid_err(hdev, "hid-msi-claw failed to read status: %d\n", ret);
		goto msi_claw_switch_gamepad_mode_err;
	}

	if (memcmp((const void *)&check_status, (const void *)status, sizeof(*status))) {
		hid_err(hdev, "hid-msi-claw current status and target one are different\n");
		ret = -EIO;
		goto msi_claw_switch_gamepad_mode_err;
	}

	// the device now sends back 03 00 00 00 00 00 00 00 00

	// this command is always issued by the windows counterpart after a mode switch
	ret = sync_to_rom(hdev);
	if (ret) {
		hid_err(hdev, "hid-msi-claw failed the sync to rom command: %d\n", ret);
		return ret;
	}

msi_claw_switch_gamepad_mode_err:
	return ret;
}

static ssize_t reset_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t count)
{
	struct hid_device *hdev = to_hid_device(dev);
	int ret;

	ret = msi_claw_reset_device(hdev);
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
	struct msi_claw_control_status status;
	int ret;

	ret = msi_claw_read_gamepad_mode(hdev, &status);
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
	struct msi_claw_drvdata *drvdata = hid_get_drvdata(hdev);
	enum msi_claw_gamepad_mode new_gamepad_mode = ARRAY_SIZE(gamepad_mode_map);
	struct msi_claw_control_status status = {
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
			new_gamepad_mode = (enum msi_claw_gamepad_mode)i;

	kfree(input);

	if (new_gamepad_mode == ARRAY_SIZE(gamepad_mode_map)) {
		hid_err(hdev, "Invalid gamepad mode selected\n");
		ret = -EINVAL;
		goto gamepad_mode_store_err;
	}

	status.gamepad_mode = new_gamepad_mode;
	ret = msi_claw_switch_gamepad_mode(hdev, &status);
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
	struct msi_claw_control_status status;
	int ret = msi_claw_read_gamepad_mode(hdev, &status);

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
	struct msi_claw_drvdata *drvdata = hid_get_drvdata(hdev);
	enum msi_claw_mkeys_function new_mkeys_function = ARRAY_SIZE(mkeys_function_map);
	struct msi_claw_control_status status = {
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
	err = msi_claw_switch_gamepad_mode(hdev, &status);
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
	uint8_t codes[MSI_CLAW_M_REMAP_MAX_KEYS];
	const char *name;
	int key_count, ret, i;
	ssize_t len = 0;

	ret = msi_claw_read_m_remap(hdev, MSI_CLAW_M1_KEY, codes, &key_count);
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
	uint8_t codes[MSI_CLAW_M_REMAP_MAX_KEYS];
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

		if (key_count >= MSI_CLAW_M_REMAP_MAX_KEYS) {
			hid_err(hdev, "hid-msi-claw too many keys (max %d)\n",
				MSI_CLAW_M_REMAP_MAX_KEYS);
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
	for (i = key_count; i < MSI_CLAW_M_REMAP_MAX_KEYS; i++)
		codes[i] = 0xff;

	ret = msi_claw_set_m_remap(hdev, MSI_CLAW_M1_KEY, codes);
	if (ret)
		return ret;

	return count;
}
static DEVICE_ATTR_RW(m1_remap);

static ssize_t m2_remap_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	struct hid_device *hdev = to_hid_device(dev);
	uint8_t codes[MSI_CLAW_M_REMAP_MAX_KEYS];
	const char *name;
	int key_count, ret, i;
	ssize_t len = 0;

	ret = msi_claw_read_m_remap(hdev, MSI_CLAW_M2_KEY, codes, &key_count);
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
	uint8_t codes[MSI_CLAW_M_REMAP_MAX_KEYS];
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

		if (key_count >= MSI_CLAW_M_REMAP_MAX_KEYS) {
			hid_err(hdev, "hid-msi-claw too many keys (max %d)\n",
				MSI_CLAW_M_REMAP_MAX_KEYS);
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
	for (i = key_count; i < MSI_CLAW_M_REMAP_MAX_KEYS; i++)
		codes[i] = 0xff;

	ret = msi_claw_set_m_remap(hdev, MSI_CLAW_M2_KEY, codes);
	if (ret)
		return ret;

	return count;
}
static DEVICE_ATTR_RW(m2_remap);

static int __maybe_unused msi_claw_resume(struct hid_device *hdev)
{
	int ret;
	struct msi_claw_drvdata *drvdata = hid_get_drvdata(hdev);
	struct msi_claw_control_status status = {
		.gamepad_mode = drvdata->control->gamepad_mode,
		.mkeys_function = drvdata->control->mkeys_function,
	};

	// TODO: clear out events list here (or in suspend?)

	// wait for device to be ready
	msleep(500);

	ret = msi_claw_switch_gamepad_mode(hdev, &status);
	if (ret) {
		hid_err(hdev, "Error changing gamepad mode: %d\n", (int)ret);
		goto msi_claw_resume_err;
	}

	// TODO: retry until this works?

	return 0;

msi_claw_resume_err:
	return ret;
}

static int msi_claw_probe(struct hid_device *hdev, const struct hid_device_id *id)
{
	int ret;
	struct msi_claw_drvdata *drvdata;

	if (!hid_is_usb(hdev)) {
		hid_err(hdev, "hid-msi-claw hid not usb\n");
		return -ENODEV;
	}

	drvdata = devm_kzalloc(&hdev->dev, sizeof(*drvdata), GFP_KERNEL);
	if (drvdata == NULL) {
		hid_err(hdev, "hid-msi-claw can't alloc descriptor\n");
		return -ENOMEM;
	}

	mutex_init(&drvdata->read_data_mutex);
	drvdata->read_data = NULL;
	drvdata->control = NULL;

	hid_set_drvdata(hdev, drvdata);

	ret = hid_parse(hdev);
	if (ret) {
		hid_err(hdev, "hid-msi-claw hid parse failed: %d\n", ret);
		return ret;
	}

	/*
	 * For non-control interfaces (keyboard/mouse), use HID_CONNECT_DEFAULT
	 * with INPUT_PER_APP quirk to create separate keyboard/mouse devices.
	 * This matches hid-generic behavior and allows userspace to grab the devices.
	 */
	if (hdev->rdesc[0] != MSI_CLAW_DEVICE_CONTROL_DESC) {
		hid_dbg(hdev, "non-control interface (0x%02x), using HID_CONNECT_DEFAULT\n",
			hdev->rdesc[0]);
		/* Set quirk to create separate input devices per HID application */
		hdev->quirks |= HID_QUIRK_INPUT_PER_APP;
		ret = hid_hw_start(hdev, HID_CONNECT_DEFAULT);
		if (ret) {
			hid_err(hdev, "hw start failed for non-control: %d\n", ret);
			return ret;
		}
		/* Don't call hid_hw_open() - let userspace grab the device */
		return 0;
	}

	/*
	 * For control interface: use HID_CONNECT_HIDRAW only (no input devices)
	 * and open the HID transport for sending commands.
	 */
	ret = hid_hw_start(hdev, HID_CONNECT_HIDRAW | HID_CONNECT_DRIVER);
	if (ret) {
		hid_err(hdev, "hid-msi-claw hw start failed: %d\n", ret);
		return ret;
	}

	ret = hid_hw_open(hdev);
	if (ret) {
		hid_err(hdev, "hid-msi-claw failed to open HID device: %d\n", ret);
		goto err_stop_hw;
	}

	/* Get firmware version for m_remap support */
	{
		struct usb_interface *intf = to_usb_interface(hdev->dev.parent);
		struct usb_device *udev = interface_to_usbdev(intf);
		drvdata->bcd_device = le16_to_cpu(udev->descriptor.bcdDevice);
	}

	drvdata->m_remap_supported = msi_claw_fw_supports_m_remap(
		drvdata->bcd_device, &drvdata->m_remap_addr);

	if (!drvdata->m_remap_supported)
		hid_warn(hdev, "hid-msi-claw firmware 0x%04x not supported for m_remap\n",
			drvdata->bcd_device);

	if (hdev->rdesc[0] == MSI_CLAW_DEVICE_CONTROL_DESC) {
		drvdata->control = devm_kzalloc(&hdev->dev, sizeof(*(drvdata->control)), GFP_KERNEL);
		if (drvdata->control == NULL) {
			hid_err(hdev, "hid-msi-claw can't alloc control interface data\n");
			ret = -ENOMEM;
			goto err_close;
		}

		drvdata->control->gamepad_mode = MSI_CLAW_GAMEPAD_MODE_XINPUT;
		drvdata->control->mkeys_function = MSI_CLAW_MKEY_FUNCTION_MACRO;

		ret = sysfs_create_file(&hdev->dev.kobj, &dev_attr_gamepad_mode_index.attr);
		if (ret) {
			hid_err(hdev, "hid-msi-claw failed to sysfs_create_file dev_attr_gamepad_mode_index: %d\n", ret);
			goto err_close;
		}

		ret = sysfs_create_file(&hdev->dev.kobj, &dev_attr_gamepad_mode.attr);
		if (ret) {
			hid_err(hdev, "hid-msi-claw failed to sysfs_create_file dev_attr_gamepad_mode: %d\n", ret);
			goto err_dev_attr_gamepad_mode;
		}

		ret = sysfs_create_file(&hdev->dev.kobj, &dev_attr_mkeys_function_index.attr);
		if (ret) {
			hid_err(hdev, "hid-msi-claw failed to sysfs_create_file dev_attr_mkeys_function_index: %d\n", ret);
			goto err_dev_attr_mkeys_function_index;
		}

		ret = sysfs_create_file(&hdev->dev.kobj, &dev_attr_mkeys_function.attr);
		if (ret) {
			hid_err(hdev, "hid-msi-claw failed to sysfs_create_file dev_attr_mkeys_function: %d\n", ret);
			goto err_dev_attr_mkeys_function;
		}

		ret = sysfs_create_file(&hdev->dev.kobj, &dev_attr_reset.attr);
		if (ret) {
			hid_err(hdev, "hid-msi-claw failed to sysfs_create_file dev_attr_reset: %d\n", ret);
			goto err_dev_attr_reset;
		}

		if (drvdata->m_remap_supported) {
			ret = sysfs_create_file(&hdev->dev.kobj, &dev_attr_m_remap_index.attr);
			if (ret) {
				hid_err(hdev, "hid-msi-claw failed to create m_remap_index: %d\n", ret);
				goto err_m_remap_index;
			}

			ret = sysfs_create_file(&hdev->dev.kobj, &dev_attr_m1_remap.attr);
			if (ret) {
				hid_err(hdev, "hid-msi-claw failed to create m1_remap: %d\n", ret);
				goto err_m1_remap;
			}

			ret = sysfs_create_file(&hdev->dev.kobj, &dev_attr_m2_remap.attr);
			if (ret) {
				hid_err(hdev, "hid-msi-claw failed to create m2_remap: %d\n", ret);
				goto err_m2_remap;
			}
		}

		/* Initialize RGB LED */
		ret = msi_claw_led_init(hdev);
		if (ret) {
			hid_warn(hdev, "hid-msi-claw: LED init failed: %d (continuing)\n", ret);
			/* LED failure is not fatal, continue */
		}
	}

	return 0;

err_m2_remap:
	sysfs_remove_file(&hdev->dev.kobj, &dev_attr_m1_remap.attr);
err_m1_remap:
	sysfs_remove_file(&hdev->dev.kobj, &dev_attr_m_remap_index.attr);
err_m_remap_index:
	sysfs_remove_file(&hdev->dev.kobj, &dev_attr_reset.attr);
err_dev_attr_gamepad_mode:
	sysfs_remove_file(&hdev->dev.kobj, &dev_attr_gamepad_mode_index.attr);
err_dev_attr_mkeys_function_index:
	sysfs_remove_file(&hdev->dev.kobj, &dev_attr_gamepad_mode.attr);
err_dev_attr_mkeys_function:
	sysfs_remove_file(&hdev->dev.kobj, &dev_attr_mkeys_function_index.attr);
err_dev_attr_reset:
	sysfs_remove_file(&hdev->dev.kobj, &dev_attr_mkeys_function.attr);
err_close:
	hid_hw_close(hdev);
err_stop_hw:
	hid_hw_stop(hdev);
	return ret;
}

static void msi_claw_remove(struct hid_device *hdev)
{
	struct msi_claw_drvdata *drvdata = hid_get_drvdata(hdev);

	if (drvdata->control) {
		/* Cleanup LED */
		msi_claw_led_exit(hdev);

		if (drvdata->m_remap_supported) {
			sysfs_remove_file(&hdev->dev.kobj, &dev_attr_m_remap_index.attr);
			sysfs_remove_file(&hdev->dev.kobj, &dev_attr_m1_remap.attr);
			sysfs_remove_file(&hdev->dev.kobj, &dev_attr_m2_remap.attr);
		}
		sysfs_remove_file(&hdev->dev.kobj, &dev_attr_gamepad_mode_index.attr);
		sysfs_remove_file(&hdev->dev.kobj, &dev_attr_gamepad_mode.attr);
		sysfs_remove_file(&hdev->dev.kobj, &dev_attr_mkeys_function_index.attr);
		sysfs_remove_file(&hdev->dev.kobj, &dev_attr_mkeys_function.attr);
		sysfs_remove_file(&hdev->dev.kobj, &dev_attr_reset.attr);

		/* Only close if we opened (control interface only) */
		hid_hw_close(hdev);
	}

	hid_hw_stop(hdev);
}

static const struct hid_device_id msi_claw_devices[] = {
	{ HID_USB_DEVICE(USB_VENDOR_ID_MSI_NEW, USB_DEVICE_ID_MSI_CLAW_XINPUT) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_MSI_NEW, USB_DEVICE_ID_MSI_CLAW_DINPUT) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_MSI_NEW, USB_DEVICE_ID_MSI_CLAW_DESKTOP) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_MSI_NEW, USB_DEVICE_ID_MSI_CLAW_BIOS) },
	{ }
};
MODULE_DEVICE_TABLE(hid, msi_claw_devices);

static struct hid_driver msi_claw_driver = {
	.name			= "hid-msi-claw",
	.id_table		= msi_claw_devices,
	.raw_event		= msi_claw_raw_event,
	.probe			= msi_claw_probe,
	.remove			= msi_claw_remove,
 #ifdef CONFIG_PM
 	.resume			= msi_claw_resume,
 #endif
};
module_hid_driver(msi_claw_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Denis Benato <benato.denis96@gmail.com>");
MODULE_AUTHOR("Deerek J. Clark <derekjohn.clark@gmail.com>");
MODULE_DESCRIPTION("Manage MSI Claw gamepad device");
