// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Gaming Input Protocol driver for Xbox One/Series controllers
 *
 * Copyright (c) 2025 Valve Software
 *
 * This driver is based on the Microsoft GIP spec at:
 * https://aka.ms/gipdocs
 * https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-gipusb/e7c90904-5e21-426e-b9ad-d82adeee0dbc
 */

#ifndef _GIP_H
#define _GIP_H

#include <linux/hid.h>
#ifdef CONFIG_JOYSTICK_XBOX_GIP_LEDS
#include <linux/led-class-multicolor.h>
#endif
#include <linux/rcupdate.h>
#include <linux/usb/input.h>

#define BASE_GIP_MTU 64
#define MAX_GIP_MTU 2048

#define MAX_ATTACHMENTS 8

#define MAX_IN_MESSAGES 8
#define MAX_OUT_MESSAGES 8

#define GIP_QUIRK_NO_HELLO		BIT(0)
#define GIP_QUIRK_NO_IMPULSE_VIBRATION	BIT(1)
#define GIP_QUIRK_SWAP_LB_RB		BIT(2)

#define GIP_FEATURE_CONTROLLER				BIT(0)
#define GIP_FEATURE_CONSOLE_FUNCTION_MAP		BIT(1)
#define GIP_FEATURE_CONSOLE_FUNCTION_MAP_OVERFLOW	BIT(2)
#define GIP_FEATURE_ELITE_BUTTONS			BIT(3)
#define GIP_FEATURE_DYNAMIC_LATENCY_INPUT		BIT(4)
#define GIP_FEATURE_SECURITY_OPT_OUT			BIT(5)
#define GIP_FEATURE_MOTOR_CONTROL			BIT(6)
#define GIP_FEATURE_GUIDE_COLOR				BIT(7)
#define GIP_FEATURE_EXTENDED_SET_DEVICE_STATE		BIT(8)

/* System messages */
#define GIP_CMD_PROTO_CONTROL		0x01
#define GIP_CMD_HELLO_DEVICE		0x02
#define GIP_CMD_STATUS_DEVICE		0x03
#define GIP_CMD_METADATA		0x04
#define GIP_CMD_SET_DEVICE_STATE	0x05
#define GIP_CMD_SECURITY		0x06
#define GIP_CMD_GUIDE_BUTTON		0x07
#define GIP_CMD_AUDIO_CONTROL		0x08
#define GIP_CMD_LED			0x0a
#define GIP_CMD_HID_REPORT		0x0b
#define GIP_CMD_FIRMWARE		0x0c
#define GIP_CMD_EXTENDED		0x1e
#define GIP_CMD_DEBUG			0x1f
#define GIP_AUDIO_DATA			0x60

/* Navigation vendor messages */
#define GIP_CMD_DIRECT_MOTOR		0x09
#define GIP_LL_INPUT_REPORT		0x20
#define GIP_LL_OVERFLOW_INPUT_REPORT	0x26

/* Wheel and ArcadeStick vendor messages */
#define GIP_CMD_INITIAL_REPORTS_REQUEST	0x0a
#define GIP_LL_STATIC_CONFIGURATION	0x21
#define GIP_LL_BUTTON_INFO_REPORT	0x22

#define MAX_GIP_CMD 0x80

#define GIP_DEV(p) \
	_Generic((p), \
		struct gip_attachment * : gip_attachment_dev, \
		struct gip_interface * : gip_interface_dev, \
		struct gip_device * : gip_device_dev)(p)

enum gip_init_status {
	GIP_INIT_OK = 0,
	GIP_INIT_NO_INPUT = 1,
};

enum gip_metadata_status {
	GIP_METADATA_NONE = 0,
	GIP_METADATA_GOT = 1,
	GIP_METADATA_FAKED = 2,
	GIP_METADATA_PENDING = 3,
};

enum gip_elite_button_format {
	GIP_BTN_FMT_UNKNOWN,
	GIP_BTN_FMT_XBE1,
	GIP_BTN_FMT_XBE2_RAW,
	GIP_BTN_FMT_XBE2_4,
	GIP_BTN_FMT_XBE2_5,
};

struct gip_header {
	uint8_t message_type;
	uint8_t flags;
	uint8_t sequence_id;
	uint64_t length;
};

struct gip_raw_message {
	uint16_t num_bytes;
	uint8_t bytes[BASE_GIP_MTU];
};

struct gip_device_metadata {
	uint8_t num_audio_formats;
	uint8_t num_preferred_types;
	uint8_t num_supported_interfaces;
	uint8_t hid_descriptor_size;

	uint32_t in_system_messages[8];
	uint32_t out_system_messages[8];

	struct gip_audio_format_pair *audio_formats;
	char **preferred_types;
	guid_t *supported_interfaces;
	uint8_t *hid_descriptor;
};

struct gip_message_metadata {
	uint8_t type;
	uint16_t length;
	uint16_t data_type;
	uint32_t flags;
	uint16_t period;
	uint16_t persistence_timeout;
};

struct gip_metadata {
	uint16_t version_major;
	uint16_t version_minor;

	struct gip_device_metadata device;

	uint8_t num_messages;
	struct gip_message_metadata *message_metadata;
};

struct gip_status {
	int power_level;
	int charge;
	int battery_type;
	int battery_level;
};

struct gip_status_event {
	uint16_t event_type;
	uint32_t fault_tag;
	uint32_t fault_address;
};

struct gip_extended_status {
	struct gip_status base;
	bool device_active;

	int num_events;
	struct gip_status_event events[5];
};

struct gip_attachment;
typedef int (*gip_command_handler)(struct gip_attachment *a, const struct gip_header *header,
		const uint8_t *bytes, int num_bytes);

struct gip_device;
struct gip_attachment {
	const struct gip_driver *driver;
	struct gip_device *device;
	void *driver_data;
	gip_command_handler vendor_handlers[MAX_GIP_CMD];

	uint8_t attachment_index;
	struct input_dev __rcu *input;
	uint16_t vendor_id;
	uint16_t product_id;
	char *uniq;
	const char *name;
	char phys[32];
	char serial[32];
	struct mutex lock;

	uint8_t fragment_message;
	uint16_t total_length;
	uint8_t *fragment_data;
	uint32_t fragment_offset;
	struct delayed_work fragment_timeout;
	int fragment_retries;

	uint16_t firmware_major_version;
	uint16_t firmware_minor_version;

	enum gip_metadata_status got_metadata;
	struct delayed_work metadata_next;
	int metadata_retries;
	struct gip_metadata metadata;

	uint8_t seq_system;
	uint8_t seq_security;
	uint8_t seq_extended;
	uint8_t seq_audio;
	uint8_t seq_vendor;

	int device_state;
#ifdef CONFIG_JOYSTICK_XBOX_GIP_LEDS
	union {
		struct led_classdev standard;
		struct led_classdev_mc color;
	} guide_led;
#endif

	struct gip_extended_status status;

	enum gip_elite_button_format xbe_format;
	uint32_t features;
	uint32_t quirks;

	int extra_buttons;
	int extra_axes;

	bool dpad_as_buttons;
	struct hid_device __rcu *hdev;
};

struct gip_urb {
	struct urb *urb;
	uint8_t *data;
	unsigned int offset;
};

struct gip_interface {
	struct gip_device *device;
	struct usb_interface *intf;
	uint32_t mtu;
	int isoc_messages;

	struct urb *urb_in;
	uint8_t *in_data;

	struct usb_anchor out_anchor;
	struct gip_urb out_queue[MAX_OUT_MESSAGES];
};

struct gip_device {
	struct usb_device *udev;

	struct gip_interface data;
	struct gip_interface audio;

	struct gip_raw_message in_queue[MAX_IN_MESSAGES];
	int pending_in_messages;
	int next_in_message;

	struct work_struct receive_message;
	spinlock_t message_lock;

	struct gip_attachment *attachments[MAX_ATTACHMENTS];
};

struct gip_quirks {
	uint16_t vendor_id;
	uint16_t product_id;
	uint8_t attachment_index;
	const char *override_name;
	uint32_t added_features;
	uint32_t filtered_features;
	uint32_t quirks;
	uint32_t extra_in_system[8];
	uint32_t extra_out_system[8];
	uint8_t extra_buttons;
	uint8_t extra_axes;
};

struct gip_driver {
	const char *const *types;
	guid_t guid;

	const struct gip_quirks *quirks;

	int (*probe)(struct gip_attachment *a);
	void (*remove)(struct gip_attachment *a);
	int (*init)(struct gip_attachment *a);
	int (*setup_input)(struct gip_attachment *a, struct input_dev* input);
	int (*handle_input_report)(struct gip_attachment *a,
		struct input_dev* input, const uint8_t *bytes, int num_bytes);
	gip_command_handler vendor_handlers[MAX_GIP_CMD];
};

static inline struct device *gip_attachment_dev(struct gip_attachment *attachment)
{
	return &attachment->device->udev->dev;
}

static inline struct device *gip_interface_dev(struct gip_interface *intf)
{
	return &intf->device->udev->dev;
}

static inline struct device *gip_device_dev(struct gip_device *device)
{
	return &device->udev->dev;
}

bool gip_supports_vendor_message(struct gip_attachment *attachment, uint8_t command, bool upstream);

int gip_send_system_message(struct gip_attachment *attachment,
	uint8_t message_type, uint8_t flags, const void *bytes, int num_bytes);
int gip_send_vendor_message(struct gip_attachment *attachment,
	uint8_t message_type, uint8_t flags, const void *bytes, int num_bytes);

extern const struct gip_driver gip_driver_navigation;
extern const struct gip_driver gip_driver_gamepad;
extern const struct gip_driver gip_driver_arcade_stick;
extern const struct gip_driver gip_driver_wheel;
extern const struct gip_driver gip_driver_flight_stick;
#endif
