/* SPDX-License-Identifier: GPL-2.0-or-later */

/* Copyright(C) 2025 Derek J. Clark <derekjohn.clark@gmail.com> */

#ifndef _LENOVO_LEGO_HID_CONFIG_
#define _LENOVO_LEGO_HID_CONFIG_

#include <linux/types.h>

struct hid_device;
struct hid_device_id;
struct work_struct;

int lego_cfg_raw_event(u8 *data, int size);
void cfg_setup(struct work_struct *work);
int lego_cfg_probe(struct hid_device *hdev, const struct hid_device_id *_id);
void lego_cfg_remove(struct hid_device *hdev);

#endif /* !_LENOVO_LEGO_HID_CONFIG_*/
