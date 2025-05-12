/* SPDX-License-Identifier: GPL-2.0-or-later */

/* Copyright(C) 2025 Derek J. Clark <derekjohn.clark@gmail.com> */

#ifndef _LENOVO_LEGO_HID_CORE_
#define _LENOVO_LEGO_HID_CORE_

#include <linux/types.h>

#define GO_PACKET_SIZE 64

struct hid_device;

enum lego_interface {
	//LEGION_GO_KB_INTF_IN = 0x80,
	LEGION_GO_TP_INTF_IN = 0x81,
	LEGION_GO_GP_INFT_IN = 0x83,
	LEGION_GO_CFG_INTF_IN = 0x84,
};

u8 get_endpoint_address(struct hid_device *hdev);

#endif /* !_LENOVO_LEGO_HID_CORE_*/
