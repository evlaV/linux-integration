// SPDX-License-Identifier: (GPL-2.0-only OR BSD-3-Clause)
//
// This file is provided under a dual BSD/GPLv2 license. When using or
// redistributing this file, you may do so under either license.
//
// Copyright(c) 2023 Advanced Micro Devices, Inc.
//
// Authors: Venkata Prasad Potturu <venkataprasad.potturu@amd.com>

/*
 * Hardware interface for Audio DSP on Vangogh platform
 */

#include <linux/delay.h>
#include <linux/module.h>

#include "acp.h"

#define I2S_HS_INSTANCE		0
#define I2S_BT_INSTANCE		1
#define I2S_SP_INSTANCE		2
#define PDM_DMIC_INSTANCE	3
#define I2S_HS_VIRTUAL_INSTANCE	4

static struct snd_soc_dai_driver vangogh_sof_dai[] = {
	[I2S_HS_INSTANCE] = {
		.id = I2S_HS_INSTANCE,
		.name = "acp-sof-hs",
		.playback = {
			.rates = SNDRV_PCM_RATE_8000_96000,
			.formats = SNDRV_PCM_FMTBIT_S16_LE | SNDRV_PCM_FMTBIT_S8 |
				   SNDRV_PCM_FMTBIT_U8 | SNDRV_PCM_FMTBIT_S32_LE,
			.channels_min = 2,
			.channels_max = 8,
			.rate_min = 8000,
			.rate_max = 96000,
		},
		.capture = {
			.rates = SNDRV_PCM_RATE_8000_48000,
			.formats = SNDRV_PCM_FMTBIT_S16_LE | SNDRV_PCM_FMTBIT_S8 |
				   SNDRV_PCM_FMTBIT_U8 | SNDRV_PCM_FMTBIT_S32_LE,
			/* Supporting only stereo for I2S HS controller capture */
			.channels_min = 2,
			.channels_max = 2,
			.rate_min = 8000,
			.rate_max = 48000,
		},
	},

	[I2S_BT_INSTANCE] = {
		.id = I2S_BT_INSTANCE,
		.name = "acp-sof-bt",
		.playback = {
			.rates = SNDRV_PCM_RATE_8000_96000,
			.formats = SNDRV_PCM_FMTBIT_S16_LE | SNDRV_PCM_FMTBIT_S8 |
				   SNDRV_PCM_FMTBIT_U8 | SNDRV_PCM_FMTBIT_S32_LE,
			.channels_min = 2,
			.channels_max = 8,
			.rate_min = 8000,
			.rate_max = 96000,
		},
		.capture = {
			.rates = SNDRV_PCM_RATE_8000_48000,
			.formats = SNDRV_PCM_FMTBIT_S16_LE | SNDRV_PCM_FMTBIT_S8 |
				   SNDRV_PCM_FMTBIT_U8 | SNDRV_PCM_FMTBIT_S32_LE,
			/* Supporting only stereo for I2S BT controller capture */
			.channels_min = 2,
			.channels_max = 2,
			.rate_min = 8000,
			.rate_max = 48000,
		},
	},

	[I2S_SP_INSTANCE] = {
		.id = I2S_SP_INSTANCE,
		.name = "acp-sof-sp",
		.playback = {
			.rates = SNDRV_PCM_RATE_8000_96000,
			.formats = SNDRV_PCM_FMTBIT_S16_LE | SNDRV_PCM_FMTBIT_S8 |
				   SNDRV_PCM_FMTBIT_U8 | SNDRV_PCM_FMTBIT_S32_LE,
			.channels_min = 2,
			.channels_max = 8,
			.rate_min = 8000,
			.rate_max = 96000,
		},
		.capture = {
			.rates = SNDRV_PCM_RATE_8000_48000,
			.formats = SNDRV_PCM_FMTBIT_S16_LE | SNDRV_PCM_FMTBIT_S8 |
				   SNDRV_PCM_FMTBIT_U8 | SNDRV_PCM_FMTBIT_S32_LE,
			/* Supporting only stereo for I2S SP controller capture */
			.channels_min = 2,
			.channels_max = 2,
			.rate_min = 8000,
			.rate_max = 48000,
		},
	},

	[PDM_DMIC_INSTANCE] = {
		.id = PDM_DMIC_INSTANCE,
		.name = "acp-sof-dmic",
		.capture = {
			.rates = SNDRV_PCM_RATE_8000_48000,
			.formats = SNDRV_PCM_FMTBIT_S32_LE,
			.channels_min = 2,
			.channels_max = 4,
			.rate_min = 8000,
			.rate_max = 48000,
		},
	},

	[I2S_HS_VIRTUAL_INSTANCE] = {
		.id = I2S_HS_VIRTUAL_INSTANCE,
		.name = "acp-sof-hs-virtual",
		.playback = {
			.rates = SNDRV_PCM_RATE_8000_96000,
			.formats = SNDRV_PCM_FMTBIT_S16_LE | SNDRV_PCM_FMTBIT_S8 |
					   SNDRV_PCM_FMTBIT_U8 | SNDRV_PCM_FMTBIT_S32_LE,
			.channels_min = 2,
			.channels_max = 8,
			.rate_min = 8000,
			.rate_max = 96000,
		},
		.capture = {
			.rates = SNDRV_PCM_RATE_8000_48000,
			.formats = SNDRV_PCM_FMTBIT_S16_LE | SNDRV_PCM_FMTBIT_S8 |
				   SNDRV_PCM_FMTBIT_U8 | SNDRV_PCM_FMTBIT_S32_LE,
			/* Supporting only stereo for I2S HS-Virtual controller capture */
			.channels_min = 2,
			.channels_max = 2,
			.rate_min = 8000,
			.rate_max = 48000,
		},
	},
};

static ssize_t dsp_crash_ctl_write(struct file *file, const char __user *from,
				   size_t count, loff_t *ppos)
{
	struct acp_dev_data *adata = file->private_data;
	struct snd_sof_dev *sdev = adata->dev;
	char *cmd;
	int ret = count;

	cmd = memdup_user_nul(from, count);
	if (IS_ERR(cmd))
		return PTR_ERR(cmd);

	if (cmd[count - 1] == '\n')
		cmd[count - 1] = '\0';

	if (!strcmp(cmd, "start")) {
		adata->dsp_crash_ctl = DSP_CRASH_CTL_ENABLED;
		dev_info(sdev->dev, "scheduled dsp crash\n");
	} else if (!strcmp(cmd, "stop")) {
		adata->dsp_crash_ctl = DSP_CRASH_CTL_DISABLED;
		dev_info(sdev->dev, "unscheduled dsp crash\n");
	} else if (!strcmp(cmd, "once")) {
		adata->dsp_crash_ctl = DSP_CRASH_CTL_ONCE;
		dev_info(sdev->dev, "scheduled one-time dsp crash\n");
	} else if (!strcmp(cmd, "suspend")) {
		adata->dsp_crash_ctl = DSP_CRASH_CTL_SUSPEND;
	} else if (!strcmp(cmd, "panic")) {
		adata->dsp_crash_ctl = DSP_CRASH_CTL_PANIC;
	} else if (!strcmp(cmd, "panic-resume")) {
		adata->dsp_crash_ctl = DSP_CRASH_CTL_PANIC_RESUME;
	} else {
		dev_err(sdev->dev, "invalid dsp crash ctl cmd: %s\n", cmd);
		ret = -EINVAL;
	}

	kfree(cmd);
	return ret;
}

static const struct file_operations sof_dsp_crash_ctl_fops = {
	.open = simple_open,
	.write = dsp_crash_ctl_write,
	.llseek = default_llseek,
};

#include <linux/debugfs.h>
static int debugfs_create_dsp_crash_ctl(struct snd_sof_dev *sdev)
{
	struct acp_dev_data *adata = sdev->pdata->hw_pdata;

	debugfs_create_file("dsp_crash_ctl", 0200, adata->dev->debugfs_root, adata,
			    &sof_dsp_crash_ctl_fops);

	return 0;
}

static int sof_vangogh_post_fw_run_delay(struct snd_sof_dev *sdev)
{
	struct acp_dev_data *adata = sdev->pdata->hw_pdata;
	/*
	 * Resuming from suspend in some cases my cause the DSP firmware
	 * to enter an unrecoverable faulty state.  Delaying a bit any host
	 * to DSP transmission right after firmware boot completion seems
	 * to resolve the issue.
	 */
	if (!sdev->first_boot) {
		usleep_range(100, 150);
		if (adata->dsp_crash_ctl == DSP_CRASH_CTL_SUSPEND) {
			adata->dsp_crash_ctl = DSP_CRASH_CTL_ENABLED;
			sof_set_fw_state(sdev, SOF_FW_BOOT_READY_FAILED);
			return -EIO;
		} else if (adata->dsp_crash_ctl == DSP_CRASH_CTL_PANIC_RESUME) {
			adata->dsp_crash_ctl = DSP_CRASH_CTL_PANIC;
		}
	} else {
		debugfs_create_dsp_crash_ctl(sdev);
	}

	return 0;
}

/* Vangogh ops */
struct snd_sof_dsp_ops sof_vangogh_ops;
EXPORT_SYMBOL_NS(sof_vangogh_ops, "SND_SOC_SOF_AMD_COMMON");

int sof_vangogh_ops_init(struct snd_sof_dev *sdev)
{
	const struct dmi_system_id *dmi_id;
	struct acp_quirk_entry *quirks;

	/* common defaults */
	memcpy(&sof_vangogh_ops, &sof_acp_common_ops, sizeof(struct snd_sof_dsp_ops));

	sof_vangogh_ops.drv = vangogh_sof_dai;
	sof_vangogh_ops.num_drv = ARRAY_SIZE(vangogh_sof_dai);

	dmi_id = dmi_first_match(acp_sof_quirk_table);
	if (dmi_id) {
		quirks = dmi_id->driver_data;

		if (quirks->signed_fw_image)
			sof_vangogh_ops.load_firmware = acp_sof_load_signed_firmware;

		if (quirks->post_fw_run_delay)
			sof_vangogh_ops.post_fw_run = sof_vangogh_post_fw_run_delay;
	}

	return 0;
}
