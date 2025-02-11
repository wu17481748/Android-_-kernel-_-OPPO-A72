/*
 * Copyright (c) 2019 MediaTek Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#ifndef _MTK_DRM_LOWPOWER_H_
#define _MTK_DRM_LOWPOWER_H_

#include <drm/drm_crtc.h>

struct mtk_drm_idlemgr_context {
	unsigned long long idle_check_interval;
	unsigned long long idlemgr_last_kick_time;
	unsigned int enterulps;
	int session_mode_before_enter_idle;
	int is_idle;
	int cur_lp_cust_mode;
};

struct mtk_drm_idlemgr {
	struct task_struct *idlemgr_task;
	wait_queue_head_t idlemgr_wq;
	atomic_t idlemgr_task_active;
	struct mtk_drm_idlemgr_context *idlemgr_ctx;
#ifdef VENDOR_EDIT
/*
 * LiPing-M@PSW.MM.Display.LCD.Feature, 2019/11/25
 * Add for MATE mode switch RGB display
 */
    struct task_struct *idle_for_meta_mode;
#endif /* VENDOR_EDIT */
};

void mtk_drm_idlemgr_kick(const char *source, struct drm_crtc *crtc,
			  int need_lock);
bool mtk_drm_is_idle(struct drm_crtc *crtc);

int mtk_drm_idlemgr_init(struct drm_crtc *crtc, int index);
unsigned int mtk_drm_set_idlemgr(struct drm_crtc *crtc, unsigned int flag,
				 bool need_lock);
unsigned long long
mtk_drm_set_idle_check_interval(struct drm_crtc *crtc,
				unsigned long long new_interval);
unsigned long long
mtk_drm_get_idle_check_interval(struct drm_crtc *crtc);
int _primary_path_idle_for_meta_mode(struct drm_crtc *crtc, char cmd);
#endif
