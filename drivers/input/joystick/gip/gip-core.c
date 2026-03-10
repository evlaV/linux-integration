// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Gaming Input Protocol driver for Xbox One/Series controllers
 *
 * Copyright (c) 2025 Valve Software
 *
 * TODO:
 * - Audio device support
 * - Security packet handshake
 * - Event logging
 * - Sending fragmented messages
 * - Raw character device
 * - Wheel support
 * - Flight stick support
 * - More arcade stick testing
 * - Arcade stick extra buttons
 * - Split into driver-per-attachment GIP-as-a-bus approach drivers
 *
 * This driver is based on the Microsoft GIP spec at:
 * https://aka.ms/gipdocs
 * https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-gipusb/e7c90904-5e21-426e-b9ad-d82adeee0dbc
 */

#include <linux/module.h>
#include <linux/uuid.h>
#include "gip.h"

#define GIP_WIRED_INTF_DATA 0
#define GIP_WIRED_INTF_AUDIO 1

#define MAX_MESSAGE_LENGTH 0x4000

#define MAX_AUDIO_MESSAGES 9

#define GIP_DATA_CLASS_COMMAND		(0u << 5)
#define GIP_DATA_CLASS_LOW_LATENCY	(1u << 5)
#define GIP_DATA_CLASS_STANDARD_LATENCY	(2u << 5)
#define GIP_DATA_CLASS_AUDIO		(3u << 5)

#define GIP_DATA_CLASS_SHIFT 5
#define GIP_DATA_CLASS_MASK (7u << 5)

/* Undocumented Elite 2 vendor messages */
#define GIP_CMD_RAW_REPORT		0x0c
#define GIP_CMD_GUIDE_COLOR		0x0e
#define GIP_SL_ELITE_CONFIG		0x4d

#define GIP_BTN_OFFSET_XBE1 28
#define GIP_BTN_OFFSET_XBE2 14

#define GIP_FLAG_FRAGMENT	BIT(7)
#define GIP_FLAG_INIT_FRAG	BIT(6)
#define GIP_FLAG_SYSTEM		BIT(5)
#define GIP_FLAG_ACME		BIT(4)
#define GIP_FLAG_ATTACHMENT_MASK 0x7

#define GIP_AUDIO_FORMAT_NULL		0
#define GIP_AUDIO_FORMAT_8000HZ_1CH	1
#define GIP_AUDIO_FORMAT_8000HZ_2CH	2
#define GIP_AUDIO_FORMAT_12000HZ_1CH	3
#define GIP_AUDIO_FORMAT_12000HZ_2CH	4
#define GIP_AUDIO_FORMAT_16000HZ_1CH	5
#define GIP_AUDIO_FORMAT_16000HZ_2CH	6
#define GIP_AUDIO_FORMAT_20000HZ_1CH	7
#define GIP_AUDIO_FORMAT_20000HZ_2CH	8
#define GIP_AUDIO_FORMAT_24000HZ_1CH	9
#define GIP_AUDIO_FORMAT_24000HZ_2CH	10
#define GIP_AUDIO_FORMAT_32000HZ_1CH	11
#define GIP_AUDIO_FORMAT_32000HZ_2CH	12
#define GIP_AUDIO_FORMAT_40000HZ_1CH	13
#define GIP_AUDIO_FORMAT_40000HZ_2CH	14
#define GIP_AUDIO_FORMAT_48000HZ_1CH	15
#define GIP_AUDIO_FORMAT_48000HZ_2CH	16
#define GIP_AUDIO_FORMAT_48000HZ_6CH	32
#define GIP_AUDIO_FORMAT_48000HZ_8CH	33
#define MAX_GIP_AUDIO_FORMAT GIP_AUDIO_FORMAT_48000HZ_8CH

/* Protocol Control constants */
#define GIP_CONTROL_CODE_ACK	0
#define GIP_CONTROL_CODE_NACK	1 /* obsolete */
#define GIP_CONTROL_CODE_UNK	2 /* obsolete */
#define GIP_CONTROL_CODE_AB	3 /* obsolete */
#define GIP_CONTROL_CODE_MPER	4 /* obsolete */
#define GIP_CONTROL_CODE_STOP	5 /* obsolete */
#define GIP_CONTROL_CODE_START	6 /* obsolete */
#define GIP_CONTROL_CODE_ERR	7 /* obsolete */

/* Status Device constants */
#define GIP_POWER_LEVEL_OFF	0
#define GIP_POWER_LEVEL_STANDBY	1 /* obsolete */
#define GIP_POWER_LEVEL_FULL	2

#define GIP_NOT_CHARGING	0
#define GIP_CHARGING		1
#define GIP_CHARGE_ERROR	2

#define GIP_BATTERY_ABSENT		0
#define GIP_BATTERY_STANDARD		1
#define GIP_BATTERY_RECHARGEABLE	2

#define GIP_BATTERY_CRITICAL	0
#define GIP_BATTERY_LOW		1
#define GIP_BATTERY_MEDIUM	2
#define GIP_BATTERY_FULL	3

#define GIP_EVENT_FAULT 0x0002

#define GIP_FAULT_UNKNOWN	0
#define GIP_FAULT_HARD		1
#define GIP_FAULT_NMI		2
#define GIP_FAULT_SVC		3
#define GIP_FAULT_PEND_SV	4
#define GIP_FAULT_SMART_PTR	5
#define GIP_FAULT_MCU		6
#define GIP_FAULT_BUS		7
#define GIP_FAULT_USAGE		8
#define GIP_FAULT_RADIO_HANG	9
#define GIP_FAULT_WATCHDOG	10
#define GIP_FAULT_LINK_STALL	11
#define GIP_FAULT_ASSERTION	12

/* Metadata constants */
#define GIP_MESSAGE_FLAG_BIG_ENDIAN		BIT(0)
#define GIP_MESSAGE_FLAG_RELIABLE		BIT(1)
#define GIP_MESSAGE_FLAG_SEQUENCED		BIT(2)
#define GIP_MESSAGE_FLAG_DOWNSTREAM		BIT(3)
#define GIP_MESSAGE_FLAG_UPSTREAM		BIT(4)
#define GIP_MESSAGE_FLAG_DS_REQUEST_RESPONSE	BIT(5)

#define GIP_DATA_TYPE_CUSTOM	1
#define GIP_DATA_TYPE_AUDIO	2
#define GIP_DATA_TYPE_SECURITY	3
#define GIP_DATA_TYPE_GIP	4

/* Set Device State constants */
#define GIP_STATE_START		0
#define GIP_STATE_STOP		1
#define GIP_STATE_STANDBY	2 /* obsolete */
#define GIP_STATE_FULL_POWER	3
#define GIP_STATE_OFF		4
#define GIP_STATE_QUIESCE	5
#define GIP_STATE_UNK6		6
#define GIP_STATE_RESET		7

/* Guide Button Status constants */
#define GIP_LED_GUIDE	0
#define GIP_LED_IR	1 /* deprecated, for Kinect */

#define GIP_LED_GUIDE_OFF		0
#define GIP_LED_GUIDE_ON		1
#define GIP_LED_GUIDE_FAST_BLINK	2
#define GIP_LED_GUIDE_SLOW_BLINK	3
#define GIP_LED_GUIDE_CHARGING_BLINK	4
#define GIP_LED_GUIDE_RAMP_TO_LEVEL	0xd

#define GIP_LED_IR_OFF		0
#define GIP_LED_IR_ON_100MS	1
#define GIP_LED_IR_PATTERN	4

/* Direct Motor Command constants */
#define GIP_MOTOR_RIGHT_VIBRATION	BIT(0)
#define GIP_MOTOR_LEFT_VIBRATION	BIT(1)
#define GIP_MOTOR_RIGHT_IMPULSE		BIT(2)
#define GIP_MOTOR_LEFT_IMPULSE		BIT(3)
#define GIP_MOTOR_ALL 0xf

/* Extended Command constants */
#define GIP_EXTCMD_GET_CAPABILITIES	0x00
#define GIP_EXTCMD_GET_TELEMETRY_DATA	0x01
#define GIP_EXTCMD_GET_SERIAL_NUMBER	0x04

#define GIP_EXTENDED_STATUS_OK			0
#define GIP_EXTENDED_STATUS_NOT_SUPPORTED	1
#define GIP_EXTENDED_STATUS_NOT_READY		2
#define GIP_EXTENDED_STATUS_ACCESS_DENIED	3
#define GIP_EXTENDED_STATUS_FAILED		4

/* Internal constants, not part of protocol */
#define GIP_DEFAULT_IN_SYSTEM_MESSAGES 0x5e
#define GIP_DEFAULT_OUT_SYSTEM_MESSAGES 0x472

#define GIP_FEATURE_CONTROLLER				BIT(0)
#define GIP_FEATURE_CONSOLE_FUNCTION_MAP		BIT(1)
#define GIP_FEATURE_CONSOLE_FUNCTION_MAP_OVERFLOW	BIT(2)
#define GIP_FEATURE_ELITE_BUTTONS			BIT(3)
#define GIP_FEATURE_DYNAMIC_LATENCY_INPUT		BIT(4)
#define GIP_FEATURE_SECURITY_OPT_OUT			BIT(5)
#define GIP_FEATURE_MOTOR_CONTROL			BIT(6)
#define GIP_FEATURE_GUIDE_COLOR				BIT(7)
#define GIP_FEATURE_EXTENDED_SET_DEVICE_STATE		BIT(8)

#define GIP_LED_GUIDE_MAX_BRIGHTNESS	100 /* Spec says 47, but larger values work */
#define GIP_LED_GUIDE_INIT_BRIGHTNESS	20

#ifndef VK_LWIN
#define VK_LWIN 0x5b
#endif

static const guid_t guid_console_function_map =
	GUID_INIT(0xecddd2fe, 0xd387, 0x4294, 0xbd, 0x96, 0x1a, 0x71, 0x2e, 0x3d, 0xc7, 0x7d);
static const guid_t guid_console_function_map_overflow =
	GUID_INIT(0x137d4bd0, 0x9347, 0x4472, 0xaa, 0x26, 0x8c, 0x34, 0xa0, 0x8f, 0xf9, 0xbd);
static const guid_t guid_controller =
	GUID_INIT(0x9776ff56, 0x9bfd, 0x4581, 0xad, 0x45, 0xb6, 0x45, 0xbb, 0xa5, 0x26, 0xd6);
static const guid_t guid_dev_auth_pc_opt_out =
	GUID_INIT(0x7a34ce77, 0x7de2, 0x45c6, 0x8c, 0xa4, 0x00, 0x42, 0xc0, 0x8b, 0xd9, 0x4a);
static const guid_t guid_dynamic_latency_input =
	GUID_INIT(0x87f2e56b, 0xc3bb, 0x49b1, 0x82, 0x65, 0xff, 0xff, 0xf3, 0x77, 0x99, 0xee);
static const guid_t guid_elite_buttons =
	GUID_INIT(0x37d19ff7, 0xb5c6, 0x49d1, 0xa7, 0x5e, 0x03, 0xb2, 0x4b, 0xef, 0x8c, 0x89);
static const guid_t guid_headset =
	GUID_INIT(0xbc25d1a3, 0xc24e, 0x4992, 0x9d, 0xda, 0xef, 0x4f, 0x12, 0x3e, 0xf5, 0xdc);

/*
 * The following GUIDs are observed, but the exact meanings aren't known, so
 * for now we document them but don't use them anywhere.
 *
 * GamepadEmu: GUID_INIT(0xe2e5f1bc, 0xa6e6, 0x41a2, 0x8f, 0x43, 0x33, 0xcf, 0xa2, 0x51, 0x09, 0x81)
 * IAudioOnly: GUID_INIT(0x92844cd1, 0xf7c8, 0x49ef, 0x97, 0x77, 0x46, 0x7d, 0xa7, 0x08, 0xad, 0x10)
 * IControllerProfileModeState: GUID_INIT(0xf758dc66, 0x022c, 0x48b8, 0xa4, 0xf6, 0x45, 0x7b, 0xa8, 0x0e, 0x2a, 0x5b)
 * ICustomAudio: GUID_INIT(0x63fd9cc9, 0x94ee, 0x4b5d, 0x9c, 0x4d, 0x8b, 0x86, 0x4c, 0x14, 0x9c, 0xac)
 * IExtendedDeviceFlags: GUID_INIT(0x34ad9b1e, 0x36ad, 0x4fb5, 0x8a, 0xc7, 0x17, 0x23, 0x4c, 0x9f, 0x54, 0x6f)
 * IProgrammableGamepad: GUID_INIT(0x31c1034d, 0xb5b7, 0x4551, 0x98, 0x13, 0x87, 0x69, 0xd4, 0xa0, 0xe4, 0xf9)
 * IVirtualDevice: GUID_INIT(0xdfd26825, 0x110a, 0x4e94, 0xb9, 0x37, 0xb2, 0x7c, 0xe4, 0x7b, 0x25, 0x40)
 * OnlineDevAuth: GUID_INIT(0x632b1fd1, 0xa3e9, 0x44f9, 0x84, 0x20, 0x5c, 0xe3, 0x44, 0xa0, 0x64, 0x04)
 *
 * Seen on Elite Controller, Adaptive Controller: 9ebd00a3-b5e6-4c08-a33b-673126459ec4
 * Seen on Adaptive Controller: ce1e58c5-221c-4bdb-9c24-bf3941601320
 * Seen on Adaptive Joystick: db02f681-5038-4219-8668-c3459c5c3293
 * Seen on Elite 2 Controller: f758dc66-022c-48b8-a4f6-457ba80e2a5b (IControllerProfileModeState)
 * Seen on Elite 2 Controller: 31c1034d-b5b7-4551-9813-8769d4a0e4f9 (IProgrammableGamepad)
 * Seen on Elite 2 Controller: 34ad9b1e-36ad-4fb5-8ac7-17234c9f546f (IExtendedDeviceFlags)
 * Seen on Elite 2 Controller: 88e0b694-6bd9-4416-a560-e7fafdfa528f
 * Seen on Elite 2 Controller: ea96c8c0-b216-448b-be80-7e5deb0698e2
 */

static const int gip_data_class_mtu[8] = { 64, 64, 64, 2048, 0, 0, 0, 0 };

struct gip_audio_format {
	uint16_t rate;
	uint8_t channels;
};

static const struct gip_audio_format gip_audio_format_table[MAX_GIP_AUDIO_FORMAT + 1] = {
	[GIP_AUDIO_FORMAT_8000HZ_1CH] = { .rate = 8000, .channels = 1 },
	[GIP_AUDIO_FORMAT_8000HZ_2CH] = { .rate = 8000, .channels = 2 },
	[GIP_AUDIO_FORMAT_12000HZ_1CH] = { .rate = 12000, .channels = 1 },
	[GIP_AUDIO_FORMAT_12000HZ_2CH] = { .rate = 12000, .channels = 2 },
	[GIP_AUDIO_FORMAT_16000HZ_1CH] = { .rate = 16000, .channels = 1 },
	[GIP_AUDIO_FORMAT_16000HZ_2CH] = { .rate = 16000, .channels = 2 },
	[GIP_AUDIO_FORMAT_20000HZ_1CH] = { .rate = 20000, .channels = 1 },
	[GIP_AUDIO_FORMAT_20000HZ_2CH] = { .rate = 20000, .channels = 2 },
	[GIP_AUDIO_FORMAT_24000HZ_1CH] = { .rate = 24000, .channels = 1 },
	[GIP_AUDIO_FORMAT_24000HZ_2CH] = { .rate = 24000, .channels = 2 },
	[GIP_AUDIO_FORMAT_32000HZ_1CH] = { .rate = 32000, .channels = 1 },
	[GIP_AUDIO_FORMAT_32000HZ_2CH] = { .rate = 32000, .channels = 2 },
	[GIP_AUDIO_FORMAT_40000HZ_1CH] = { .rate = 40000, .channels = 1 },
	[GIP_AUDIO_FORMAT_40000HZ_2CH] = { .rate = 40000, .channels = 2 },
	[GIP_AUDIO_FORMAT_48000HZ_1CH] = { .rate = 48000, .channels = 1 },
	[GIP_AUDIO_FORMAT_48000HZ_2CH] = { .rate = 48000, .channels = 2 },
	[GIP_AUDIO_FORMAT_48000HZ_6CH] = { .rate = 48000, .channels = 6 },
	[GIP_AUDIO_FORMAT_48000HZ_8CH] = { .rate = 48000, .channels = 8 },
};


static enum power_supply_property gip_battery_props[] = {
	POWER_SUPPLY_PROP_PRESENT,
	POWER_SUPPLY_PROP_CAPACITY_LEVEL,
	POWER_SUPPLY_PROP_SCOPE,
	POWER_SUPPLY_PROP_STATUS,
};

static const struct gip_quirks base_quirks[] = {
	/* PDP Rock Candy */
	{ 0x0e6f, 0x0246, 0, .quirks = GIP_QUIRK_NO_HELLO },

	{0},
};

struct gip_audio_format_pair {
	uint8_t inbound;
	uint8_t outbound;
};
static_assert(sizeof(struct gip_audio_format_pair) == 2);

struct gip_hello_device {
	uint64_t device_id;
	uint16_t vendor_id;
	uint16_t product_id;
	uint16_t firmware_major_version;
	uint16_t firmware_minor_version;
	uint16_t firmware_build_version;
	uint16_t firmware_revision;
	uint8_t hardware_major_version;
	uint8_t hardware_minor_version;
	uint8_t rf_proto_major_version;
	uint8_t rf_proto_minor_version;
	uint8_t security_major_version;
	uint8_t security_minor_version;
	uint8_t gip_major_version;
	uint8_t gip_minor_version;
};

struct gip_direct_motor {
	uint8_t command;
	uint8_t motor_bitmap;
	uint8_t left_impulse_level;
	uint8_t right_impulse_level;
	uint8_t left_vibration_level;
	uint8_t right_vibration_level;
	uint8_t duration;
	uint8_t delay;
	uint8_t repeat;
};

static const struct gip_driver* base_drivers[] = {
	&gip_driver_navigation,
	&gip_driver_gamepad,
	&gip_driver_arcade_stick,
	NULL /* Sentinel */
};

static int gip_decode_length(uint64_t *length, const uint8_t *bytes, int num_bytes)
{
	*length = 0;
	int offset;

	for (offset = 0; offset < num_bytes; offset++) {
		uint8_t byte = bytes[offset];

		*length |= (byte & 0x7full) << (offset * 7);
		if (!(byte & 0x80)) {
			offset++;
			break;
		}
	}
	return offset;
}

static int gip_encode_length(uint64_t length, uint8_t *bytes, int num_bytes)
{
	int offset;

	for (offset = 0; offset < num_bytes; offset++) {
		uint8_t byte = length & 0x7f;

		length >>= 7;
		if (length)
			byte |= 0x80;
		bytes[offset] = byte;
		if (!length) {
			offset++;
			break;
		}
	}
	return offset;
}

static bool gip_supports_system_message(struct gip_attachment *attachment,
	uint8_t command, bool upstream)
{
	if (upstream)
		return attachment->metadata.device
			.in_system_messages[command >> 5] & (1u << command);
	else
		return attachment->metadata.device
			.out_system_messages[command >> 5] & (1u << command);
}

bool gip_supports_vendor_message(struct gip_attachment *attachment,
	uint8_t command, bool upstream)
{
	size_t i;

	for (i = 0; i < attachment->metadata.num_messages; i++) {
		struct gip_message_metadata *metadata =
			&attachment->metadata.message_metadata[i];

		if (metadata->type != command)
			continue;
		if (metadata->flags & GIP_MESSAGE_FLAG_DS_REQUEST_RESPONSE)
			return true;

		if (upstream)
			return metadata->flags & GIP_MESSAGE_FLAG_UPSTREAM;
		else
			return metadata->flags & GIP_MESSAGE_FLAG_DOWNSTREAM;
	}
	return false;
}

static uint8_t gip_sequence_next(struct gip_attachment *attachment,
	uint8_t command, bool system)
{
	uint8_t seq;

	if (system) {
		switch (command) {
		case GIP_CMD_SECURITY:
			seq = attachment->seq_security++;
			if (!seq)
				seq = attachment->seq_security++;
			break;
		case GIP_CMD_EXTENDED:
			seq = attachment->seq_extended++;
			if (!seq)
				seq = attachment->seq_extended++;
			break;
		case GIP_AUDIO_DATA:
			seq = attachment->seq_audio++;
			if (!seq)
				seq = attachment->seq_audio++;
			break;
		default:
			seq = attachment->seq_system++;
			if (!seq)
				seq = attachment->seq_system++;
			break;
		}
	} else {
		seq = attachment->seq_vendor++;
		if (!seq)
			seq = attachment->seq_vendor++;
	}
	return seq;
}

static void gip_handle_quirks_array(struct gip_attachment *attachment,
	const struct gip_quirks *quirks)
{
	size_t i, j;

	for (i = 0; quirks[i].vendor_id; i++) {
		if (quirks[i].vendor_id != attachment->vendor_id)
			continue;
		if (quirks[i].product_id != attachment->product_id)
			continue;
		if (quirks[i].attachment_index != attachment->attachment_index)
			continue;

		attachment->features |= quirks[i].added_features;
		attachment->features &= ~quirks[i].filtered_features;
		attachment->quirks |= quirks[i].quirks;

		if (quirks[i].override_name)
			attachment->name = quirks[i].override_name;

		for (j = 0; j < 8; ++j) {
			struct gip_device_metadata *metadata = &attachment->metadata.device;

			metadata->in_system_messages[j] |= quirks[i].extra_in_system[j];
			metadata->out_system_messages[j] |= quirks[i].extra_out_system[j];
		}

		attachment->extra_buttons = quirks[i].extra_buttons;
		attachment->extra_axes = quirks[i].extra_axes;
		break;
	}

}

static void gip_handle_quirks(struct gip_attachment *attachment)
{
	gip_handle_quirks_array(attachment, base_quirks);

	if (attachment->driver && attachment->driver->quirks)
		gip_handle_quirks_array(attachment, attachment->driver->quirks);
}

static int gip_send_raw_message(struct gip_device *device,
	uint8_t message_type, uint8_t flags, uint8_t seq, const uint8_t *bytes,
	int num_bytes)
{
	struct gip_interface *intf;
	int offset = 3;
	struct gip_urb *urb = NULL;
	int i;
	int rc = 0;

	if (num_bytes < 0) {
		dev_warn(GIP_DEV(device), "Invalid message length %d\n", num_bytes);
		return -EINVAL;
	}

	if (num_bytes > gip_data_class_mtu[message_type >> GIP_DATA_CLASS_SHIFT]) {
		dev_err(GIP_DEV(device),
			"Attempted to send a message that requires fragmenting, which is not yet supported.\n");
		return -ENOTSUPP;
	}

	if ((message_type & GIP_DATA_CLASS_MASK) == GIP_DATA_CLASS_AUDIO)
		intf = &device->audio;
	else
		intf = &device->data;

	if (intf->isoc_messages) {
		/* TODO: Needed for audio support */
		dev_warn(GIP_DEV(intf), "Unimplemented isochronous message output\n");
		return -ENOTSUPP;
	}

	guard(spinlock_irqsave)(&device->message_lock);
	for (i = 0; i < MAX_OUT_MESSAGES && !urb; i++) {
		if (!intf->out_queue[i].urb)
			continue;
		if (!intf->out_queue[i].urb->anchor)
			urb = &intf->out_queue[i];
	}
	if (!urb) {
		dev_err(GIP_DEV(device), "Output queue is full; dropping message\n");
		return -ENOSPC;
	}
	urb->data[0] = message_type;
	urb->data[1] = flags;
	urb->data[2] = seq;
	offset += gip_encode_length(num_bytes, &urb->data[offset],
		sizeof(urb->data) - offset);

	if (num_bytes > 0)
		memcpy(&urb->data[offset], bytes, num_bytes);

	num_bytes += offset;
	urb->urb->transfer_buffer_length = num_bytes;

	print_hex_dump_debug(KBUILD_MODNAME ": Sending message: ",
		DUMP_PREFIX_OFFSET, 16, 1, urb->data, num_bytes,
		false);

	usb_anchor_urb(urb->urb, &intf->out_anchor);
	rc = usb_submit_urb(urb->urb, GFP_ATOMIC);
	if (rc) {
		dev_err(&intf->intf->dev,
			"%s - usb_submit_urb failed with result %d\n",
			__func__, rc);
		usb_unanchor_urb(urb->urb);
		rc = -EIO;
	}

	return rc;
}

int gip_send_system_message(struct gip_attachment *attachment,
	uint8_t message_type, uint8_t flags, const void *bytes, int num_bytes)
{
	return gip_send_raw_message(attachment->device, message_type,
		GIP_FLAG_SYSTEM | attachment->attachment_index | flags,
		gip_sequence_next(attachment, message_type, true),
		bytes, num_bytes);
}

int gip_send_vendor_message(struct gip_attachment *attachment,
	uint8_t message_type, uint8_t flags, const void *bytes, int num_bytes)
{
	return gip_send_raw_message(attachment->device, message_type, flags,
		gip_sequence_next(attachment, message_type, false),
		bytes, num_bytes);
}

static int gip_hid_ll_parse(struct hid_device *hdev)
{
	struct gip_attachment *attachment = hdev->driver_data;

	return hid_parse_report(hdev,
		attachment->metadata.device.hid_descriptor,
		attachment->metadata.device.hid_descriptor_size);
}

static int gip_hid_ll_start(struct hid_device *hdev)
{
	return 0;
}

static void gip_hid_ll_stop(struct hid_device *hdev)
{
}

static int gip_hid_ll_open(struct hid_device *hdev)
{
	return 0;
}

static void gip_hid_ll_close(struct hid_device *hdev)
{
}

static int gip_hid_ll_raw_request(struct hid_device *hdev,
	unsigned char reportnum, uint8_t *buf, size_t count,
	unsigned char report_type, int reqtype)
{
	/*
	 * TODO: Based on the metadata, output reports appear to be possible,
	 * but the chatpad doesn't have the LEDs it claims to support, so
	 * it's not clear how to test we're sending them properly.
	 */
	return 0;
}

static const struct hid_ll_driver gip_hid_ll_driver = {
	.parse = gip_hid_ll_parse,
	.start = gip_hid_ll_start,
	.stop = gip_hid_ll_stop,
	.open = gip_hid_ll_open,
	.close = gip_hid_ll_close,
	.raw_request = gip_hid_ll_raw_request,
};

static void gip_metadata_free(struct device *dev, struct gip_metadata *metadata)
{
	devm_kfree(dev, metadata->device.audio_formats);

	if (metadata->device.preferred_types) {
		int i;

		for (i = 0; i < metadata->device.num_preferred_types; i++)
			devm_kfree(dev, metadata->device.preferred_types[i]);
		devm_kfree(dev, metadata->device.preferred_types);
	}
	devm_kfree(dev, metadata->device.supported_interfaces);
	devm_kfree(dev, metadata->device.hid_descriptor);
	devm_kfree(dev, metadata->message_metadata);

	memset(metadata, 0, sizeof(*metadata));
}

static int gip_parse_audio_format_metadata(struct device *dev,
	struct gip_device_metadata *dev_metadata, const uint8_t *bytes,
	int length, int buffer_offset)
{
	unsigned int i;

	dev_metadata->num_audio_formats = bytes[buffer_offset];
	if (buffer_offset + dev_metadata->num_audio_formats * 2 + 1 > length)
		return -EINVAL;
	dev_metadata->audio_formats = devm_kmalloc_array(dev,
		dev_metadata->num_audio_formats, 2, GFP_KERNEL);
	if (!dev_metadata->audio_formats)
		return -ENOMEM;
	memcpy(dev_metadata->audio_formats, &bytes[buffer_offset + 1],
		dev_metadata->num_audio_formats * 2);

	for (i = 0; i < dev_metadata->num_audio_formats; i++) {
		const struct gip_audio_format_pair *pair = &dev_metadata->audio_formats[i];
		const struct gip_audio_format *inbound = NULL;
		const struct gip_audio_format *outbound = NULL;

		if (pair->inbound <= MAX_GIP_AUDIO_FORMAT) {
			inbound = &gip_audio_format_table[pair->inbound];
			if (pair->inbound != GIP_AUDIO_FORMAT_NULL && inbound->rate == 0)
				inbound = NULL;
		}
		if (!inbound)
			dev_warn(dev, "Unknown audio format %u\n", pair->inbound);

		if (pair->outbound <= MAX_GIP_AUDIO_FORMAT) {
			outbound = &gip_audio_format_table[pair->outbound];
			if (pair->outbound != GIP_AUDIO_FORMAT_NULL && outbound->rate == 0)
				outbound = NULL;
		}
		if (!outbound)
			dev_warn(dev, "Unknown audio format %u\n", pair->outbound);

		if (inbound && outbound)
			dev_dbg(dev,
				"Supported audio format: %uHz %uch inbound, %uHz %uch outbound\n",
				inbound->rate,
				inbound->channels,
				outbound->rate,
				outbound->channels);
	}
	return 0;
}

static int gip_parse_preferred_types_metadata(struct device *dev,
	struct gip_device_metadata *dev_metadata, const uint8_t *bytes,
	int length, int buffer_offset)
{
	int i;
	int count;

	dev_metadata->num_preferred_types = bytes[buffer_offset];
	dev_metadata->preferred_types = devm_kcalloc(dev,
		dev_metadata->num_preferred_types, sizeof(char *), GFP_KERNEL);
	if (!dev_metadata->preferred_types)
		return -ENOMEM;

	buffer_offset++;
	for (i = 0; i < dev_metadata->num_preferred_types; i++) {
		if (buffer_offset + 2 >= length)
			return -EINVAL;

		count = bytes[buffer_offset];
		count |= bytes[buffer_offset];
		buffer_offset += 2;
		if (buffer_offset + count > length)
			return -EINVAL;

		dev_metadata->preferred_types[i] = devm_kcalloc(dev, count + 1,
			sizeof(char), GFP_KERNEL);
		if (!dev_metadata->preferred_types[i])
			return -ENOMEM;
		memcpy(dev_metadata->preferred_types[i], &bytes[buffer_offset], count);
		buffer_offset += count;
	}

	return 0;
}

static int gip_parse_supported_interfaces_metadata(struct device *dev,
	struct gip_device_metadata *dev_metadata, const uint8_t *bytes,
	int length, int buffer_offset)
{
	dev_metadata->num_supported_interfaces = bytes[buffer_offset];
	if (buffer_offset + 1 +
		(int32_t) (dev_metadata->num_supported_interfaces * sizeof(guid_t)) > length)
		return -EINVAL;

	dev_metadata->supported_interfaces = devm_kmalloc_array(dev,
		dev_metadata->num_supported_interfaces, sizeof(guid_t), GFP_KERNEL);
	if (!dev_metadata->supported_interfaces)
		return -ENOMEM;

	memcpy(dev_metadata->supported_interfaces, &bytes[buffer_offset + 1],
		sizeof(guid_t) * dev_metadata->num_supported_interfaces);

	return 0;
}

static int gip_parse_hid_descriptor_metadata(struct device *dev,
	struct gip_device_metadata *dev_metadata, const uint8_t *bytes,
	int length, int buffer_offset)
{
	dev_metadata->hid_descriptor_size = bytes[buffer_offset];
	if (buffer_offset + 1 + dev_metadata->hid_descriptor_size > length)
		return -EINVAL;

	dev_metadata->hid_descriptor = devm_kmalloc(dev,
		dev_metadata->hid_descriptor_size, GFP_KERNEL);
	if (!dev_metadata->hid_descriptor)
		return -ENOMEM;

	memcpy(dev_metadata->hid_descriptor, &bytes[buffer_offset + 1],
		dev_metadata->hid_descriptor_size);
	print_hex_dump_debug(KBUILD_MODNAME ": Received HID descriptor: ",
		DUMP_PREFIX_OFFSET, 16, 1, dev_metadata->hid_descriptor,
		dev_metadata->hid_descriptor_size, false);

	return 0;
}

static int gip_parse_device_metadata(struct device *dev,
	struct gip_metadata *metadata, const uint8_t *bytes, int num_bytes,
	int *offset)
{
	struct gip_device_metadata *dev_metadata = &metadata->device;
	int buffer_offset;
	int count;
	int length;
	int i;
	int rc;

	bytes = &bytes[*offset];
	num_bytes -= *offset;
	if (num_bytes < 16)
		return -EINVAL;

	length = bytes[0];
	length |= bytes[1] << 8;
	if (num_bytes < length)
		return -EINVAL;

	/* Skip supported firmware versions for now */

	buffer_offset = bytes[4];
	buffer_offset |= bytes[5] << 8;
	if (buffer_offset >= length)
		return -EINVAL;

	if (buffer_offset > 0) {
		rc = gip_parse_audio_format_metadata(dev, dev_metadata,
			bytes, length, buffer_offset);
		if (rc)
			return rc;
	}

	buffer_offset = bytes[6];
	buffer_offset |= bytes[7] << 8;
	if (buffer_offset >= length)
		return -EINVAL;

	if (buffer_offset > 0) {
		count = bytes[buffer_offset];
		if (buffer_offset + count + 1 > length)
			return -EINVAL;

		for (i = 0; i < count; i++) {
			uint8_t message = bytes[buffer_offset + 1 + i];

			dev_dbg(dev,
				"Supported upstream system message %02x\n",
				message);
			dev_metadata->in_system_messages[message >> 5] |=
				BIT(message & 0x1F);
		}
	}

	buffer_offset = bytes[8];
	buffer_offset |= bytes[9] << 8;
	if (buffer_offset >= length)
		return -EINVAL;

	if (buffer_offset > 0) {
		count = bytes[buffer_offset];
		if (buffer_offset + count + 1 > length)
			return -EINVAL;

		for (i = 0; i < count; i++) {
			uint8_t message = bytes[buffer_offset + 1 + i];

			dev_dbg(dev,
				"Supported downstream system message %02x\n",
				message);
			dev_metadata->out_system_messages[message >> 5] |=
				BIT(message & 0x1F);
		}
	}

	buffer_offset = bytes[10];
	buffer_offset |= bytes[11] << 8;
	if (buffer_offset >= length)
		return -EINVAL;

	if (buffer_offset > 0) {
		rc = gip_parse_preferred_types_metadata(dev, dev_metadata,
			bytes, length, buffer_offset);
		if (rc)
			return rc;
	}

	buffer_offset = bytes[12];
	buffer_offset |= bytes[13] << 8;
	if (buffer_offset >= length)
		return -EINVAL;

	if (buffer_offset > 0) {
		rc = gip_parse_supported_interfaces_metadata(dev,
			dev_metadata, bytes, length, buffer_offset);
		if (rc)
			return rc;
	}

	if (metadata->version_major > 1 || metadata->version_minor >= 1) {
		/* HID descriptor support added in metadata version 1.1 */
		buffer_offset = bytes[14];
		buffer_offset |= bytes[15] << 8;
		if (buffer_offset >= length)
			return -EINVAL;

		if (buffer_offset > 0) {
			rc = gip_parse_hid_descriptor_metadata(dev,
				dev_metadata, bytes, length, buffer_offset);
			if (rc)
				return rc;
		}
	}

	*offset += length;
	return 0;
}

static int gip_parse_message_metadata(struct device *dev,
	struct gip_message_metadata *metadata, const uint8_t *bytes,
	int num_bytes, int *offset)
{
	uint16_t length;

	bytes = &bytes[*offset];
	num_bytes -= *offset;

	if (num_bytes < 2)
		return -EINVAL;

	length = bytes[0];
	length |= bytes[1] << 8;
	if (num_bytes < length)
		return -EINVAL;

	if (length < 15)
		return -EINVAL;

	metadata->type = bytes[2];
	metadata->length = bytes[3];
	metadata->length |= bytes[4] << 8;
	metadata->data_type = bytes[5];
	metadata->data_type |= bytes[6] << 8;
	metadata->flags = bytes[7];
	metadata->flags |= bytes[8] << 8;
	metadata->flags |= bytes[9] << 16;
	metadata->flags |= bytes[10] << 24;
	metadata->period = bytes[11];
	metadata->period |= bytes[12] << 8;
	metadata->persistence_timeout = bytes[13];
	metadata->persistence_timeout |= bytes[14] << 8;

	dev_dbg(dev,
		"Supported vendor message type %02x of length %d, %s, %s, %s\n",
		metadata->type, metadata->length,
		metadata->flags & GIP_MESSAGE_FLAG_UPSTREAM ?
			(metadata->flags & GIP_MESSAGE_FLAG_DOWNSTREAM ? "bidirectional" : "upstream") :
			metadata->flags & GIP_MESSAGE_FLAG_DOWNSTREAM ? "downstream" :
			metadata->flags & GIP_MESSAGE_FLAG_DS_REQUEST_RESPONSE ? "downstream request response" :
			"unknown direction",
		metadata->flags & GIP_MESSAGE_FLAG_SEQUENCED ? "sequenced" : "not sequenced",
		metadata->flags & GIP_MESSAGE_FLAG_RELIABLE ? "reliable" : "unreliable");

	*offset += length;
	return 0;
}

static bool gip_parse_metadata(struct device *dev,
	struct gip_metadata *metadata, const uint8_t *bytes, int num_bytes)
{
	int header_size;
	int metadata_size;
	int offset = 0;
	int i;
	int rc;

	if (num_bytes < 16)
		return -EINVAL;

	print_hex_dump_debug(KBUILD_MODNAME ": Received metadata: ", DUMP_PREFIX_OFFSET,
		16, 1, bytes, num_bytes, false);

	header_size = bytes[0];
	header_size |= bytes[1] << 8;
	if (num_bytes < header_size || header_size < 16)
		return -EINVAL;

	metadata->version_major = bytes[2];
	metadata->version_major |= bytes[3] << 8;
	metadata->version_minor = bytes[4];
	metadata->version_minor |= bytes[5] << 8;
	/* Middle bytes are reserved */
	metadata_size = bytes[14];
	metadata_size |= bytes[15] << 8;

	if (num_bytes < metadata_size || metadata_size < header_size)
		return -EINVAL;

	offset = header_size;

	rc = gip_parse_device_metadata(dev, metadata, bytes, num_bytes, &offset);
	if (rc)
		goto parse_err;

	if (offset >= num_bytes)
		goto parse_err;

	metadata->num_messages = bytes[offset];
	offset++;
	if (metadata->num_messages > 0) {
		metadata->message_metadata = devm_kcalloc(dev,
			metadata->num_messages,
			sizeof(*metadata->message_metadata), GFP_KERNEL);
		if (!metadata->message_metadata)
			return -ENOMEM;

		for (i = 0; i < metadata->num_messages; i++) {
			rc = gip_parse_message_metadata(dev,
				&metadata->message_metadata[i], bytes,
				num_bytes, &offset);
			if (rc)
				goto parse_err;
		}
	}

	return 0;

parse_err:
	gip_metadata_free(dev, metadata);
	return rc;
}

static int gip_acknowledge(struct gip_device *device,
	const struct gip_header *header, uint32_t fragment_offset,
	uint16_t bytes_remaining)
{
	uint8_t buffer[] = {
		GIP_CONTROL_CODE_ACK,
		header->message_type,
		header->flags & GIP_FLAG_SYSTEM,
		fragment_offset,
		fragment_offset >> 8,
		fragment_offset >> 16,
		fragment_offset >> 24,
		bytes_remaining,
		bytes_remaining >> 8,
	};

	return gip_send_raw_message(device, GIP_CMD_PROTO_CONTROL,
		GIP_FLAG_SYSTEM | (header->flags & GIP_FLAG_ATTACHMENT_MASK),
		header->sequence_id, buffer, sizeof(buffer));
}

static int gip_fragment_failed(struct gip_attachment *attachment,
	const struct gip_header *header)
{
	attachment->fragment_retries++;
	if (attachment->fragment_retries > 8) {
		devm_kfree(GIP_DEV(attachment), attachment->fragment_data);
		attachment->fragment_data = NULL;
		attachment->fragment_message = 0;
	}
	return gip_acknowledge(attachment->device, header,
		attachment->fragment_offset,
		attachment->total_length - attachment->fragment_offset);
}

static int gip_bind_driver(struct gip_attachment *attachment, const struct gip_driver *driver)
{
	if (driver->probe) {
		int rc = driver->probe(attachment);

		if (rc)
			return rc;
	}

	attachment->driver = driver;
	memcpy(attachment->vendor_handlers, driver->vendor_handlers,
		sizeof(attachment->vendor_handlers));
	return 0;
}

static int gip_enable_elite_buttons(struct gip_attachment *attachment)
{
	if (attachment->vendor_id == 0x045e) {
		if (attachment->product_id == 0x02e3) {
			attachment->xbe_format = GIP_BTN_FMT_XBE1;
		} else if (attachment->product_id == 0x0b00) {
			if (attachment->firmware_major_version == 4) {
				attachment->xbe_format = GIP_BTN_FMT_XBE2_4;
			} else if (attachment->firmware_major_version == 5) {
				/*
				 * The exact range for this being necessary is
				 * unknown, but it starts at 5.11 and at either
				 * 5.16 or 5.17. This approach still works on
				 * 5.21, even if it's not necessary, so having
				 * a loose upper limit is fine.
				 */
				if (attachment->firmware_minor_version >= 11 &&
					attachment->firmware_minor_version < 17)
					attachment->xbe_format = GIP_BTN_FMT_XBE2_RAW;
				else
					attachment->xbe_format = GIP_BTN_FMT_XBE2_5;
			}
		}
	}

	if (attachment->xbe_format == GIP_BTN_FMT_XBE2_RAW) {
		/*
		 * The meaning of this packet is unknown and not documented, but
		 * it's needed for the Elite 2 controller to send raw reports
		 */
		static const uint8_t enable_raw_report[] = { 7, 0 };

		return gip_send_vendor_message(attachment, GIP_SL_ELITE_CONFIG,
			0, enable_raw_report, sizeof(enable_raw_report));
	}

	return 0;
}

#ifdef CONFIG_JOYSTICK_XBOX_GIP_FF
static int gip_play_effect(struct input_dev *dev, void *data, struct ff_effect *effect)
{
	struct gip_attachment *attachment = input_get_drvdata(dev);
	struct gip_direct_motor control = {
		.motor_bitmap = GIP_MOTOR_LEFT_VIBRATION | GIP_MOTOR_RIGHT_VIBRATION
	};

	if (effect->type != FF_RUMBLE)
		return 0;

	control.left_vibration_level = effect->u.rumble.strong_magnitude * 100 / 0xFFFF;
	control.right_vibration_level = effect->u.rumble.weak_magnitude * 100 / 0xFFFF;
	control.duration = 255;

	return gip_send_vendor_message(attachment, GIP_CMD_DIRECT_MOTOR,
		0, &control, sizeof(control));
}
#endif

static int gip_send_guide_button_led(struct gip_attachment *attachment,
	uint8_t pattern, uint8_t intensity)
{
	uint8_t buffer[] = {
		GIP_LED_GUIDE,
		pattern,
		intensity,
	};

	if (!gip_supports_system_message(attachment, GIP_CMD_LED, false))
		return 0;

#ifdef CONFIG_JOYSTICK_XBOX_GIP_LEDS
	if (!(attachment->features & GIP_FEATURE_GUIDE_COLOR))
		attachment->guide_led.standard.brightness = intensity;
#endif

	return gip_send_system_message(attachment, GIP_CMD_LED, 0, buffer, sizeof(buffer));
}

#ifdef CONFIG_JOYSTICK_XBOX_GIP_LEDS
static int gip_send_guide_button_color_led(struct gip_attachment *attachment,
	uint8_t r, uint8_t g, uint8_t b, uint8_t w)
{
	uint8_t buffer[] = { 0x00, w, r, g, b };

	if (!(attachment->features & GIP_FEATURE_GUIDE_COLOR))
		return -EINVAL;

	attachment->guide_led.color.subled_info[0].brightness = r;
	attachment->guide_led.color.subled_info[1].brightness = g;
	attachment->guide_led.color.subled_info[2].brightness = b;
	attachment->guide_led.color.subled_info[3].brightness = w;

	return gip_send_vendor_message(attachment, GIP_CMD_GUIDE_COLOR, 0, buffer, sizeof(buffer));
}

static int gip_guide_led_set(struct led_classdev *led,
	enum led_brightness value)
{
	struct gip_attachment *attachment = container_of(led,
		struct gip_attachment, guide_led.standard);

	guard(mutex)(&attachment->lock);
	return gip_send_guide_button_led(attachment, GIP_LED_GUIDE_ON, value);
}

static int gip_guide_color_led_set(struct led_classdev *led,
	enum led_brightness value)
{
	struct led_classdev_mc *mc_cdev = container_of(led,
		struct led_classdev_mc, led_cdev);
	struct gip_attachment *attachment = container_of(mc_cdev,
		struct gip_attachment, guide_led.color);

	led_mc_calc_color_components(mc_cdev, value);
	guard(mutex)(&attachment->lock);
	return gip_send_guide_button_color_led(attachment,
		mc_cdev->subled_info[0].brightness,
		mc_cdev->subled_info[1].brightness,
		mc_cdev->subled_info[2].brightness,
		mc_cdev->subled_info[3].brightness);
}

static int gip_guide_led_probe(struct gip_attachment *attachment, struct device *dev)
{
	int rc = 0;

	if (!gip_supports_system_message(attachment, GIP_CMD_LED, false))
		return 0;

	if (attachment->features & GIP_FEATURE_GUIDE_COLOR) {
		struct mc_subled *mc_led_info;
		struct led_classdev_mc *mc_cdev = &attachment->guide_led.color;
		struct led_classdev *cdev = &mc_cdev->led_cdev;

		mc_led_info = devm_kcalloc(dev, 4,
			sizeof(*mc_led_info), GFP_KERNEL);
		if (!mc_led_info)
			return -ENOMEM;

		mc_led_info[0].color_index = LED_COLOR_ID_RED;
		mc_led_info[1].color_index = LED_COLOR_ID_GREEN;
		mc_led_info[2].color_index = LED_COLOR_ID_BLUE;
		mc_led_info[3].color_index = LED_COLOR_ID_WHITE;

		mc_cdev->subled_info = mc_led_info;
		mc_cdev->num_colors = 4;

		cdev->brightness = 51;
		cdev->max_brightness = 255;
		cdev->flags = LED_CORE_SUSPENDRESUME | LED_RETAIN_AT_SHUTDOWN;
		cdev->brightness_set_blocking = gip_guide_color_led_set;
		cdev->name = devm_kasprintf(dev, GFP_KERNEL,
			"%s:rgb:power", dev_name(dev));
		if (!cdev->name)
			rc = -ENOMEM;

		if (!rc)
			rc = devm_led_classdev_multicolor_register(dev,
				&attachment->guide_led.color);

		if (rc)
			devm_kfree(dev, mc_led_info);
	} else {
		struct led_classdev *cdev = &attachment->guide_led.standard;

		cdev->max_brightness = GIP_LED_GUIDE_MAX_BRIGHTNESS;
		cdev->brightness = GIP_LED_GUIDE_INIT_BRIGHTNESS;
		cdev->flags = LED_CORE_SUSPENDRESUME | LED_RETAIN_AT_SHUTDOWN;
		cdev->brightness_set_blocking = gip_guide_led_set;
		cdev->name = devm_kasprintf(dev, GFP_KERNEL,
			"%s:white:power", dev_name(dev));
		if (!cdev->name)
			return -ENOMEM;

		rc = devm_led_classdev_register(dev,
			&attachment->guide_led.standard);
	}

	return rc;
}
#endif

static int gip_battery_get_property(struct power_supply *psy,
	enum power_supply_property psp, union power_supply_propval *val)
{
	struct gip_attachment *attachment = power_supply_get_drvdata(psy);

	guard(mutex)(&attachment->lock);
	switch (psp) {
	case POWER_SUPPLY_PROP_PRESENT:
		val->intval = attachment->status.base.battery_type != GIP_BATTERY_ABSENT;
		break;
	case POWER_SUPPLY_PROP_SCOPE:
		val->intval = POWER_SUPPLY_SCOPE_DEVICE;
		break;
	case POWER_SUPPLY_PROP_STATUS:
		if (attachment->status.base.battery_type == GIP_BATTERY_ABSENT) {
			val->intval = POWER_SUPPLY_STATUS_NOT_CHARGING;
		} else {
			switch (attachment->status.base.charge) {
			case GIP_CHARGING:
				if (attachment->status.base.battery_level == GIP_BATTERY_FULL)
					val->intval = POWER_SUPPLY_STATUS_FULL;
				else
					val->intval = POWER_SUPPLY_STATUS_CHARGING;
				break;
			case GIP_NOT_CHARGING:
				val->intval = POWER_SUPPLY_STATUS_DISCHARGING;
				break;
			case GIP_CHARGE_ERROR:
			default:
				val->intval = POWER_SUPPLY_STATUS_UNKNOWN;
				break;
			}
		}
		break;
	case POWER_SUPPLY_PROP_CAPACITY_LEVEL:
		if (attachment->status.base.battery_type == GIP_BATTERY_ABSENT) {
			val->intval = POWER_SUPPLY_CAPACITY_LEVEL_FULL;
		} else {
			switch (attachment->status.base.battery_level) {
			case GIP_BATTERY_CRITICAL:
				val->intval = POWER_SUPPLY_CAPACITY_LEVEL_CRITICAL;
				break;
			case GIP_BATTERY_LOW:
				val->intval = POWER_SUPPLY_CAPACITY_LEVEL_LOW;
				break;
			case GIP_BATTERY_MEDIUM:
				val->intval = POWER_SUPPLY_CAPACITY_LEVEL_NORMAL;
				break;
			case GIP_BATTERY_FULL:
				val->intval = POWER_SUPPLY_CAPACITY_LEVEL_FULL;
				break;
			default:
				val->intval = POWER_SUPPLY_CAPACITY_LEVEL_UNKNOWN;
				break;
			}
			break;
		}
		break;
	default:
		return -EINVAL;
	}
	return 0;
}

static int gip_battery_create(struct gip_attachment *attachment, struct device *dev)
{
	struct power_supply_config supply_config = { .drv_data = attachment, };

	attachment->battery_desc.properties = gip_battery_props;
	attachment->battery_desc.num_properties = ARRAY_SIZE(gip_battery_props);
	attachment->battery_desc.get_property = gip_battery_get_property;
	attachment->battery_desc.type = POWER_SUPPLY_TYPE_BATTERY;
	attachment->battery_desc.name = devm_kasprintf(dev, GFP_KERNEL,
		"gip-battery-%s", dev_name(dev));

	if (!attachment->battery_desc.name)
		return -ENOMEM;

	attachment->battery = devm_power_supply_register(dev,
		&attachment->battery_desc, &supply_config);
	if (IS_ERR(attachment->battery))
		return PTR_ERR(attachment->battery);

	return power_supply_powers(attachment->battery, dev);
}

static bool gip_send_set_device_state(struct gip_attachment *attachment, uint8_t state)
{
	uint8_t buffer[] = { state };

	return gip_send_system_message(attachment, GIP_CMD_SET_DEVICE_STATE,
		attachment->attachment_index, buffer, sizeof(buffer));
}

static int gip_handle_command_raw_report(struct gip_attachment *attachment,
	const struct gip_header *header, const uint8_t *bytes, int num_bytes)
{
	struct input_dev *input;

	if (num_bytes < 17) {
		dev_dbg(GIP_DEV(attachment), "Discarding too-short raw report\n");
		return -EINVAL;
	}
	guard(rcu)();
	input = rcu_dereference(attachment->input);
	if (!input)
		return -ENODEV;

	if ((attachment->features & GIP_FEATURE_ELITE_BUTTONS)
		&& attachment->xbe_format == GIP_BTN_FMT_XBE2_RAW) {
		input_report_abs(input, ABS_PROFILE, bytes[15] & 3);
		if (bytes[15] & 3) {
			input_report_key(input, BTN_GRIPL, 0);
			input_report_key(input, BTN_GRIPR, 0);
			input_report_key(input, BTN_GRIPL2, 0);
			input_report_key(input, BTN_GRIPR2, 0);
		} else {
			input_report_key(input, BTN_GRIPL,
				bytes[GIP_BTN_OFFSET_XBE2] & BIT(2));
			input_report_key(input, BTN_GRIPR,
				bytes[GIP_BTN_OFFSET_XBE2] & BIT(0));
			input_report_key(input, BTN_GRIPL2,
				bytes[GIP_BTN_OFFSET_XBE2] & BIT(3));
			input_report_key(input, BTN_GRIPR2,
				bytes[GIP_BTN_OFFSET_XBE2] & BIT(1));
		}

		input_sync(input);
	}
	return 0;
}

int gip_setup_input_device(struct gip_attachment *attachment)
{
	struct input_dev *input;
	int rc;

	if (!attachment->driver || !attachment->driver->setup_input)
		return -ENODEV;

	rcu_read_lock();
	input = rcu_dereference(attachment->input);
	rcu_read_unlock();
	if (input)
		return 0;

	input = input_allocate_device();
	if (!input)
		return -ENOMEM;
	input->id.bustype = BUS_USB;
	input->id.vendor = attachment->vendor_id;
	input->id.product = attachment->product_id;
	input->uniq = attachment->uniq;
	if (attachment->name)
		input->name = attachment->name;
	else if (attachment->attachment_index == 0)
		input->name = attachment->device->udev->product;
	input->phys = attachment->phys;

	rc = attachment->driver->setup_input(attachment, input);
	if (rc < 0)
		goto err_free_device;

	if (attachment->features & GIP_FEATURE_CONSOLE_FUNCTION_MAP)
		input_set_capability(input, EV_KEY, KEY_RECORD);

	if (attachment->features & GIP_FEATURE_ELITE_BUTTONS) {
		input_set_capability(input, EV_KEY, BTN_GRIPL);
		input_set_capability(input, EV_KEY, BTN_GRIPR);
		input_set_capability(input, EV_KEY, BTN_GRIPL2);
		input_set_capability(input, EV_KEY, BTN_GRIPR2);
		if (attachment->xbe_format == GIP_BTN_FMT_XBE1)
			input_set_abs_params(input, ABS_PROFILE, 0, 1, 0, 0);
		else
			input_set_abs_params(input, ABS_PROFILE, 0, 3, 0, 0);

		attachment->vendor_handlers[GIP_CMD_RAW_REPORT] = gip_handle_command_raw_report;
	}

#ifdef CONFIG_JOYSTICK_XBOX_GIP_FF
	if (attachment->features & GIP_FEATURE_MOTOR_CONTROL) {
		input_set_capability(input, EV_FF, FF_RUMBLE);
		input_ff_create_memless(input, NULL, gip_play_effect);
	}
#endif

	input_set_drvdata(input, attachment);
	rcu_assign_pointer(attachment->input, input);
	rc = input_register_device(input);
	if (rc)
		goto err_free_device;

#ifdef CONFIG_JOYSTICK_XBOX_GIP_LEDS
	rc = gip_guide_led_probe(attachment, &input->dev);
	if (rc)
		dev_err(GIP_DEV(attachment), "Failed to register LEDs: %d\n", rc);
#endif
	rc = gip_battery_create(attachment, &input->dev);
	if (rc)
		dev_err(GIP_DEV(attachment), "Failed to register battery: %d\n", rc);

	return 0;

err_free_device:
	input_free_device(input);
	return rc;
}

static int gip_send_init_sequence(struct gip_attachment *attachment)
{
	int rc = 0;
	size_t len;

	if (attachment->features & GIP_FEATURE_EXTENDED_SET_DEVICE_STATE) {
		/*
		 * The meaning of this packet is unknown and not documented, but it's
		 * needed for the Elite 2 controller to start up on older firmwares
		 */
		static const uint8_t set_device_state[] = {
			GIP_STATE_UNK6, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
			0x55, 0x53, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0
		};

		rc = gip_send_system_message(attachment,
			GIP_CMD_SET_DEVICE_STATE, 0, set_device_state,
			sizeof(set_device_state));
		if (rc)
			return rc;
	}
	rc = gip_enable_elite_buttons(attachment);
	if (rc)
		return rc;
	if (!gip_supports_system_message(attachment, GIP_CMD_AUDIO_CONTROL, false)) {
		rc = gip_send_set_device_state(attachment, GIP_STATE_START);
		if (rc)
			return rc;
		attachment->device_state = GIP_STATE_START;
	} else {
		rc = gip_send_set_device_state(attachment, GIP_STATE_STOP);
		if (rc)
			return rc;
		attachment->device_state = GIP_STATE_STOP;
	}

	rc = gip_send_guide_button_led(attachment,
		GIP_LED_GUIDE_ON,
		GIP_LED_GUIDE_INIT_BRIGHTNESS);
	if (rc)
		return rc;

	if (gip_supports_system_message(attachment, GIP_CMD_SECURITY, false)
		&& !(attachment->features & GIP_FEATURE_SECURITY_OPT_OUT)) {
		/* TODO: Implement Security command property */
		uint8_t buffer[] = { 0x1, 0x0 };

		rc = gip_send_system_message(attachment, GIP_CMD_SECURITY, 0,
			buffer, sizeof(buffer));
		if (rc)
			return rc;
	}

	usb_make_path(attachment->device->udev, attachment->phys,
		sizeof(attachment->phys));
	len = strlen(attachment->phys);
	if (len < sizeof(attachment->phys) - 1)
		snprintf(attachment->phys + len,
			sizeof(attachment->phys) - len, "/input%d",
			attachment->attachment_index);

	if (attachment->driver && attachment->driver->init) {
		rc = attachment->driver->init(attachment);
		if (rc < 0)
			return rc;
	}

	if (rc != GIP_INIT_NO_INPUT && (attachment->features & GIP_FEATURE_CONTROLLER)) {
		rc = gip_setup_input_device(attachment);
		if (rc == -ENODEV)
			return 0;
		if (rc)
			return rc;
	}
	rc = 0;

	if (attachment->metadata.device.hid_descriptor) {
		struct hid_device *hdev = hid_allocate_device();

		if (IS_ERR(hdev))
			return PTR_ERR(hdev);

		hdev->ll_driver = &gip_hid_ll_driver;
		hdev->bus = BUS_USB;
		hdev->vendor = attachment->vendor_id;
		hdev->product = attachment->product_id;
		hdev->dev.parent = GIP_DEV(attachment);
		hdev->driver_data = attachment;
		if (attachment->name)
			strscpy(hdev->name, attachment->name);
		else
			strscpy(hdev->name, "Xbox Chatpad");
		strscpy(hdev->phys, attachment->phys);
		rc = hid_add_device(hdev);
		if (rc) {
			dev_err(GIP_DEV(attachment), "HID device add failed: %d\n", rc);
			hid_destroy_device(hdev);
		} else {
			rcu_assign_pointer(attachment->hdev, hdev);
			synchronize_rcu();
		}
	}

	return rc;
}

static void gip_fragment_timeout(struct work_struct *work)
{
	struct gip_attachment *attachment = container_of(to_delayed_work(work),
		struct gip_attachment, fragment_timeout);

	guard(mutex)(&attachment->lock);
	devm_kfree(GIP_DEV(attachment), attachment->fragment_data);
	attachment->fragment_data = NULL;
	attachment->fragment_message = 0;
}

static void gip_retry_metadata(struct work_struct *work)
{
	struct gip_attachment *attachment = container_of(to_delayed_work(work),
		struct gip_attachment, metadata_next);

	guard(mutex)(&attachment->lock);
	if (attachment->metadata_retries < 4) {
		attachment->metadata_retries++;
		schedule_delayed_work(&attachment->metadata_next, HZ / 2);
		gip_send_system_message(attachment, GIP_CMD_METADATA, 0, NULL, 0);
	} else {
		dev_info(GIP_DEV(attachment),
			"Unable to obtain metadata, attempting to reset device\n");
		gip_send_set_device_state(attachment, GIP_STATE_RESET);
	}
}

static int gip_ensure_metadata(struct gip_attachment *attachment)
{
	switch (attachment->got_metadata) {
	case GIP_METADATA_GOT:
	case GIP_METADATA_FAKED:
		return 0;
	case GIP_METADATA_NONE:
		attachment->got_metadata = GIP_METADATA_PENDING;
		cancel_delayed_work_sync(&attachment->metadata_next);
		schedule_delayed_work(&attachment->metadata_next, HZ / 2);
		attachment->metadata_retries = 0;
		return gip_send_system_message(attachment, GIP_CMD_METADATA, 0, NULL, 0);
	default:
		return 0;
	}
}

static void gip_set_metadata_defaults(struct gip_attachment *attachment)
{
	if (attachment->got_metadata != GIP_METADATA_NONE)
		return;

	attachment->metadata.device.in_system_messages[0] =
		GIP_DEFAULT_IN_SYSTEM_MESSAGES;
	attachment->metadata.device.out_system_messages[0] =
		GIP_DEFAULT_OUT_SYSTEM_MESSAGES;
	if (attachment->attachment_index == 0) {
		/* Some decent default settings */
		attachment->features |= GIP_FEATURE_CONTROLLER;
		attachment->metadata.device.in_system_messages[0] |= (1u << GIP_CMD_GUIDE_BUTTON);
	}

	gip_handle_quirks(attachment);
	if (attachment->quirks & GIP_QUIRK_NO_HELLO)
		gip_ensure_metadata(attachment);

	attachment->got_metadata = GIP_METADATA_FAKED;
}

static bool gip_handle_command_protocol_control(struct gip_attachment *attachment,
	const struct gip_header *header, const uint8_t *bytes, int num_bytes)
{
	/* TODO */
	dev_warn(GIP_DEV(attachment), "Unimplemented Protocol Control message\n");
	return -ENOTSUPP;
}

static bool gip_handle_command_hello_device(struct gip_attachment *attachment,
	const struct gip_header *header, const uint8_t *bytes, int num_bytes)
{
	struct gip_hello_device message = {0};

	if (num_bytes != 28)
		return -EINVAL;

	message.device_id = (uint64_t) bytes[0];
	message.device_id |= (uint64_t) bytes[1] << 8;
	message.device_id |= (uint64_t) bytes[2] << 16;
	message.device_id |= (uint64_t) bytes[3] << 24;
	message.device_id |= (uint64_t) bytes[4] << 32;
	message.device_id |= (uint64_t) bytes[5] << 40;
	message.device_id |= (uint64_t) bytes[6] << 48;
	message.device_id |= (uint64_t) bytes[7] << 56;

	message.vendor_id = bytes[8];
	message.vendor_id |= bytes[9] << 8;

	message.product_id = bytes[10];
	message.product_id |= bytes[11] << 8;

	message.firmware_major_version = bytes[12];
	message.firmware_major_version |= bytes[13] << 8;

	message.firmware_minor_version = bytes[14];
	message.firmware_minor_version |= bytes[15] << 8;

	message.firmware_build_version = bytes[16];
	message.firmware_build_version |= bytes[17] << 8;

	message.firmware_revision = bytes[18];
	message.firmware_revision |= bytes[19] << 8;

	message.hardware_major_version = bytes[20];
	message.hardware_minor_version = bytes[21];

	message.rf_proto_major_version = bytes[22];
	message.rf_proto_minor_version = bytes[23];

	message.security_major_version = bytes[24];
	message.security_minor_version = bytes[25];

	message.gip_major_version = bytes[26];
	message.gip_minor_version = bytes[27];

	dev_dbg(GIP_DEV(attachment), "Device hello from %llx (%04x:%04x)\n",
		message.device_id, message.vendor_id, message.product_id);
	dev_dbg(GIP_DEV(attachment), "Firmware version %d.%d.%d rev %d\n",
		message.firmware_major_version, message.firmware_minor_version,
		message.firmware_build_version, message.firmware_revision);

	/*
	 * The GIP spec specifies that the host should reject the device if any of these are wrong.
	 * I don't know if Windows or an Xbox do, however, so let's just log warnings instead.
	 */
	if (message.rf_proto_major_version != 1 && message.rf_proto_minor_version != 0)
		dev_warn(GIP_DEV(attachment),
			"Invalid RF protocol version %d.%d, expected 1.0\n",
			message.rf_proto_major_version, message.rf_proto_minor_version);

	if (message.security_major_version != 1 && message.security_minor_version != 0)
		dev_warn(GIP_DEV(attachment),
			"Invalid security protocol version %d.%d, expected 1.0\n",
			message.security_major_version, message.security_minor_version);

	if (message.gip_major_version != 1 && message.gip_minor_version != 0)
		dev_warn(GIP_DEV(attachment),
			"Invalid GIP version %d.%d, expected 1.0\n",
			message.gip_major_version, message.gip_minor_version);

	attachment->firmware_major_version = message.firmware_major_version;
	attachment->firmware_minor_version = message.firmware_minor_version;
	attachment->vendor_id = message.vendor_id;
	attachment->product_id = message.product_id;
	attachment->uniq = devm_kasprintf(GIP_DEV(attachment),
		GFP_KERNEL, "%llx", message.device_id);

	if (header->flags & GIP_FLAG_ATTACHMENT_MASK)
		return gip_send_system_message(attachment, GIP_CMD_METADATA, 0, NULL, 0);
	if (attachment->got_metadata == GIP_METADATA_FAKED)
		attachment->got_metadata = GIP_METADATA_NONE;
	return gip_ensure_metadata(attachment);
}

static int gip_handle_command_status_device(struct gip_attachment *attachment,
	const struct gip_header *header, const uint8_t *bytes, int num_bytes)
{
	int i;

	if (num_bytes < 1)
		return -EINVAL;

	attachment->status.base.battery_level = bytes[0] & 3;
	attachment->status.base.battery_type = (bytes[0] >> 2) & 3;
	attachment->status.base.charge = (bytes[0] >> 4) & 3;
	attachment->status.base.power_level = (bytes[0] >> 6) & 3;

	if (num_bytes >= 4) {
		attachment->status.device_active = bytes[1] & 1;
		if (bytes[1] & 2) {
			/* Events present */
			if (num_bytes < 5)
				return -EINVAL;

			attachment->status.num_events = bytes[4];
			if (attachment->status.num_events > 5) {
				dev_info(GIP_DEV(attachment),
					"Device reported too many events, %d > 5\n",
					attachment->status.num_events);
				return -EINVAL;
			}
			if (5 + attachment->status.num_events * 10 > num_bytes)
				return -EINVAL;

			for (i = 0; i < attachment->status.num_events; i++) {
				struct gip_status_event *event = &attachment->status.events[i];

				event->event_type = bytes[i * 10 + 5];
				event->event_type |= bytes[i * 10 + 6] << 8;
				event->fault_tag = bytes[i * 10 + 7];
				event->fault_tag |= bytes[i * 10 + 8] << 8;
				event->fault_tag |= bytes[i * 10 + 9] << 16;
				event->fault_tag |= bytes[i * 10 + 10] << 24;
				event->fault_address = bytes[i * 10 + 11];
				event->fault_address |= bytes[i * 10 + 12] << 8;
				event->fault_address |= bytes[i * 10 + 13] << 16;
				event->fault_address |= bytes[i * 10 + 14] << 24;

				dev_info(GIP_DEV(attachment),
					"Attachment %i event type %i, tag %i address %x\n",
					attachment->attachment_index,
					event->event_type,
					event->fault_tag,
					event->fault_address);
			}
		}
	}

	return gip_ensure_metadata(attachment);
}

static int gip_handle_command_metadata_respose(struct gip_attachment *attachment,
	const struct gip_header *header, const uint8_t *bytes, int num_bytes)
{
	struct gip_metadata metadata = {0};
	const guid_t *expected_guid = NULL;
	bool found_expected_guid;
	bool found_controller_guid = false;
	int i, j, k;
	int rc;

	rc = gip_parse_metadata(GIP_DEV(attachment), &metadata, bytes, num_bytes);
	if (rc)
		return rc;

	if (attachment->got_metadata == GIP_METADATA_GOT) {
		struct input_dev *input;

		gip_metadata_free(GIP_DEV(attachment), &attachment->metadata);
		rcu_read_lock();
		input = rcu_dereference(attachment->input);
		rcu_read_unlock();
		if (input) {
			rcu_assign_pointer(attachment->input, NULL);
			synchronize_rcu();
			input_unregister_device(input);
		}
	}

	attachment->metadata = metadata;
	attachment->got_metadata = GIP_METADATA_GOT;
	attachment->features = 0;
	cancel_delayed_work_sync(&attachment->metadata_next);

	for (i = 0; i < metadata.device.num_preferred_types; i++) {
		const char *type = metadata.device.preferred_types[i];

		dev_dbg(GIP_DEV(attachment), "Device preferred type: %s\n",
			type);
	}
	for (i = 0; i < metadata.device.num_preferred_types; i++) {
		const char *type = metadata.device.preferred_types[i];

		for (j = 0; base_drivers[j] && !expected_guid; j++) {
			for (k = 0; base_drivers[j]->types[k] && !expected_guid; k++) {
				if (strcmp(type, base_drivers[j]->types[k]) == 0) {
					rc = gip_bind_driver(attachment, base_drivers[j]);
					if (rc == 0)
						expected_guid = &base_drivers[j]->guid;
					else if (rc != -ENODEV)
						return rc;
				}
			}
		}
		if (expected_guid)
			break;

		if (strcmp(type, "Windows.Xbox.Input.Chatpad") == 0) {
			break;
		}
		if (strcmp(type, "Windows.Xbox.Input.Headset") == 0) {
			expected_guid = &guid_headset;
			break;
		}
	}

	found_expected_guid = !expected_guid;
	for (i = 0; i < metadata.device.num_supported_interfaces; i++) {
		const guid_t *guid = &metadata.device.supported_interfaces[i];

		dev_dbg(GIP_DEV(attachment), "Supported interface: %pUl\n", guid);
		if (expected_guid && guid_equal(expected_guid, guid))
			found_expected_guid = true;

		if (guid_equal(&guid_controller, guid)) {
			found_controller_guid = true;
			continue;
		}
		if (guid_equal(&gip_driver_navigation.guid, guid)) {
			attachment->features |= GIP_FEATURE_CONTROLLER;
			continue;
		}
		if (guid_equal(&guid_dev_auth_pc_opt_out, guid)) {
			attachment->features |= GIP_FEATURE_SECURITY_OPT_OUT;
			continue;
		}
		if (guid_equal(&guid_console_function_map, guid)) {
			attachment->features |= GIP_FEATURE_CONSOLE_FUNCTION_MAP;
			continue;
		}
		if (guid_equal(&guid_console_function_map_overflow, guid)) {
			attachment->features |= GIP_FEATURE_CONSOLE_FUNCTION_MAP_OVERFLOW;
			continue;
		}
		if (guid_equal(&guid_elite_buttons, guid)) {
			attachment->features |= GIP_FEATURE_ELITE_BUTTONS;
			continue;
		}
		if (guid_equal(&guid_dynamic_latency_input, guid)) {
			attachment->features |= GIP_FEATURE_DYNAMIC_LATENCY_INPUT;
			continue;
		}
	}

	for (i = 0; i < metadata.num_messages; i++) {
		struct gip_message_metadata *message = &metadata.message_metadata[i];

		if (message->type == GIP_CMD_DIRECT_MOTOR && message->length >= 9
			&& (message->flags & GIP_MESSAGE_FLAG_DOWNSTREAM))
			attachment->features |= GIP_FEATURE_MOTOR_CONTROL;
	}

	if (!found_expected_guid || !found_controller_guid)
		dev_dbg(GIP_DEV(attachment),
			"Controller was missing expected GUID. "
			"This controller probably won't work on an actual Xbox.\n");

	gip_handle_quirks(attachment);

	if ((attachment->features & GIP_FEATURE_GUIDE_COLOR)
		&& !gip_supports_vendor_message(attachment,
			GIP_CMD_GUIDE_COLOR, false))
		attachment->features &= ~GIP_FEATURE_GUIDE_COLOR;

	dev_dbg(GIP_DEV(attachment),
		"Attachment %i has features: %02x\n",
		attachment->attachment_index, attachment->features);

	return gip_send_init_sequence(attachment);
}

static int gip_handle_command_security(struct gip_attachment *attachment,
	const struct gip_header *header, const uint8_t *bytes, int num_bytes)
{
	/* TODO: Needed for controllers that connect via dongles */
	dev_warn(GIP_DEV(attachment), "Unimplemented Security message\n");
	return -ENOTSUPP;
}

static int gip_handle_command_guide_button_status(struct gip_attachment *attachment,
	const struct gip_header *header, const uint8_t *bytes, int num_bytes)
{
	struct input_dev *input;

	if (num_bytes < 2)
		return -EINVAL;

	guard(rcu)();
	input = rcu_dereference(attachment->input);
	if (!input)
		return -ENODEV;

	if (bytes[1] == VK_LWIN) {
		input_report_key(input, BTN_MODE, bytes[0] & 3);
		input_sync(input);
	}

	return 0;
}

static int gip_handle_command_audio_control(struct gip_attachment *attachment,
	const struct gip_header *header, const uint8_t *bytes, int num_bytes)
{
	/* TODO: Needed for audio */
	dev_warn(GIP_DEV(attachment), "Unimplemented Audio Control message\n");
	return -ENOTSUPP;
}

static int gip_handle_command_firmware(struct gip_attachment *attachment,
	const struct gip_header *header, const uint8_t *bytes, int num_bytes)
{
	if (num_bytes < 1)
		return -EINVAL;

	if (bytes[0] == 1) {
		uint16_t major, minor, build, rev;

		if (num_bytes < 14) {
			dev_dbg(GIP_DEV(attachment),
				"Discarding too-short firmware message\n");

			return -EINVAL;
		}
		major = bytes[6];
		major |= bytes[7] << 8;
		minor = bytes[8];
		minor |= bytes[9] << 8;
		build = bytes[10];
		build |= bytes[11] << 8;
		rev = bytes[12];
		rev |= bytes[13] << 8;

		dev_dbg(GIP_DEV(attachment),
			"Firmware version: %d.%d.%d rev %d\n", major, minor, build, rev);

		attachment->firmware_major_version = major;
		attachment->firmware_minor_version = minor;

		if (attachment->vendor_id == 0x045e
			&& attachment->product_id == 0x0b00)
			return gip_enable_elite_buttons(attachment);

		return 0;
	}

	dev_warn(GIP_DEV(attachment), "Unimplemented Firmware message\n");

	return -ENOTSUPP;
}

static int gip_handle_command_hid_report(struct gip_attachment *attachment,
	const struct gip_header *header, uint8_t *bytes, int num_bytes)
{
	struct hid_device *hdev;

	guard(rcu)();
	hdev = rcu_dereference(attachment->hdev);
	if (hdev)
		return hid_input_report(hdev, HID_INPUT_REPORT, bytes, num_bytes, true);

	dev_warn(GIP_DEV(attachment), "Got HID report with no HID descriptor\n");

	return -EINVAL;
}

static int gip_handle_command_extended(struct gip_attachment *attachment,
	const struct gip_header *header, const uint8_t *bytes, int num_bytes)
{
	if (num_bytes < 2)
		return -EINVAL;

	if (bytes[1] != GIP_EXTENDED_STATUS_OK) {
		dev_dbg(GIP_DEV(attachment),
			"Extended message type %02x failed with status %i\n",
			bytes[0], bytes[1]);
		return -EPROTO;
	}

	switch (bytes[0]) {
	case GIP_EXTCMD_GET_SERIAL_NUMBER:
		memcpy(attachment->serial, &bytes[2],
			min(sizeof(attachment->serial), (size_t)(num_bytes - 2)));
		break;
	default:
		/* TODO */
		dev_dbg(GIP_DEV(attachment), "Unimplemented extended message type %02x\n",
			bytes[0]);
		return -ENOTSUPP;
	}

	return 0;
}

static int gip_handle_elite_buttons(struct gip_attachment *attachment,
	struct input_dev *input, const uint8_t *bytes, int num_bytes)
{
	bool grip[4] = { 0, 0, 0, 0 };
	int profile = -1;

	if (attachment->xbe_format == GIP_BTN_FMT_XBE1
		&& num_bytes > GIP_BTN_OFFSET_XBE1) {
		profile = bytes[GIP_BTN_OFFSET_XBE1] >> 4;
		if (profile) {
			grip[0] = bytes[GIP_BTN_OFFSET_XBE1] & BIT(0);
			grip[1] = bytes[GIP_BTN_OFFSET_XBE1] & BIT(1);
			grip[2] = bytes[GIP_BTN_OFFSET_XBE1] & BIT(2);
			grip[3] = bytes[GIP_BTN_OFFSET_XBE1] & BIT(3);
		}
	} else if ((attachment->xbe_format == GIP_BTN_FMT_XBE2_4
		|| attachment->xbe_format == GIP_BTN_FMT_XBE2_5)
		&& num_bytes > GIP_BTN_OFFSET_XBE2) {
		int profile_offset;

		if (attachment->xbe_format == GIP_BTN_FMT_XBE2_4)
			profile_offset = 15;
		else
			profile_offset = 20;
		profile = bytes[profile_offset] & 3;

		if (!profile) {
			grip[0] = bytes[GIP_BTN_OFFSET_XBE2] & BIT(2);
			grip[1] = bytes[GIP_BTN_OFFSET_XBE2] & BIT(0);
			grip[2] = bytes[GIP_BTN_OFFSET_XBE2] & BIT(3);
			grip[3] = bytes[GIP_BTN_OFFSET_XBE2] & BIT(1);
		}
	}
	if (profile >= 0) {
		input_report_key(input, BTN_GRIPL, grip[0]);
		input_report_key(input, BTN_GRIPR, grip[1]);
		input_report_key(input, BTN_GRIPL2, grip[2]);
		input_report_key(input, BTN_GRIPR2, grip[3]);
		input_report_abs(input, ABS_PROFILE, profile);
	}
	return 0;
}

static int gip_handle_console_map(struct gip_attachment *attachment,
	struct input_dev *input, const uint8_t *bytes, int num_bytes)
{
	int function_map_offset = -1;
	if (num_bytes < 32)
		return 0;

	if (attachment->features & GIP_FEATURE_DYNAMIC_LATENCY_INPUT) {
		/* The dynamic latency input bytes are after the console function map */
		if (num_bytes >= 40)
			function_map_offset = num_bytes - 26;
	} else {
		function_map_offset = num_bytes - 18;
	}
	if (function_map_offset >= 14) {
		input_report_key(input, KEY_RECORD,
			bytes[function_map_offset] & BIT(0));
	}
	return 0;
}

static int gip_handle_ll_input_report(struct gip_attachment *attachment,
	const struct gip_header *header, const uint8_t *bytes, int num_bytes)
{
	struct input_dev *input;
	int rc = 0;

	guard(rcu)();
	input = rcu_dereference(attachment->input);
	if (!input)
		return -ENODEV;

	if (attachment->device_state != GIP_STATE_START) {
		dev_dbg(GIP_DEV(attachment), "Discarding early input report\n");
		attachment->device_state = GIP_STATE_START;
		return 0;
	}

	if (attachment->driver && attachment->driver->handle_input_report) {
		rc = attachment->driver->handle_input_report(attachment, input, bytes, num_bytes);
		if (rc < 0)
			return rc;
	}

	if (attachment->features & GIP_FEATURE_ELITE_BUTTONS) {
		rc = gip_handle_elite_buttons(attachment, input, bytes, num_bytes);
		if (rc < 0)
			goto exit;
	}

	rc = gip_handle_console_map(attachment, input, bytes, num_bytes);

exit:
	input_sync(input);

	return rc;
}

static int gip_handle_ll_overflow_input_report(struct gip_attachment *attachment,
	const struct gip_header *header, const uint8_t *bytes, int num_bytes)
{
	/* TODO: Unknown if any devices actually use this */
	dev_dbg(GIP_DEV(attachment), "Unimplemented Overflow Input Report message\n");
	return -ENOTSUPP;
}

static int gip_handle_audio_data(struct gip_attachment *attachment,
	const struct gip_header *header, const uint8_t *bytes, int num_bytes)
{
	/* TODO: Needed for audio support */
	dev_dbg(GIP_DEV(attachment), "Unimplemented Audio Data message\n");
	return -ENOTSUPP;
}

static int gip_handle_system_message(struct gip_attachment *attachment,
	const struct gip_header *header, uint8_t *bytes, int num_bytes)
{
	if (!gip_supports_system_message(attachment, header->message_type, true)) {
		dev_warn(GIP_DEV(attachment),
			"Received claimed-unsupported system message type %02x\n",
			header->message_type);
		return -EINVAL;
	}
	switch (header->message_type) {
	case GIP_CMD_PROTO_CONTROL:
		return gip_handle_command_protocol_control(attachment, header,
			bytes, num_bytes);
	case GIP_CMD_HELLO_DEVICE:
		return gip_handle_command_hello_device(attachment, header,
			bytes, num_bytes);
	case GIP_CMD_STATUS_DEVICE:
		return gip_handle_command_status_device(attachment, header,
			bytes, num_bytes);
	case GIP_CMD_METADATA:
		return gip_handle_command_metadata_respose(attachment, header,
			bytes, num_bytes);
	case GIP_CMD_SECURITY:
		return gip_handle_command_security(attachment, header, bytes,
			num_bytes);
	case GIP_CMD_GUIDE_BUTTON:
		return gip_handle_command_guide_button_status(attachment,
			header, bytes, num_bytes);
	case GIP_CMD_AUDIO_CONTROL:
		return gip_handle_command_audio_control(attachment, header,
			bytes, num_bytes);
	case GIP_CMD_FIRMWARE:
		return gip_handle_command_firmware(attachment, header, bytes,
			num_bytes);
	case GIP_CMD_HID_REPORT:
		return gip_handle_command_hid_report(attachment, header,
			bytes, num_bytes);
	case GIP_CMD_EXTENDED:
		return gip_handle_command_extended(attachment, header, bytes,
			num_bytes);
	case GIP_AUDIO_DATA:
		return gip_handle_audio_data(attachment, header, bytes,
			num_bytes);
	default:
		dev_warn(GIP_DEV(attachment),
			"Received unknown system message type %02x\n",
			header->message_type);
		return -EINVAL;
	}
}

static struct gip_attachment *gip_ensure_attachment(struct gip_device *device,
	uint8_t attachment_index)
{
	struct gip_attachment *attachment = device->attachments[attachment_index];

	if (!attachment) {
		attachment = devm_kzalloc(GIP_DEV(device),
			sizeof(*attachment), GFP_KERNEL);
		if (!attachment)
			return ERR_PTR(-ENOMEM);

		attachment->attachment_index = attachment_index;
		attachment->device = device;

		if (attachment_index == 0) {
			attachment->vendor_id = device->udev->descriptor.idVendor;
			attachment->product_id = device->udev->descriptor.idProduct;
		}

		device->attachments[attachment_index] = attachment;

		mutex_init(&attachment->lock);
		INIT_DELAYED_WORK(&attachment->fragment_timeout, gip_fragment_timeout);
		INIT_DELAYED_WORK(&attachment->metadata_next, gip_retry_metadata);

		gip_set_metadata_defaults(attachment);
	}
	return attachment;
}

static int gip_handle_message(struct gip_attachment *attachment,
	const struct gip_header *header, uint8_t *bytes, int num_bytes)
{
	if (header->flags & GIP_FLAG_SYSTEM)
		return gip_handle_system_message(attachment, header, bytes,
			num_bytes);

	if (header->message_type < MAX_GIP_CMD && attachment->vendor_handlers[header->message_type])
		return attachment->vendor_handlers[header->message_type](attachment,
			header, bytes, num_bytes);

	switch (header->message_type) {
	case GIP_LL_INPUT_REPORT:
		return gip_handle_ll_input_report(attachment, header, bytes,
			num_bytes);
	case GIP_LL_OVERFLOW_INPUT_REPORT:
		return gip_handle_ll_overflow_input_report(attachment, header,
			bytes, num_bytes);
	}
	dev_warn(GIP_DEV(attachment),
		"Received unknown vendor message type %02x\n",
		header->message_type);
	return -ENOTSUPP;
}

static int gip_receive_fragment(struct gip_attachment *attachment,
	const struct gip_header *header, int offset,
	uint64_t *fragment_offset, uint16_t *bytes_remaining, uint8_t *bytes,
	int num_bytes)
{
	int rc = 0;

	if (header->flags & GIP_FLAG_INIT_FRAG) {
		uint64_t total_length;

		if (attachment->fragment_message) {
			/*
			 * Reset fragment buffer if we get a new initial
			 * fragment before finishing the last message.
			 * TODO: Is this the correct behavior?
			 */
			devm_kfree(GIP_DEV(attachment), attachment->fragment_data);
			attachment->fragment_data = NULL;
		}
		offset += gip_decode_length(&total_length, &bytes[offset],
			num_bytes - offset);
		if (total_length > MAX_MESSAGE_LENGTH)
			return -EINVAL;

		attachment->total_length = total_length;
		attachment->fragment_message = header->message_type;
		if (header->length > num_bytes - offset) {
			dev_warn(GIP_DEV(attachment),
				"Received fragment that claims to be %llu bytes, expected %i\n",
				header->length, num_bytes - offset);
			return -EINVAL;
		}
		if (header->length > total_length) {
			dev_warn(GIP_DEV(attachment),
				"Received too long fragment, %llu bytes, exceeds %d\n",
				header->length, attachment->total_length);
			return -EINVAL;
		}
		attachment->fragment_data = devm_kmalloc(GIP_DEV(attachment),
			attachment->total_length, GFP_KERNEL);
		if (!attachment->fragment_data)
			return -ENOMEM;
		memcpy(attachment->fragment_data, &bytes[offset],
			header->length);
		*fragment_offset = header->length;
		attachment->fragment_offset = header->length;
		*bytes_remaining = attachment->total_length - header->length;
	} else {
		if (header->message_type != attachment->fragment_message) {
			dev_warn(GIP_DEV(attachment),
				"Received out of sequence message type %02x, expected %02x\n",
				header->message_type, attachment->fragment_message);
			gip_fragment_failed(attachment, header);
			return -EINVAL;
		}

		offset += gip_decode_length(fragment_offset, &bytes[offset],
			num_bytes - offset);
		if (*fragment_offset != attachment->fragment_offset) {
			dev_warn(GIP_DEV(attachment),
				"Received out of sequence fragment, (claimed %llu, expected %d)\n",
				*fragment_offset, attachment->fragment_offset);
			gip_acknowledge(attachment->device, header,
				attachment->fragment_offset,
				attachment->total_length - attachment->fragment_offset);
			return -EINVAL;
		} else if (*fragment_offset + header->length > attachment->total_length) {
			dev_warn(GIP_DEV(attachment),
				"Received too long fragment, %llu exceeds %d\n",
				*fragment_offset + header->length, attachment->total_length);
			gip_fragment_failed(attachment, header);
			return -EINVAL;
		}

		*bytes_remaining = attachment->total_length -
			(*fragment_offset + header->length);
		if (header->length != 0) {
			memcpy(&attachment->fragment_data[*fragment_offset],
				&bytes[offset], header->length);
		} else {
			rc = gip_handle_message(attachment, header,
				attachment->fragment_data,
				attachment->total_length);
			devm_kfree(GIP_DEV(attachment), attachment->fragment_data);
			attachment->fragment_data = NULL;
			attachment->fragment_message = 0;
		}
		*fragment_offset += header->length;
		attachment->fragment_offset = *fragment_offset;
	}
	cancel_delayed_work_sync(&attachment->fragment_timeout);
	schedule_delayed_work(&attachment->fragment_timeout, HZ);

	return rc;
}

static int gip_receive_message(struct gip_device *device, uint8_t *bytes,
	int num_bytes)
{
	struct gip_header header;
	int offset = 3;
	int rc = 0;
	uint64_t fragment_offset = 0;
	uint16_t bytes_remaining = 0;
	bool is_fragment;
	uint8_t attachment_index;
	struct gip_attachment *attachment;

	if (num_bytes < 5)
		return -EINVAL;

	header.message_type = bytes[0];
	header.flags = bytes[1];
	header.sequence_id = bytes[2];
	offset += gip_decode_length(&header.length, &bytes[offset], num_bytes - offset);

	is_fragment = header.flags & GIP_FLAG_FRAGMENT;
	attachment_index = header.flags & GIP_FLAG_ATTACHMENT_MASK;
	attachment = gip_ensure_attachment(device, attachment_index);

	print_hex_dump_debug(KBUILD_MODNAME ": Received message: ", DUMP_PREFIX_OFFSET,
		16, 1, bytes, num_bytes, false);

	guard(mutex)(&attachment->lock);
	/* Handle coalescing fragmented messages */
	if (is_fragment) {
		rc = gip_receive_fragment(attachment, &header, offset,
			&fragment_offset, &bytes_remaining, bytes, num_bytes);
	} else if (header.length + offset > num_bytes) {
		dev_warn(GIP_DEV(device),
			"Received message with erroneous length (claimed %llu, actual %d), discarding\n",
			header.length + offset, num_bytes);
		rc = -EINVAL;
	} else {
		num_bytes -= offset;
		bytes += offset;
		fragment_offset = header.length;
		rc = gip_handle_message(attachment, &header, bytes, num_bytes);
	}

	if (!rc && (header.flags & GIP_FLAG_ACME))
		gip_acknowledge(device, &header, fragment_offset, bytes_remaining);

	return rc;
}

static void gip_receive_work(struct work_struct *work)
{
	struct gip_device *device = container_of(work, struct gip_device,
		receive_message);
	unsigned long flags;

	spin_lock_irqsave(&device->message_lock, flags);
	while (device->pending_in_messages) {
		struct gip_raw_message *message = &device->in_queue[device->next_in_message];

		spin_unlock_irqrestore(&device->message_lock, flags);

		gip_receive_message(device, message->bytes, message->num_bytes);

		spin_lock_irqsave(&device->message_lock, flags);
		device->next_in_message = (device->next_in_message + 1) % MAX_IN_MESSAGES;
		device->pending_in_messages--;
	}
	spin_unlock_irqrestore(&device->message_lock, flags);
}

static void gip_urb_in(struct urb *urb)
{
	struct gip_interface *intf = urb->context;
	struct gip_device *gip = intf->device;
	struct device *dev = &intf->intf->dev;
	int status = urb->status;
	int message_id;
	struct gip_raw_message *message;
	unsigned long flags;

	switch (status) {
	case 0:
		/* success */
		break;
	case -ECONNRESET:
	case -ENOENT:
	case -ESHUTDOWN:
		/* this urb is terminated, clean up */
		dev_dbg(dev, "%s - urb shutting down with status: %d\n",
			__func__, status);
		return;
	default:
		dev_dbg(dev, "%s - urb has status of: %d\n",
			__func__, status);
		goto exit;
	}
	if (intf->isoc_messages) {
		/* TODO: Needed for audio support */
		dev_warn(GIP_DEV(gip), "Unimplemented isochronous message input\n");
		goto exit;
	}

	spin_lock_irqsave(&gip->message_lock, flags);
	if (gip->pending_in_messages >= MAX_IN_MESSAGES) {
		dev_err(GIP_DEV(gip), "Input queue is full; dropping message\n");
	} else {
		message_id = (gip->next_in_message + gip->pending_in_messages) % MAX_IN_MESSAGES;
		message = &gip->in_queue[message_id];
		gip->pending_in_messages++;
		memcpy(message->bytes, intf->in_data, urb->actual_length);
		message->num_bytes = urb->actual_length;
	}
	spin_unlock_irqrestore(&gip->message_lock, flags);
	schedule_work(&gip->receive_message);

exit:
	status = usb_submit_urb(urb, GFP_ATOMIC);
	if (status)
		dev_err(dev, "%s - usb_submit_urb failed with result %d\n",
			__func__, status);
}

static void gip_urb_out(struct urb *urb)
{
	struct gip_interface *intf = urb->context;
	struct device *dev = &intf->intf->dev;
	int status = urb->status;

	guard(spinlock_irqsave)(&intf->device->message_lock);

	switch (status) {
	case 0:
		/* success */
		break;

	case -ECONNRESET:
	case -ENOENT:
	case -ESHUTDOWN:
		/* this urb is terminated, clean up */
		dev_dbg(dev, "%s - urb shutting down with status: %d\n",
			__func__, status);
		break;

	default:
		dev_dbg(dev, "%s - nonzero urb status received: %d\n",
			__func__, status);
		break;
	}
}

static int gip_init_input(struct gip_interface *intf,
	struct usb_endpoint_descriptor *ep_in)
{
	int error;
	struct usb_device *udev = interface_to_usbdev(intf->intf);

	intf->urb_in = usb_alloc_urb(intf->isoc_messages, GFP_KERNEL);
	if (!intf->urb_in)
		return -ENOMEM;

	intf->in_data = usb_alloc_coherent(udev, intf->mtu, GFP_KERNEL,
		&intf->urb_in->transfer_dma);

	if (!intf->in_data) {
		return -ENOMEM;
		goto err_free_urb;
	}

	usb_fill_int_urb(intf->urb_in, udev,
		usb_rcvintpipe(udev, ep_in->bEndpointAddress),
		intf->in_data, intf->mtu, gip_urb_in, intf,
		ep_in->bInterval);
	intf->urb_in->transfer_flags |= URB_NO_TRANSFER_DMA_MAP;

	if (intf->isoc_messages)
		intf->urb_in->transfer_flags |= URB_ISO_ASAP;

	return 0;

err_free_urb:
	usb_free_urb(intf->urb_in);
	intf->urb_in = NULL;

	return error;
}

static int gip_init_output(struct gip_interface *intf,
	struct usb_endpoint_descriptor *ep_out)
{
	int error;
	struct usb_device *udev = interface_to_usbdev(intf->intf);
	int i;

	if (usb_ifnum_to_if(udev, GIP_WIRED_INTF_AUDIO)) {
		/*
		 * Explicitly disable the audio interface. This is needed
		 * for some controllers, such as the PowerA Enhanced Wired
		 * Controller for Series X|S (0x20d6:0x200e) to report the
		 * guide button.
		 */
		error = usb_set_interface(udev, GIP_WIRED_INTF_AUDIO, 0);
		if (error)
			dev_warn(GIP_DEV(intf),
				"unable to disable audio interface: %d\n",
				error);
	}

	init_usb_anchor(&intf->out_anchor);

	for (i = 0; i < MAX_OUT_MESSAGES; i++) {
		intf->out_queue[i].urb = usb_alloc_urb(intf->isoc_messages, GFP_KERNEL);
		if (!intf->out_queue[i].urb) {
			error = -ENOMEM;
			goto err_free_urbs;
		}

		intf->out_queue[i].data = usb_alloc_coherent(udev, intf->mtu, GFP_KERNEL,
			&intf->out_queue[i].urb->transfer_dma);

		if (!intf->out_queue[i].data) {
			return -ENOMEM;
			goto err_free_urbs;
		}

		usb_fill_int_urb(intf->out_queue[i].urb, udev,
			usb_sndintpipe(udev, ep_out->bEndpointAddress),
			intf->out_queue[i].data, intf->mtu, gip_urb_out, intf,
			ep_out->bInterval);
		intf->out_queue[i].urb->transfer_flags |= URB_NO_TRANSFER_DMA_MAP;

		if (intf->isoc_messages)
			intf->out_queue[i].urb->transfer_flags |= URB_ISO_ASAP;
	}

	return 0;

err_free_urbs:
	for (i = 0; i < MAX_OUT_MESSAGES; i++) {
		if (intf->out_queue[i].data)
			usb_free_coherent(udev, intf->mtu, intf->out_queue[i].data,
				intf->out_queue[i].urb->transfer_dma);
		if (intf->out_queue[i].urb) {
			usb_free_urb(intf->out_queue[i].urb);
			intf->out_queue[i].urb = NULL;
		}
	}
	return error;
}

static void gip_deinit_output(struct gip_interface *intf)
{
	int i;

	for (i = 0; i < MAX_OUT_MESSAGES; i++) {
		if (!intf->out_queue[i].urb)
			continue;
		usb_free_coherent(interface_to_usbdev(intf->intf), intf->mtu,
			intf->out_queue[i].data, intf->out_queue[i].urb->transfer_dma);
		usb_free_urb(intf->out_queue[i].urb);
		intf->out_queue[i].data = NULL;
		intf->out_queue[i].urb = NULL;
	}
}

static void gip_deinit_input(struct gip_interface *intf)
{
	usb_free_coherent(interface_to_usbdev(intf->intf), intf->mtu,
		intf->in_data, intf->urb_in->transfer_dma);
	usb_free_urb(intf->urb_in);
	intf->urb_in = NULL;
}

static int gip_interface_init(struct gip_interface *intf)
{
	struct usb_endpoint_descriptor *ep_in = NULL;
	struct usb_endpoint_descriptor *ep_out = NULL;
	int error = usb_find_common_endpoints(intf->intf->cur_altsetting,
		NULL, NULL, &ep_in, &ep_out);

	if (error)
		return error;

	if (!ep_in || !ep_out)
		return -ENODEV;

	error = gip_init_input(intf, ep_in);
	if (error)
		return error;

	error = gip_init_output(intf, ep_out);
	if (error)
		goto err_free_input;

	if (usb_submit_urb(intf->urb_in, GFP_KERNEL)) {
		error = -EIO;
		goto err_free_output;
	}

	return 0;

err_free_output:
	gip_deinit_output(intf);
err_free_input:
	gip_deinit_input(intf);
	return error;
}

static int gip_probe(struct usb_interface *intf, const struct usb_device_id *id)
{
	struct usb_device *udev = interface_to_usbdev(intf);
	struct gip_device *gip = NULL;
	struct gip_attachment *attachment;
	int rc;

	if (intf->cur_altsetting->desc.bInterfaceNumber != GIP_WIRED_INTF_DATA) {
		/*
		 * The Xbox One controller lists three interfaces all with the
		 * same interface class, subclass and protocol. Differentiate by
		 * interface number.
		 */
		return 0;
	}

	gip = devm_kzalloc(&udev->dev, sizeof(*gip), GFP_KERNEL);
	if (!gip)
		return -ENOMEM;

	gip->udev = udev;
	gip->data.device = gip;
	gip->data.intf = intf;
	gip->data.mtu = BASE_GIP_MTU;
	gip->audio.device = gip;
	gip->audio.mtu = MAX_GIP_MTU;
	gip->audio.isoc_messages = MAX_AUDIO_MESSAGES;

	INIT_WORK(&gip->receive_message, gip_receive_work);
	spin_lock_init(&gip->message_lock);

	rc = gip_interface_init(&gip->data);
	if (rc) {
		devm_kfree(GIP_DEV(gip), gip);
		return rc;
	}
	/* Don't init audio interface -- we aren't using it yet */

	usb_set_intfdata(intf, gip);

	/* Pre-create the first attachment, as it should always exist */
	attachment = gip_ensure_attachment(gip, 0);
	if (IS_ERR(attachment))
		return PTR_ERR(attachment);

	return 0;
}

static int gip_shutdown(struct gip_device *device)
{
	int i;

	cancel_work_sync(&device->receive_message);

	for (i = 0; i < MAX_ATTACHMENTS; i++) {
		struct gip_attachment *attachment = device->attachments[i];
		struct input_dev *input;
		struct hid_device *hdev;

		if (!attachment)
			continue;

		scoped_guard (mutex, &attachment->lock) {
			cancel_delayed_work_sync(&attachment->metadata_next);
			cancel_delayed_work_sync(&attachment->fragment_timeout);

			rcu_read_lock();
			input = rcu_dereference(attachment->input);
			hdev = rcu_dereference(attachment->hdev);
			rcu_read_unlock();

			attachment->battery = NULL;
			rcu_assign_pointer(attachment->input, NULL);
			rcu_assign_pointer(attachment->hdev, NULL);
			synchronize_rcu();
		}

		if (input)
			input_unregister_device(input);

		if (hdev)
			hid_destroy_device(hdev);
	}

	return 0;
}

static void gip_disconnect(struct usb_interface *intf)
{
	struct gip_device *gip = usb_get_intfdata(intf);
	unsigned long flags;
	int i;

	if (!gip)
		return;

	usb_kill_urb(gip->data.urb_in);
	if (gip->audio.intf)
		usb_kill_urb(gip->audio.urb_in);

	gip_shutdown(gip);

	spin_lock_irqsave(&gip->message_lock, flags);
	gip_deinit_input(&gip->data);
	gip_deinit_output(&gip->data);
	if (gip->audio.intf) {
		gip_deinit_input(&gip->audio);
		gip_deinit_output(&gip->audio);
	}
	spin_unlock_irqrestore(&gip->message_lock, flags);

	usb_set_intfdata(intf, NULL);

	for (i = 0; i < MAX_ATTACHMENTS; i++) {
		struct gip_attachment *attachment = gip->attachments[i];

		if (!attachment)
			continue;
		devm_kfree(GIP_DEV(attachment), attachment->uniq);
		devm_kfree(GIP_DEV(attachment), attachment);
	}

	devm_kfree(GIP_DEV(gip), gip);
}

static int gip_suspend(struct usb_interface *intf, pm_message_t message)
{
	struct gip_device *gip = usb_get_intfdata(intf);

	if (!gip)
		return 0;

	usb_kill_urb(gip->data.urb_in);
	if (gip->audio.intf)
		usb_kill_urb(gip->audio.urb_in);

	if (gip->attachments[0]) {
		struct gip_attachment *attachment = gip->attachments[0];

		guard(mutex)(&attachment->lock);
		gip_send_set_device_state(attachment, GIP_STATE_OFF);
		attachment->device_state = GIP_STATE_OFF;
	}

	return gip_shutdown(gip);
}

static int gip_resume(struct usb_interface *intf)
{
	struct gip_device *gip = usb_get_intfdata(intf);

	if (!gip)
		return 0;

	if (usb_submit_urb(gip->data.urb_in, GFP_KERNEL))
		return -EIO;

	return 0;
}

/* The Xbox One controller uses subclass 71 and protocol 208. */
#define GIP_VENDOR(vend) \
	{ \
		.match_flags = USB_DEVICE_ID_MATCH_VENDOR | USB_DEVICE_ID_MATCH_INT_INFO, \
		.idVendor = (vend), \
		.bInterfaceClass = USB_CLASS_VENDOR_SPEC, \
		.bInterfaceSubClass = 71, \
		.bInterfaceProtocol = 208 \
	}

static const struct usb_device_id gip_table[] = {
	/*
	 * Please keep this list sorted by vendor ID.
	 */
	GIP_VENDOR(0x03f0),		/* HP/HyperX */
	GIP_VENDOR(0x044f),		/* ThrustMaster */
	GIP_VENDOR(0x045e),		/* Microsoft */
	GIP_VENDOR(0x046d),		/* Logitech */
	GIP_VENDOR(0x0738),		/* Mad Catz */
	GIP_VENDOR(0x0b05),		/* ASUS */
	GIP_VENDOR(0x0e6f),		/* PDP */
	GIP_VENDOR(0x0f0d),		/* Hori */
	GIP_VENDOR(0x10f5),		/* Turtle Beach */
	GIP_VENDOR(0x1532),		/* Razer */
	GIP_VENDOR(0x20d6),		/* PowerA/BDA */
	GIP_VENDOR(0x24c6),		/* PowerA/BDA/ThrustMaster */
	GIP_VENDOR(0x294b),		/* Snakebyte */
	GIP_VENDOR(0x2dc8),		/* 8BitDo */
	GIP_VENDOR(0x2e24),		/* Hyperkin */
	GIP_VENDOR(0x2e95),		/* SCUF Gaming */
	GIP_VENDOR(0x3285),		/* Nacon */
	GIP_VENDOR(0x3537),		/* GameSir */
	GIP_VENDOR(0x366c),		/* ByoWave */
	{ }
};

MODULE_DEVICE_TABLE(usb, gip_table);

static struct usb_driver gip_driver = {
	.name		= "xbox-gip",
	.probe		= gip_probe,
	.disconnect	= gip_disconnect,
	.suspend	= gip_suspend,
	.resume		= gip_resume,
	.id_table	= gip_table,
};

module_usb_driver(gip_driver);

MODULE_AUTHOR("Vicki Pfau <vi@endrift.com>");
MODULE_DESCRIPTION("Xbox Gaming Input Protocol driver");
MODULE_LICENSE("GPL");
