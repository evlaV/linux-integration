// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Base drivers for common GIP devices
 *
 * Copyright (c) 2025 Valve Software
 *
 * This driver is based on the Microsoft GIP spec at:
 * https://aka.ms/gipdocs
 * https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-gipusb/e7c90904-5e21-426e-b9ad-d82adeee0dbc
 */
#include "gip.h"

struct gip_device_capabilities_response {
	uint8_t extra_button_count;
	uint8_t extra_axis_count;
	uint8_t led_count;
	uint8_t max_global_led_gain;
};

static bool dpad_as_buttons;

static int gip_setup_gamepad_input(struct gip_attachment *attachment, struct input_dev* input)
{
	int ret = gip_driver_navigation.setup_input(attachment, input);

	if (ret < 0)
		return ret;
	input_set_capability(input, EV_KEY, BTN_THUMBR);
	input_set_capability(input, EV_KEY, BTN_THUMBL);
	input_set_abs_params(input, ABS_X, -32768, 32767, 16, 128);
	input_set_abs_params(input, ABS_Y, -32768, 32767, 16, 128);
	input_set_abs_params(input, ABS_RX, -32768, 32767, 16, 128);
	input_set_abs_params(input, ABS_RY, -32768, 32767, 16, 128);
	input_set_abs_params(input, ABS_Z, 0, 1023, 0, 0);
	input_set_abs_params(input, ABS_RZ, 0, 1023, 0, 0);

	/* Xbox Adaptive Controller */
	if (attachment->vendor_id == 0x045e && attachment->product_id == 0x0b0a)
		input_set_abs_params(input, ABS_PROFILE, 0, 3, 0, 0);
	return 0;
}

static int gip_handle_gamepad_report(struct gip_attachment *attachment,
	struct input_dev *input, const uint8_t *bytes, int num_bytes)
{
	int16_t axis;
	int ret = gip_driver_navigation.handle_input_report(attachment, input, bytes, num_bytes);

	if (ret < 0)
		return ret;

	if (num_bytes < 14) {
		dev_dbg(GIP_DEV(attachment), "Discarding too-short input report\n");
		return -EINVAL;
	}

	input_report_key(input, BTN_THUMBL, bytes[1] & BIT(6));
	input_report_key(input, BTN_THUMBR, bytes[1] & BIT(7));

	axis = bytes[2];
	axis |= bytes[3] << 8;
	input_report_abs(input, ABS_Z, axis);

	axis = bytes[4];
	axis |= bytes[5] << 8;
	input_report_abs(input, ABS_RZ, axis);

	axis = bytes[6];
	axis |= bytes[7] << 8;
	input_report_abs(input, ABS_X, axis);
	axis = bytes[8];
	axis |= bytes[9] << 8;
	input_report_abs(input, ABS_Y, ~axis);
	axis = bytes[10];
	axis |= bytes[11] << 8;
	input_report_abs(input, ABS_RX, axis);
	axis = bytes[12];
	axis |= bytes[13] << 8;
	input_report_abs(input, ABS_RY, ~axis);

	/* Xbox Adaptive Controller */
	if (attachment->vendor_id == 0x045e && attachment->product_id == 0x0b0a && num_bytes >= 31)
		input_report_abs(input, ABS_PROFILE, bytes[30] & 3);

	return 0;
}

const struct gip_driver gip_driver_gamepad = {
	.types = (const char* const[]) { "Windows.Xbox.Input.Gamepad", NULL },
	.guid = GUID_INIT(0x082e402c, 0x07df, 0x45e1, 0xa5, 0xab,
		0xa3, 0x12, 0x7a, 0xf1, 0x97, 0xb5),

	.quirks = (const struct gip_quirks[]) {
		/* Xbox One Controller (model 1573) */
		{ 0x045e, 0x02d1, 0, .override_name = "Xbox One Controller" },

		/* Xbox One Controller (model 1697) */
		{ 0x045e, 0x02dd, 0, .override_name = "Xbox One Controller" },

		/* Xbox Elite */
		{ 0x045e, 0x02e3, 0,
			.override_name = "Xbox Elite Controller",
			.added_features = GIP_FEATURE_ELITE_BUTTONS,
			.filtered_features = GIP_FEATURE_CONSOLE_FUNCTION_MAP },

		/* Xbox One Controller (model 1708) */
		{ 0x045e, 0x02ea, 0, .override_name = "Xbox One Controller" },

		/* Xbox Elite 2 */
		{ 0x045e, 0x0b00, 0,
			.override_name = "Xbox Elite Series 2 Controller",
			.added_features = GIP_FEATURE_GUIDE_COLOR |
				GIP_FEATURE_EXTENDED_SET_DEVICE_STATE },

		/* Xbox Adaptive Controller */
		{ 0x045e, 0x0b0a, 0, .override_name = "Xbox Adaptive Controller" },

		/* Xbox Wireless Controller */
		{ 0x045e, 0x0b12, 0, .override_name = "Xbox Wireless Controller" },

		{0},
	},

	.probe = NULL,
	.remove = NULL,
	.init = NULL,
	.setup_input = gip_setup_gamepad_input,
	.handle_input_report = gip_handle_gamepad_report,
};

static int gip_setup_navigation_input(struct gip_attachment *attachment, struct input_dev *input)
{
	input_set_capability(input, EV_KEY, BTN_Y);
	input_set_capability(input, EV_KEY, BTN_B);
	input_set_capability(input, EV_KEY, BTN_X);
	input_set_capability(input, EV_KEY, BTN_A);
	input_set_capability(input, EV_KEY, BTN_SELECT);
	input_set_capability(input, EV_KEY, BTN_MODE);
	input_set_capability(input, EV_KEY, BTN_START);
	input_set_capability(input, EV_KEY, BTN_TR);
	input_set_capability(input, EV_KEY, BTN_TL);

	attachment->dpad_as_buttons = dpad_as_buttons;
	if (attachment->dpad_as_buttons) {
		input_set_capability(input, EV_KEY, BTN_DPAD_UP);
		input_set_capability(input, EV_KEY, BTN_DPAD_RIGHT);
		input_set_capability(input, EV_KEY, BTN_DPAD_LEFT);
		input_set_capability(input, EV_KEY, BTN_DPAD_DOWN);
	} else {
		input_set_abs_params(input, ABS_HAT0X, -1, 1, 0, 0);
		input_set_abs_params(input, ABS_HAT0Y, -1, 1, 0, 0);
	}

	return 0;
}

static int gip_handle_navigation_report(struct gip_attachment *attachment,
	struct input_dev *input, const uint8_t *bytes, int num_bytes)
{
	if (num_bytes < 2) {
		dev_dbg(GIP_DEV(attachment), "Discarding too-short input report\n");
		return -EINVAL;
	}

	input_report_key(input, BTN_START, bytes[0] & BIT(2));
	input_report_key(input, BTN_SELECT, bytes[0] & BIT(3));
	input_report_key(input, BTN_A, bytes[0] & BIT(4));
	input_report_key(input, BTN_B, bytes[0] & BIT(5));
	input_report_key(input, BTN_X, bytes[0] & BIT(6));
	input_report_key(input, BTN_Y, bytes[0] & BIT(7));

	if (attachment->dpad_as_buttons) {
		input_report_key(input, BTN_DPAD_UP, bytes[1] & BIT(0));
		input_report_key(input, BTN_DPAD_DOWN, bytes[1] & BIT(1));
		input_report_key(input, BTN_DPAD_LEFT, bytes[1] & BIT(2));
		input_report_key(input, BTN_DPAD_RIGHT, bytes[1] & BIT(3));
	} else {
		input_report_abs(input, ABS_HAT0X,
			!!(bytes[1] & BIT(3)) - !!(bytes[1] & BIT(2)));
		input_report_abs(input, ABS_HAT0Y,
			!!(bytes[1] & BIT(1)) - !!(bytes[1] & BIT(0)));
	}

	if (attachment->quirks & GIP_QUIRK_SWAP_LB_RB) {
		/* Previous */
		input_report_key(input, BTN_TR, bytes[1] & BIT(4));
		/* Next */
		input_report_key(input, BTN_TL, bytes[1] & BIT(5));
	} else {
		input_report_key(input, BTN_TL, bytes[1] & BIT(4));
		input_report_key(input, BTN_TR, bytes[1] & BIT(5));
	}

	return 0;
}

const struct gip_driver gip_driver_navigation = {
	.types = (const char* const[]) { "Windows.Xbox.Input.NavigationController", NULL },
	.guid = GUID_INIT(0xb8f31fe7, 0x7386, 0x40e9, 0xa9, 0xf8,
		0x2f, 0x21, 0x26, 0x3a, 0xcf, 0xb7),

	.probe = NULL,
	.remove = NULL,
	.init = NULL,
	.setup_input = gip_setup_navigation_input,
	.handle_input_report = gip_handle_navigation_report,
};

module_param(dpad_as_buttons, bool, 0444);
MODULE_PARM_DESC(dpad_as_buttons, "Map the D-Pad as buttons instead of axes");
