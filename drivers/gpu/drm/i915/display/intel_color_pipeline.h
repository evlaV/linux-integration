/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2025 Intel Corporation
 */

#ifndef __INTEL_COLOR_PIPELINE_H__
#define __INTEL_COLOR_PIPELINE_H__

struct drm_plane;
enum pipe;

#define LUT_3D_SIZE 17

int intel_color_pipeline_plane_init(struct drm_plane *plane, enum pipe pipe);

#endif /* __INTEL_COLOR_PIPELINE_H__ */
