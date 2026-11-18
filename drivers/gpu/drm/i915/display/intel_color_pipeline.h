/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2025 Intel Corporation
 */

#ifndef __INTEL_COLOR_PIPELINE_H__
#define __INTEL_COLOR_PIPELINE_H__

struct drm_plane;
struct intel_crtc;
enum pipe;

#define LUT_3D_SIZE 17

int intel_color_pipeline_plane_init(struct drm_plane *plane, enum pipe pipe);
int intel_color_pipeline_crtc_init(struct intel_crtc *crtc);

#endif /* __INTEL_COLOR_PIPELINE_H__ */
