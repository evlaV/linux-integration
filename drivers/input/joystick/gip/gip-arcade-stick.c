// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Drivers for GIP arcade sticks
 *
 * Copyright (c) 2025 Valve Software
 *
 * This driver is based on the Microsoft GIP spec at:
 * https://aka.ms/gipdocs
 * https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-gipusb/e7c90904-5e21-426e-b9ad-d82adeee0dbc
 */
#include "gip.h"

enum gip_arcade_stick_vibration_motor_support {
	GIP_VIBRATION_NO_MOTOR = 0,
	GIP_VIBRATION_SINGLE_MOTOR = 1,
	GIP_VIBRATION_DUAL_MOTOR = 2,
};

struct gip_arcade_stick_info {
	uint8_t vibration_motor;
	uint8_t actuator_bits;
};

struct gip_arcade_stick_static_coniguration {
	uint8_t button_count;
	uint8_t report_version;
	uint8_t vibration_motor_support;
	uint8_t actuator_bits;
};

static int gip_arcade_stick_probe(struct gip_attachment *attachment)
{
	struct gip_arcade_stick_info *info = kzalloc(sizeof(*info), GFP_KERNEL);

	if (!info)
		return -ENOMEM;

	attachment->quirks |= GIP_QUIRK_SWAP_LB_RB;
	attachment->driver_data = info;

	return 0;
}

static void gip_arcade_stick_remove(struct gip_attachment *attachment)
{
	kfree(attachment->driver_data);
	attachment->driver_data = NULL;
}

static int gip_init_arcade_stick(struct gip_attachment *attachment)
{
	if (gip_supports_vendor_message(attachment, GIP_CMD_INITIAL_REPORTS_REQUEST, false)) {
		uint8_t request = GIP_LL_STATIC_CONFIGURATION;
		int rc = gip_send_vendor_message(attachment, GIP_CMD_INITIAL_REPORTS_REQUEST, 0,
			&request, sizeof(request));

		if (rc < 0)
			return rc;

		return GIP_INIT_NO_INPUT;
	}

	return 0;
}

static int gip_setup_arcade_stick_input(struct gip_attachment *attachment, struct input_dev* input)
{
	struct gip_arcade_stick_info *info = attachment->driver_data;
	int rc;

	if (!info)
		return -ENODEV;

	rc = gip_driver_navigation.setup_input(attachment, input);
	if (rc < 0)
		return rc;

	if (info->actuator_bits > 0) {
		input_set_abs_params(input, ABS_X, 0, (1 << info->actuator_bits) - 1, 0, 0);
		input_set_abs_params(input, ABS_Y, 0, (1 << info->actuator_bits) - 1, 0, 0);
	}

	if (attachment->extra_buttons >= 1)
		input_set_capability(input, EV_KEY, BTN_TR2);

	if (attachment->extra_buttons >= 2)
		input_set_capability(input, EV_KEY, BTN_TL2);
	return 0;
}

static int gip_handle_arcade_stick_report(struct gip_attachment *attachment,
	struct input_dev* input, const uint8_t *bytes, int num_bytes)
{
	struct gip_arcade_stick_info *info = attachment->driver_data;
	int16_t axis;
	int rc;

	if (!info)
		return -ENODEV;

	rc = gip_driver_navigation.handle_input_report(attachment, input, bytes, num_bytes);
	if (rc < 0)
		return rc;

	if (num_bytes < 6) {
		dev_dbg(GIP_DEV(attachment), "Discarding too-short input report\n");
		return -EINVAL;
	}

	if (info->actuator_bits > 0) {
		axis = bytes[2];
		axis |= bytes[3] << 8;
		input_report_abs(input, ABS_X, axis);

		axis = bytes[4];
		axis |= bytes[5] << 8;
		input_report_abs(input, ABS_Y, axis);
	}

	if (num_bytes >= 19) {
		/* Extra button 6 */
		input_report_key(input, BTN_TR2, bytes[18] & BIT(6));
		/* Extra button 7 */
		input_report_key(input, BTN_TL2, bytes[18] & BIT(7));
	}

	return 0;
}

static int gip_handle_arcade_stick_ll_static_configuration(struct gip_attachment *attachment,
	const struct gip_header *header, const uint8_t *bytes, int num_bytes)
{
	const struct gip_arcade_stick_static_coniguration *config =
		(const struct gip_arcade_stick_static_coniguration*)bytes;
	struct gip_arcade_stick_info *info = attachment->driver_data;

	if (!info)
		return -ENODEV;

	if (num_bytes < 4)
		return -EINVAL;

	attachment->extra_buttons = clamp(config->button_count, 6, 38) - 6;
	info->actuator_bits = min(config->actuator_bits, 8);

	if (config->vibration_motor_support == GIP_VIBRATION_NO_MOTOR)
		attachment->features &= ~GIP_FEATURE_MOTOR_CONTROL;

	return gip_setup_input_device(attachment);
}

const struct gip_driver gip_driver_arcade_stick = {
	.types = (const char* const[]) {
		"Windows.Xbox.Input.ArcadeStick",
		"Microsoft.Xbox.Input.ArcadeStick",
		NULL
	},
	.guid = GUID_INIT(0x332054cc, 0xa34b, 0x41d5, 0xa3, 0x4a,
		0xa6, 0xa6, 0x71, 0x1e, 0xc4, 0xb3),

	.probe = gip_arcade_stick_probe,
	.remove = gip_arcade_stick_remove,
	.init = gip_init_arcade_stick,
	.setup_input = gip_setup_arcade_stick_input,
	.handle_input_report = gip_handle_arcade_stick_report,
	.vendor_handlers = {
		[GIP_LL_STATIC_CONFIGURATION] = gip_handle_arcade_stick_ll_static_configuration,
	},
};
