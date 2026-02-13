// SPDX-License-Identifier: (GPL-2.0-only OR BSD-3-Clause)
//
// This file is provided under a dual BSD/GPLv2 license. When using or
// redistributing this file, you may do so under either license.
//
// Copyright(c) 2023 Advanced Micro Devices, Inc. All rights reserved.
//
// Authors: Venkata Prasad Potturu <venkataprasad.potturu@amd.com>

/*.
 * PCI interface for Vangogh ACP device
 */

#include "linux/container_of.h"
#include "linux/device.h"
#include "linux/device/bus.h"
#include "linux/slab.h"
#include "linux/workqueue.h"
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/string.h>
#include <sound/sof.h>
#include <sound/soc-acpi.h>

#include "../sof-pci-dev.h"
#include "../../amd/mach-config.h"
#include "acp.h"
#include "acp-dsp-offset.h"

#define ACP5X_FUTURE_REG_ACLK_0 0x1864

static const struct sof_amd_acp_desc vangogh_chip_info = {
	.name		= "vangogh",
	.pgfsm_base	= ACP5X_PGFSM_BASE,
	.ext_intr_stat	= ACP5X_EXT_INTR_STAT,
	.dsp_intr_base	= ACP5X_DSP_SW_INTR_BASE,
	.sram_pte_offset = ACP5X_SRAM_PTE_OFFSET,
	.hw_semaphore_offset = ACP5X_AXI2DAGB_SEM_0,
	.probe_reg_offset = ACP5X_FUTURE_REG_ACLK_0,
};

struct oops_data {
	struct pci_dev *pdev;
	struct device *amp0, *amp1;
	struct work_struct work;
};

static void acp_pci_vgh_driver_reload_workfn(struct work_struct *work)
{
	struct oops_data *data = container_of(work, struct oops_data, work);
	struct pci_bus *bus = data->pdev->bus;
	int ret;

	device_release_driver(data->amp0);
	device_release_driver(data->amp1);

	pci_lock_rescan_remove();
	pci_stop_and_remove_bus_device(data->pdev);
	pci_rescan_bus(bus);
	pci_unlock_rescan_remove();

	ret = device_attach(data->amp0);
	if (ret < 0)
		dev_err(data->amp0, "Failed to attach driver: %d\n", ret);

	ret = device_attach(data->amp1);
	if (ret < 0)
		dev_err(data->amp1, "Failed to attach driver: %d\n", ret);

	kfree(data);
}

static int acp_pci_vgh_oops_handler(struct snd_sof_dev *sdev)
{
	struct snd_soc_card *card = sdev->component->card;
	struct snd_soc_component *component;
	struct oops_data *data;
	struct device *amp0 = NULL, *amp1 = NULL;

	for_each_card_components(card, component) {
		if (!strcmp(component->name, "i2c-ADS8388:00"))
			amp0 = get_device(component->dev);
		if (!strcmp(component->name, "i2c-ADS8388:01"))
			amp1 = get_device(component->dev);
	}

	if (!amp0 || !amp1)
		return -ENODEV;

	data = kzalloc(sizeof(struct oops_data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	data->pdev = to_pci_dev(sdev->dev);
	data->amp0 = amp0;
	data->amp1 = amp1;
	INIT_WORK(&data->work, acp_pci_vgh_driver_reload_workfn);

	queue_work(system_unbound_wq, &data->work);

	return 0;
}

static int acp_pci_vgh_ops_init(struct snd_sof_dev *sdev)
{
	int ret = sof_vangogh_ops_init(sdev);
	if (ret)
		return ret;

	sof_vangogh_ops.oops_handler = acp_pci_vgh_oops_handler;

	return 0;
}

static const struct sof_dev_desc vangogh_desc = {
	.machines		= snd_soc_acpi_amd_vangogh_sof_machines,
	.resindex_lpe_base	= 0,
	.resindex_pcicfg_base	= -1,
	.resindex_imr_base	= -1,
	.irqindex_host_ipc	= -1,
	.chip_info		= &vangogh_chip_info,
	.ipc_supported_mask     = BIT(SOF_IPC_TYPE_3),
	.ipc_default            = SOF_IPC_TYPE_3,
	.default_fw_path	= {
		[SOF_IPC_TYPE_3] = "amd/sof",
	},
	.default_tplg_path	= {
		[SOF_IPC_TYPE_3] = "amd/sof-tplg",
	},
	.default_fw_filename	= {
		[SOF_IPC_TYPE_3] = "sof-vangogh.ri",
	},
	.nocodec_tplg_filename	= "sof-acp.tplg",
	.ops			= &sof_vangogh_ops,
	.ops_init		= acp_pci_vgh_ops_init,
};

static int acp_pci_vgh_probe(struct pci_dev *pci, const struct pci_device_id *pci_id)
{
	unsigned int flag;

	if (pci->revision != ACP_VANGOGH_PCI_ID)
		return -ENODEV;

	flag = snd_amd_acp_find_config(pci);
	if (flag != FLAG_AMD_SOF && flag != FLAG_AMD_SOF_ONLY_DMIC)
		return -ENODEV;

	return sof_pci_probe(pci, pci_id);
};

static void acp_pci_vgh_remove(struct pci_dev *pci)
{
	sof_pci_remove(pci);
}

/* PCI IDs */
static const struct pci_device_id vgh_pci_ids[] = {
	{ PCI_DEVICE(PCI_VENDOR_ID_AMD, ACP_PCI_DEV_ID),
	.driver_data = (unsigned long)&vangogh_desc},
	{ 0, }
};
MODULE_DEVICE_TABLE(pci, vgh_pci_ids);

/* pci_driver definition */
static struct pci_driver snd_sof_pci_amd_vgh_driver = {
	.name = KBUILD_MODNAME,
	.id_table = vgh_pci_ids,
	.probe = acp_pci_vgh_probe,
	.remove = acp_pci_vgh_remove,
	.driver = {
		.pm = pm_ptr(&sof_pci_pm),
	},
};
module_pci_driver(snd_sof_pci_amd_vgh_driver);

MODULE_LICENSE("Dual BSD/GPL");
MODULE_DESCRIPTION("VANGOGH SOF Driver");
MODULE_IMPORT_NS("SND_SOC_SOF_AMD_COMMON");
MODULE_IMPORT_NS("SND_SOC_SOF_PCI_DEV");
