// SPDX-License-Identifier: GPL-2.0-or-later
/*
 *  Steam Deck EC driver for battery charge control
 *
 *  Copyright (C) 2026 Valve Corporation
 */

#include <acpi/acexcep.h>
#include <acpi/battery.h>
#include <linux/acpi.h>
#include <linux/platform_device.h>
#include <linux/power_supply.h>

#define STEAMDECK_CHCTL_NAME "steamdeck-charge-control"

struct steamdeck_chctl {
	struct acpi_device *adev;
	struct platform_device *pdev;
	struct acpi_battery_hook battery_hook;
	struct power_supply *hooked_battery;
};

static bool steamdeck_chctl_bat_threshold_valid(unsigned int threshold)
{
	/*
	 * This is the range that EC firmware considers "valid"
	 */
	return threshold >= 10 && threshold <= 100;
}

static int steamdeck_chctl_psy_ext_get_prop(struct power_supply *psy,
					    const struct power_supply_ext *ext,
					    void *data,
					    enum power_supply_property psp,
					    union power_supply_propval *val)
{
	struct steamdeck_chctl *sd = data;
	unsigned long long acpi_val;

	switch (psp) {
	case POWER_SUPPLY_PROP_CHARGE_CONTROL_END_THRESHOLD:
		break;
	default:
		return -EINVAL;
	}

	if (ACPI_FAILURE(acpi_evaluate_integer(sd->adev->handle,
					       "SFBL", NULL, &acpi_val)))
		return -EIO;

	if (steamdeck_chctl_bat_threshold_valid(acpi_val))
		val->intval = acpi_val;
	else
		val->intval = 100;

	return 0;
}

static int
steamdeck_chctl_psy_ext_set_prop(struct power_supply *psy,
				 const struct power_supply_ext *ext, void *data,
				 enum power_supply_property psp,
				 const union power_supply_propval *val)
{
	struct steamdeck_chctl *sd = data;
	int acpi_val;

	switch (psp) {
	case POWER_SUPPLY_PROP_CHARGE_CONTROL_END_THRESHOLD:
		acpi_val = val->intval;

		if (!steamdeck_chctl_bat_threshold_valid(acpi_val))
			return -EINVAL;

		break;
	default:
		return -EINVAL;
	}

	if (ACPI_FAILURE(acpi_execute_simple_method(sd->adev->handle,
						    "FCBL",
						    acpi_val)))
		return -EIO;

	return 0;
}

static int steamdeck_chctl_psy_ext_prop_is_writeable(struct power_supply *psy,
						     const struct power_supply_ext *ext,
						     void *data, enum power_supply_property psp)
{
	return true;
}

static const enum power_supply_property steamdeck_chctl_psy_ext_props[] = {
	POWER_SUPPLY_PROP_CHARGE_CONTROL_END_THRESHOLD,
};

static const struct power_supply_ext steamdeck_chctl_psy_ext = {
	.name = STEAMDECK_CHCTL_NAME,
	.properties = steamdeck_chctl_psy_ext_props,
	.num_properties = ARRAY_SIZE(steamdeck_chctl_psy_ext_props),
	.get_property = steamdeck_chctl_psy_ext_get_prop,
	.set_property = steamdeck_chctl_psy_ext_set_prop,
	.property_is_writeable = steamdeck_chctl_psy_ext_prop_is_writeable,
};

static int steamdeck_chctl_add_battery(struct power_supply *battery,
				       struct acpi_battery_hook *hook)
{
	struct steamdeck_chctl *sd = container_of(hook, struct steamdeck_chctl, battery_hook);
	struct device *dev = &sd->pdev->dev;

	if (sd->hooked_battery) {
		dev_err(dev, "Multiple batteries is not supported\n");
		return -EINVAL;
	}

	sd->hooked_battery = battery;

	return power_supply_register_extension(battery,
					       &steamdeck_chctl_psy_ext,
					       dev, sd);
}

static int steamdeck_chctl_remove_battery(struct power_supply *battery,
					  struct acpi_battery_hook *hook)
{
	struct steamdeck_chctl *sd = container_of(hook, struct steamdeck_chctl, battery_hook);

	power_supply_unregister_extension(battery, &steamdeck_chctl_psy_ext);
	sd->hooked_battery = NULL;

	return 0;
}

static int steamdeck_chctl_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct steamdeck_chctl *sd;

	sd = devm_kzalloc(dev, sizeof(*sd), GFP_KERNEL);
	if (!sd)
		return -ENOMEM;

	sd->pdev = pdev;
	sd->adev = ACPI_COMPANION(dev->parent);

	sd->battery_hook.name = dev_name(dev);
	sd->battery_hook.add_battery = steamdeck_chctl_add_battery;
	sd->battery_hook.remove_battery = steamdeck_chctl_remove_battery;

	return devm_battery_hook_register(dev, &sd->battery_hook);
}

static const struct platform_device_id steamdeck_chctl_id[] = {
	{ STEAMDECK_CHCTL_NAME, 0 },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(platform, steamdeck_chctl_id);

static struct platform_driver steamdeck_chctl_driver = {
	.driver = {
		.name	= STEAMDECK_CHCTL_NAME,
	},
	.probe		= steamdeck_chctl_probe,
	.id_table	= steamdeck_chctl_id,
};
module_platform_driver(steamdeck_chctl_driver);

MODULE_DESCRIPTION("Steam Deck EC Charge Control");
MODULE_AUTHOR("Nícolas F. R. A. Prado <nfraprado@collabora.com>");
MODULE_LICENSE("GPL");
