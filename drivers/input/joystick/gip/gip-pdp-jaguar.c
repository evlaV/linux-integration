// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Drivers for GIP PDP Jaguar-style guitars
 *
 * Copyright (c) 2026 Valve Software
 */

#include <linux/unaligned.h>
#include "gip.h"

#define GIP_QUIRK_PDP_HAS_RIGHT_STICK	BIT(31)

static int gip_setup_pdp_jaguar_input(struct gip_attachment *attachment, struct input_dev *input)
{
	/*
	 * Despite having the navigation controller GUID, we don't want to use
	 * those mappings. Instead, we use the xone mappings for compatibility
	 * reasons.
	 */

	/* Lower fret */
	input_set_capability(input, EV_KEY, BTN_TRIGGER_HAPPY1);
	input_set_capability(input, EV_KEY, BTN_TRIGGER_HAPPY2);
	input_set_capability(input, EV_KEY, BTN_TRIGGER_HAPPY3);
	input_set_capability(input, EV_KEY, BTN_TRIGGER_HAPPY4);
	input_set_capability(input, EV_KEY, BTN_TRIGGER_HAPPY5);
	/* Upper fret */
	input_set_capability(input, EV_KEY, BTN_TRIGGER_HAPPY6);
	input_set_capability(input, EV_KEY, BTN_TRIGGER_HAPPY7);
	input_set_capability(input, EV_KEY, BTN_TRIGGER_HAPPY8);
	input_set_capability(input, EV_KEY, BTN_TRIGGER_HAPPY9);
	input_set_capability(input, EV_KEY, BTN_TRIGGER_HAPPY10);

	input_set_capability(input, EV_KEY, BTN_START);
	input_set_capability(input, EV_KEY, BTN_SELECT);

	/* Whammy bar */
	input_set_abs_params(input, ABS_Y, 0, 255, 0, 0);
	/* Tilt */
	input_set_abs_params(input, ABS_Z, 0, 255, 0, 0);

	input_set_abs_params(input, ABS_HAT0X, -1, 1, 0, 0);
	input_set_abs_params(input, ABS_HAT0Y, -1, 1, 0, 0);

	if (attachment->quirks & GIP_QUIRK_PDP_HAS_RIGHT_STICK) {
		input_set_capability(input, EV_KEY, BTN_THUMBR);
		input_set_abs_params(input, ABS_RX, -32768, 32767, 16, 128);
		input_set_abs_params(input, ABS_RY, -32768, 32767, 16, 128);
	}

	return 0;
}

static int gip_handle_pdp_jaguar_report(struct gip_attachment *attachment,
	struct input_dev *input, const uint8_t *bytes, int num_bytes)
{
	bool lower;

	if (num_bytes < 4) {
		gip_dbg(attachment, "Discarding too-short input report\n");
		return -EINVAL;
	}

	input_report_key(input, BTN_START, bytes[0] & BIT(2));
	input_report_key(input, BTN_SELECT, bytes[0] & BIT(3));

	if (num_bytes >= 7 && (bytes[5] || bytes[6])) {
		/* Newer report version on the RiffMaster */
		input_report_key(input, BTN_TRIGGER_HAPPY1, bytes[5] & BIT(0));
		input_report_key(input, BTN_TRIGGER_HAPPY2, bytes[5] & BIT(1));
		input_report_key(input, BTN_TRIGGER_HAPPY3, bytes[5] & BIT(2));
		input_report_key(input, BTN_TRIGGER_HAPPY4, bytes[5] & BIT(3));
		input_report_key(input, BTN_TRIGGER_HAPPY5, bytes[5] & BIT(4));

		input_report_key(input, BTN_TRIGGER_HAPPY6, bytes[6] & BIT(0));
		input_report_key(input, BTN_TRIGGER_HAPPY7, bytes[6] & BIT(1));
		input_report_key(input, BTN_TRIGGER_HAPPY8, bytes[6] & BIT(2));
		input_report_key(input, BTN_TRIGGER_HAPPY9, bytes[6] & BIT(3));
		input_report_key(input, BTN_TRIGGER_HAPPY10, bytes[6] & BIT(4));
	} else {
		lower = bytes[1] & BIT(6);
		input_report_key(input, BTN_TRIGGER_HAPPY1, !lower && (bytes[0] & BIT(4)));
		input_report_key(input, BTN_TRIGGER_HAPPY2, !lower && (bytes[0] & BIT(5)));
		input_report_key(input, BTN_TRIGGER_HAPPY3, !lower && (bytes[0] & BIT(7)));
		input_report_key(input, BTN_TRIGGER_HAPPY4, !lower && (bytes[0] & BIT(6)));
		input_report_key(input, BTN_TRIGGER_HAPPY5, !lower && (bytes[1] & BIT(4)));

		input_report_key(input, BTN_TRIGGER_HAPPY6, lower && (bytes[0] & BIT(4)));
		input_report_key(input, BTN_TRIGGER_HAPPY7, lower && (bytes[0] & BIT(5)));
		input_report_key(input, BTN_TRIGGER_HAPPY8, lower && (bytes[0] & BIT(7)));
		input_report_key(input, BTN_TRIGGER_HAPPY9, lower && (bytes[0] & BIT(6)));
		input_report_key(input, BTN_TRIGGER_HAPPY10, lower && (bytes[1] & BIT(4)));
	}

	input_report_abs(input, ABS_Y, bytes[2]);
	input_report_abs(input, ABS_Z, bytes[3]);

	input_report_abs(input, ABS_HAT0X,
		!!(bytes[1] & BIT(3)) - !!(bytes[1] & BIT(2)));
	input_report_abs(input, ABS_HAT0Y,
		!!(bytes[1] & BIT(1)) - !!(bytes[1] & BIT(0)));

	if ((attachment->quirks & GIP_QUIRK_PDP_HAS_RIGHT_STICK) && num_bytes >= 14) {
		input_report_key(input, BTN_THUMBR, bytes[1] & BIT(6));
		input_report_abs(input, ABS_RX, (int16_t)get_unaligned_le16(&bytes[10]));
		input_report_abs(input, ABS_RY, ~(int16_t)get_unaligned_le16(&bytes[12]));
	}

	return 0;
}

const struct gip_driver gip_driver_pdp_jaguar = {
	.types = (const char *const[]) { "PDP.Xbox.Guitar.Jaguar", NULL },
	.guid = GUID_INIT(0x1a266af6, 0x3a46, 0x45e3, 0xb9, 0xb6,
		0x0f, 0x2c, 0x0b, 0x2c, 0x1e, 0xbe),

	.quirks = (const struct gip_quirks[]) {
		/* PDP RiffMaster */
		{ GIP_VID_PDP, GIP_PID_PDP_RIFFMASTER, 0,
			.quirks = GIP_QUIRK_PDP_HAS_RIGHT_STICK, },

		{0},
	},
	.probe = NULL,
	.remove = NULL,
	.init = NULL,
	.setup_input = gip_setup_pdp_jaguar_input,
	.handle_input_report = gip_handle_pdp_jaguar_report,
};
