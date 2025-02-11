/*
 * Copyright (c) 2015 MediaTek Inc.
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

#include <linux/backlight.h>
#include <drm/drmP.h>
#include <drm/drm_mipi_dsi.h>
#include <drm/drm_panel.h>

#include <linux/gpio/consumer.h>
#include <linux/regulator/consumer.h>

#include <video/mipi_display.h>
#include <video/of_videomode.h>
#include <video/videomode.h>

#include <linux/module.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <soc/oppo/device_info.h>

#define CONFIG_MTK_PANEL_EXT
#if defined(CONFIG_MTK_PANEL_EXT)
#include "../mediatek/mtk_panel_ext.h"
#include "../mediatek/mtk_log.h"
#include "../mediatek/mtk_drm_graphics_base.h"
#endif

#ifdef CONFIG_MTK_ROUND_CORNER_SUPPORT
#include "../mediatek/mtk_corner_pattern/mtk_data_hw_roundedpattern.h"
#endif

u32 flag_writ = 0;
EXPORT_SYMBOL(flag_writ);

u32 flag_hbm = 0;
EXPORT_SYMBOL(flag_hbm);
#ifdef VENDOR_EDIT
#define RAMLESS_AOD_PAYLOAD_SIZE	100
#define OPPO_DC_BACKLIGHT_THRESHOLD 260
extern int oppo_dc_alpha;
extern int oppo_dc_enable_real;
extern int oppo_dc_enable;
unsigned int safe_mode = 2;
extern unsigned long oppo_display_brightness;
extern char send_cmd[RAMLESS_AOD_PAYLOAD_SIZE];
extern int oppo_seed_bright_to_alpha(int brightness);
#endif

#define LCM_DSI_CMD_MODE 1

#define REGFLAG_CMD       0xFFFA
#define REGFLAG_DELAY       0xFFFC
#define REGFLAG_UDELAY  0xFFFB
#define REGFLAG_END_OF_TABLE    0xFFFD

#define BRIGHTNESS_MAX    2047
#define BRIGHTNESS_HALF   1023

struct lcm {
	struct device *dev;
	struct drm_panel panel;
	struct backlight_device *backlight;
	struct gpio_desc *reset_gpio;
	struct gpio_desc *bias_gpio;

	bool prepared;
	bool enabled;

	int error;

	bool hbm_en;
	bool hbm_wait;
	bool cv_state;
};

struct LCM_setting_table {
	unsigned int cmd;
	unsigned char count;
	unsigned char para_list[128];
};

static struct LCM_setting_table lcm_aod_to_normal[] = {
	{REGFLAG_CMD,3,{0xF0,0x5A,0x5A}},
	{REGFLAG_CMD,2,{0x60,0x00}},
	{REGFLAG_CMD,2,{0x53,0x20}},
	{REGFLAG_CMD,2,{0xB0,0x32}},
#if 0
	{REGFLAG_CMD,2,{0xD1,0x00}},
#endif
	{REGFLAG_CMD,3,{0xF0,0xA5,0xA5}},
#if 0
	/*seed setting*/
	{REGFLAG_CMD,3,{0x81, 0x31, 0x02}},
	{REGFLAG_CMD,3,{0xF0, 0x5A, 0x5A}},
	{REGFLAG_CMD,2,{0xB0, 0x2B}},
	{REGFLAG_CMD,22,{0xB1,0xE4,0x05,0x00,0x07,0xE0,0x01,0x0C,0x02,0xF0,0x18,0xFA,0xE3,0xFB,0x0A,0xDF,0xF9,0xF5,0x01,0xFF,0xFF,0xFF}},
	{REGFLAG_CMD,2,{0xB0, 0x00}},
	{REGFLAG_CMD,3,{0xF0, 0xA5, 0xA5}},
#endif
	{REGFLAG_CMD,3,{0x9F,0x5A,0x5A}},
	{REGFLAG_CMD,1,{0x13}},
	{REGFLAG_CMD,1,{0x29}},
	{REGFLAG_CMD,3,{0x9F, 0xA5, 0xA5}},
	{REGFLAG_END_OF_TABLE, 0x00, {}}
};

struct LCM_setting_table lcm_seed_setting[] = {
	{REGFLAG_CMD,3,{0x81, 0x31, 0x02}},
	{REGFLAG_CMD,3,{0xF0, 0x5A, 0x5A}},
	{REGFLAG_CMD,2,{0xB0, 0x2B}},
	{REGFLAG_CMD,22,{0xB1,0xE4,0x05,0x00,0x07,0xE0,0x01,0x0C,0x02,0xF0,0x18,0xFA,0xE3,0xFB,0x0A,0xDF,0xF9,0xF5,0x01,0xFF,0xFF,0xFF}},
	{REGFLAG_CMD,2,{0xB0, 0x00}},
	{REGFLAG_CMD,3,{0xF0, 0xA5, 0xA5}},

	{REGFLAG_END_OF_TABLE, 0x00, {}}
};

struct LCM_setting_table lcm_setbrightness_normal[] = {
	{REGFLAG_CMD,2, {0x53,0x20}},
	{REGFLAG_CMD,3, {0x51,0x00,0x00}},
};

struct LCM_setting_table lcm_setbrightness_hbm[] = {
	{REGFLAG_CMD,3, {0x51,0x00,0x00}},
	{REGFLAG_CMD,2, {0x53,0xE8}},
	{REGFLAG_END_OF_TABLE, 0x00, {}}
};

struct LCM_setting_table lcm_finger_HBM_on_setting[] = {
	{REGFLAG_CMD,2, {0x53,0xE0}},
	{REGFLAG_CMD,3, {0x51,0x07,0xFF}},
};

struct LCM_setting_table lcm_normal_HBM_on_setting[] = {
	{REGFLAG_CMD,2, {0x53,0xE0}},
	{REGFLAG_CMD,3, {0x51,0x07,0xFF}},
};

#define lcm_dcs_write_seq(ctx, seq...) \
({\
	const u8 d[] = { seq };\
	BUILD_BUG_ON_MSG(ARRAY_SIZE(d) > 128, "DCS sequence too big for stack");\
	lcm_dcs_write(ctx, d, ARRAY_SIZE(d));\
})

#define lcm_dcs_write_seq_static(ctx, seq...) \
({\
	static const u8 d[] = { seq };\
	lcm_dcs_write(ctx, d, ARRAY_SIZE(d));\
})

static inline struct lcm *panel_to_lcm(struct drm_panel *panel)
{
	return container_of(panel, struct lcm, panel);
}

static void lcm_dcs_write(struct lcm *ctx, const void *data, size_t len)
{
	struct mipi_dsi_device *dsi = to_mipi_dsi_device(ctx->dev);
	ssize_t ret;
	char *addr;

	if (ctx->error < 0)
		return;

	addr = (char *)data;
	if ((int)*addr < 0xB0)
		ret = mipi_dsi_dcs_write_buffer(dsi, data, len);
	else
		ret = mipi_dsi_generic_write(dsi, data, len);
	if (ret < 0) {
		dev_err(ctx->dev, "error %zd writing seq: %ph\n", ret, data);
		ctx->error = ret;
	}
}

#ifdef PANEL_SUPPORT_READBACK
static int lcm_dcs_read(struct lcm *ctx, u8 cmd, void *data, size_t len)
{
	struct mipi_dsi_device *dsi = to_mipi_dsi_device(ctx->dev);
	ssize_t ret;

	if (ctx->error < 0)
		return 0;

	ret = mipi_dsi_dcs_read(dsi, cmd, data, len);
	if (ret < 0) {
		dev_err(ctx->dev, "error %d reading dcs seq:(%#x)\n", ret, cmd);
		ctx->error = ret;
	}

	return ret;
}

static void lcm_panel_get_data(struct lcm *ctx)
{
	u8 buffer[3] = {0};
	static int ret;

	if (ret == 0) {
		ret = lcm_dcs_read(ctx,  0x0A, buffer, 1);
		dev_info(ctx->dev, "return %d data(0x%08x) to dsi engine\n",
			 ret, buffer[0] | (buffer[1] << 8));
	}
}
#endif

#if defined(CONFIG_RT5081_PMU_DSV) || defined(CONFIG_MT6370_PMU_DSV)
static struct regulator *disp_bias_pos;
static struct regulator *disp_bias_neg;

static int lcm_panel_bias_regulator_init(void)
{
	static int regulator_inited;
	int ret = 0;

	if (regulator_inited)
		return ret;

	/* please only get regulator once in a driver */
	disp_bias_pos = regulator_get(NULL, "dsv_pos");
	if (IS_ERR(disp_bias_pos)) { /* handle return value */
		ret = PTR_ERR(disp_bias_pos);
		pr_err("get dsv_pos fail, error: %d\n", ret);
		return ret;
	}

	disp_bias_neg = regulator_get(NULL, "dsv_neg");
	if (IS_ERR(disp_bias_neg)) { /* handle return value */
		ret = PTR_ERR(disp_bias_neg);
		pr_err("get dsv_neg fail, error: %d\n", ret);
		return ret;
	}

	regulator_inited = 1;
	return ret; /* must be 0 */

}

static int lcm_panel_bias_enable(void)
{
	int ret = 0;
	int retval = 0;

	lcm_panel_bias_regulator_init();

	/* set voltage with min & max*/
	ret = regulator_set_voltage(disp_bias_pos, 5400000, 5400000);
	if (ret < 0)
		pr_err("set voltage disp_bias_pos fail, ret = %d\n", ret);
	retval |= ret;

	ret = regulator_set_voltage(disp_bias_neg, 5400000, 5400000);
	if (ret < 0)
		pr_err("set voltage disp_bias_neg fail, ret = %d\n", ret);
	retval |= ret;

	/* enable regulator */
	ret = regulator_enable(disp_bias_pos);
	if (ret < 0)
		pr_err("enable regulator disp_bias_pos fail, ret = %d\n", ret);
	retval |= ret;

	ret = regulator_enable(disp_bias_neg);
	if (ret < 0)
		pr_err("enable regulator disp_bias_neg fail, ret = %d\n", ret);
	retval |= ret;

	return retval;
}

static int lcm_panel_bias_disable(void)
{
	int ret = 0;
	int retval = 0;

	lcm_panel_bias_regulator_init();

	ret = regulator_disable(disp_bias_neg);
	if (ret < 0)
		pr_err("disable regulator disp_bias_neg fail, ret = %d\n", ret);
	retval |= ret;

	ret = regulator_disable(disp_bias_pos);
	if (ret < 0)
		pr_err("disable regulator disp_bias_pos fail, ret = %d\n", ret);
	retval |= ret;

	return retval;
}
#endif

static struct regulator *disp_ldo3;

static int lcm_panel_ldo3_regulator_init(struct device *dev)
{
	static int regulator_inited;
	int ret = 0;

	if (regulator_inited)
		return ret;
    pr_err("get lcm_panel_ldo3_regulator_init\n");

	/* please only get regulator once in a driver */
	disp_ldo3 = regulator_get(dev, "VMC");
	if (IS_ERR(disp_ldo3)) { /* handle return value */
		ret = PTR_ERR(disp_ldo3);
		pr_err("get ldo3 fail, error: %d\n", ret);
		return ret;
	}
	regulator_inited = 1;
	return ret; /* must be 0 */

}

static int lcm_panel_ldo3_enable(struct device *dev)
{
	int ret = 0;
	int retval = 0;

	lcm_panel_ldo3_regulator_init(dev);

	/* set voltage with min & max*/
	ret = regulator_set_voltage(disp_ldo3, 3000000, 3000000);
	if (ret < 0)
		pr_err("set voltage disp_ldo3 fail, ret = %d\n", ret);
	retval |= ret;

	/* enable regulator */
	ret = regulator_enable(disp_ldo3);
	if (ret < 0)
		pr_err("enable regulator disp_ldo3 fail, ret = %d\n", ret);
	retval |= ret;
    pr_err("get lcm_panel_ldo3_enable\n");

	return retval;
}

static int lcm_panel_ldo3_disable(struct device *dev)
{
	int ret = 0;
	int retval = 0;

	lcm_panel_ldo3_regulator_init(dev);

	ret = regulator_disable(disp_ldo3);
	if (ret < 0)
		pr_err("disable regulator disp_ldo3 fail, ret = %d\n", ret);
	retval |= ret;
    pr_err("disable regulator disp_ldo3\n");

	return retval;
}

static struct regulator *disp_pm8008_ldo1;

static int lcm_panel_pm8008_ldo1_regulator_init(struct device *dev)
{
	static int regulator_inited;
	int ret = 0;

	if (regulator_inited)
		return ret;

	/* please only get regulator once in a driver */
	disp_pm8008_ldo1 = regulator_get(dev, "pm8008_l1");
	if (IS_ERR(disp_pm8008_ldo1)) { /* handle return value */
		ret = PTR_ERR(disp_pm8008_ldo1);
		pr_err("get pm8008_ldo1 fail, error: %d\n", ret);
		return ret;
	}

	regulator_inited = 1;
	return ret; /* must be 0 */

}

static int lcm_panel_pm8008_ldo1_enable(struct device *dev)
{
	int ret = 0;
	int retval = 0;

	lcm_panel_pm8008_ldo1_regulator_init(dev);

	/* set voltage with min & max*/
	ret = regulator_set_voltage(disp_pm8008_ldo1, 1250000, 1250000);
	if (ret < 0)
		pr_err("set voltage disp_pm8008_ldo1 fail, ret = %d\n", ret);
	retval |= ret;

	/* enable regulator */
	ret = regulator_enable(disp_pm8008_ldo1);
	if (ret < 0)
		pr_err("enable regulator disp_pm8008_ldo1 fail, ret = %d\n", ret);
	retval |= ret;

	return retval;
}

static int lcm_panel_pm8008_ldo1_disable(struct device *dev)
{
	int ret = 0;
	int retval = 0;

	lcm_panel_pm8008_ldo1_regulator_init(dev);

	ret = regulator_disable(disp_pm8008_ldo1);
	if (ret < 0)
		pr_err("disable regulator disp_pm8008_ldo1 fail, ret = %d\n", ret);
	retval |= ret;

	return retval;
}

static void lcm_panel_init(struct lcm *ctx)
{
	pr_err("debug for lcm ZJB%s\n", __func__);
	/*sleep out*/
	lcm_dcs_write_seq_static(ctx, 0x11);
	msleep(20);
	lcm_dcs_write_seq_static(ctx, 0x9F, 0x5A, 0x5A);
	/*TE sync on*/
	lcm_dcs_write_seq_static(ctx, 0x35, 0x00);
	lcm_dcs_write_seq_static(ctx, 0x9F, 0xA5, 0xA5);
	/* PPS setting */
	lcm_dcs_write_seq_static(ctx, 0x9F, 0x5A, 0x5A);
	lcm_dcs_write_seq_static(ctx, 0x9D, 0x01);
	lcm_dcs_write_seq_static(ctx, 0x9E, 0x11, 0x00, 0x00, 0x89, 0x30, 0x80, 0x09, 0x60, 0x04,\
								  0x38, 0x00, 0x30, 0x02, 0x1C, 0x02, 0x1C, 0x02, 0x00, 0x02,\
								  0x52, 0x00, 0x20, 0x04, 0x23, 0x00, 0x07, 0x00, 0x0F, 0x02,\
								  0x8E, 0x02, 0x1F, 0x18, 0x00, 0x10, 0xF0, 0x03, 0x0C, 0x20,\
								  0x00, 0x06, 0x0B, 0x0B, 0x33, 0x0E, 0x1C, 0x2A, 0x38, 0x46,\
								  0x54, 0x62, 0x69, 0x70, 0x77, 0x79, 0x7B, 0x7D, 0x7E, 0x01,\
								  0x02, 0x01, 0x00, 0x09, 0x40, 0x09, 0xBE, 0x19, 0xFC, 0x19,\
								  0xFA, 0x19, 0xF8, 0x1A, 0x38, 0x1A, 0x78, 0x1A, 0xB6, 0x2A,\
								  0xB6, 0x2A, 0xF4, 0x2A, 0xF4, 0x4B, 0x34, 0x63, 0x74);
	lcm_dcs_write_seq_static(ctx, 0x9F, 0xA5, 0xA5);
	/* CASET/PASET Setting */
	lcm_dcs_write_seq_static(ctx, 0x2A, 0x00, 0x00, 0x04, 0x37);
	lcm_dcs_write_seq_static(ctx, 0x2B, 0x00, 0x00, 0x09, 0x5F);
	/* TSP SYNC ON */
	lcm_dcs_write_seq_static(ctx, 0xF0, 0x5A, 0x5A);
	lcm_dcs_write_seq_static(ctx, 0xB0, 0x05);
	lcm_dcs_write_seq_static(ctx, 0xD7, 0x07, 0x02, 0x40);
	lcm_dcs_write_seq_static(ctx, 0xF0, 0xA5, 0xA5);
	/* Driver-IC Initial Code */
	lcm_dcs_write_seq_static(ctx, 0xF0, 0x5A, 0x5A);
	lcm_dcs_write_seq_static(ctx, 0xF1, 0x5A, 0x5A);
	lcm_dcs_write_seq_static(ctx, 0xE5, 0x13, 0xE8, 0xFC);
	lcm_dcs_write_seq_static(ctx, 0xED, 0x97);
	lcm_dcs_write_seq_static(ctx, 0xB0, 0x1A);
	lcm_dcs_write_seq_static(ctx, 0xF9, 0x20);
	lcm_dcs_write_seq_static(ctx, 0xF1, 0xA5, 0xA5);
	lcm_dcs_write_seq_static(ctx, 0xF0, 0xA5, 0xA5);
	/* Dimming setting */
	lcm_dcs_write_seq_static(ctx, 0xF0, 0x5A, 0x5A);
	lcm_dcs_write_seq_static(ctx, 0xB0, 0x06);
	lcm_dcs_write_seq_static(ctx, 0xB7, 0x01);
	lcm_dcs_write_seq_static(ctx, 0xB0, 0x05);
	lcm_dcs_write_seq_static(ctx, 0xB7, 0x13);
	lcm_dcs_write_seq_static(ctx, 0xF0, 0xA5, 0xA5);
	/* ACL OFF*/
	lcm_dcs_write_seq_static(ctx, 0xF0, 0x5A, 0x5A);
	lcm_dcs_write_seq_static(ctx, 0x55, 0x00);
	lcm_dcs_write_seq_static(ctx, 0xF0, 0xA5, 0xA5);
	/* 60FPS Setting*/
	lcm_dcs_write_seq_static(ctx, 0xF0, 0x5A, 0x5A);
	lcm_dcs_write_seq_static(ctx, 0x60, 0x00);
	lcm_dcs_write_seq_static(ctx, 0xF0, 0xA5, 0xA5);
	lcm_dcs_write_seq_static(ctx, 0x53, 0x20);
	msleep(130);
	lcm_dcs_write_seq_static(ctx, 0x9F, 0x5A, 0x5A);
	/* Display On*/
	lcm_dcs_write_seq_static(ctx, 0x29);
	lcm_dcs_write_seq_static(ctx, 0x13);
	lcm_dcs_write_seq_static(ctx, 0x9F, 0xA5, 0xA5);
}

static int lcm_disable(struct drm_panel *panel)
{
	struct lcm *ctx = panel_to_lcm(panel);

#ifdef VENDOR_EDIT
/* Hujie@PSW.MM.DisplayDriver.AOD, 2019/12/10, add for keylog*/
	pr_err("debug for lcm %s\n", __func__);
#endif
	if (!ctx->enabled)
		return 0;

	if (ctx->backlight) {
		ctx->backlight->props.power = FB_BLANK_POWERDOWN;
		backlight_update_status(ctx->backlight);
	}

	ctx->enabled = false;

	return 0;
}

static int lcm_unprepare(struct drm_panel *panel)
{
	struct lcm *ctx = panel_to_lcm(panel);

#ifdef VENDOR_EDIT
	/* Hujie@PSW.MM.DisplayDriver.AOD, 2019/12/10, add for keylog*/
	pr_err("debug for lcm %s\n", __func__);
#endif

	if (!ctx->prepared)
		return 0;
#if 0
	lcm_dcs_write_seq_static(ctx, 0x28);
	msleep(10);
	lcm_dcs_write_seq_static(ctx, 0x10);
	msleep(150);
#endif

	ctx->error = 0;
	ctx->prepared = false;
#if defined(CONFIG_RT5081_PMU_DSV) || defined(CONFIG_MT6370_PMU_DSV)
	lcm_panel_bias_disable();
#endif
#if 0
	ctx->bias_gpio =
	devm_gpiod_get(ctx->dev, "bias", GPIOD_OUT_HIGH);
	gpiod_set_value(ctx->bias_gpio, 0);
	msleep(15);
	devm_gpiod_put(ctx->dev, ctx->bias_gpio);
#endif
	ctx->hbm_en = false;
	return 0;
}
#if 0
extern int power_mode;
#endif
static int lcm_prepare(struct drm_panel *panel)
{
	struct lcm *ctx = panel_to_lcm(panel);
	int ret;

	if (ctx->prepared)
		return 0;
#ifdef VENDOR_EDIT
        /* Hujie@PSW.MM.DisplayDriver.AOD, 2019/12/10, add for keylog*/
        pr_err("debug for lcm %s\n", __func__);
#endif

#if 0
	ctx->bias_gpio =
	devm_gpiod_get(ctx->dev, "bias", GPIOD_OUT_HIGH);
	gpiod_set_value(ctx->bias_gpio, 1);
	devm_gpiod_put(ctx->dev, ctx->bias_gpio);
#endif

#if 0
	if(power_mode == 2)
	{
		DDPINFO("%s + lcm_panel_init,resume status\n", __func__);
		lcm_panel_init(ctx);
	}
#endif
	ret = ctx->error;
	if (ret < 0)
		lcm_unprepare(panel);

	ctx->prepared = true;

#ifdef PANEL_SUPPORT_READBACK
	lcm_panel_get_data(ctx);
#endif

	return ret;
}

static int lcm_enable(struct drm_panel *panel)
{
	struct lcm *ctx = panel_to_lcm(panel);
#ifdef VENDOR_EDIT
/* Hujie@PSW.MM.DisplayDriver.AOD, 2019/12/10, add for keylog*/
	pr_err("debug for lcm %s\n", __func__);
#endif

	if (ctx->enabled)
		return 0;

	if (ctx->backlight) {
		ctx->backlight->props.power = FB_BLANK_UNBLANK;
		backlight_update_status(ctx->backlight);
	}

	ctx->enabled = true;

	return 0;
}

static const struct drm_display_mode default_mode = {
	.clock = 170434,
	.hdisplay = 1080,
	.hsync_start = 1080 + 52,
	.hsync_end = 1080 + 52 + 14,
	.htotal = 1080 + 52 + 14 + 22,
	.vdisplay = 2400,
	.vsync_start = 2400 + 21,
	.vsync_end = 2400 + 21 + 2,
	.vtotal = 2400 + 21 + 2 + 9,
	.vrefresh = 60,
};

static const struct drm_display_mode performance_mode = {
	.clock = 255651,
	.hdisplay = 1080,
	.hsync_start = 1080 + 52,
	.hsync_end = 1080 + 52 + 14,
	.htotal = 1080 + 52 + 14 + 22,
	.vdisplay = 2400,
	.vsync_start = 2400 + 21,
	.vsync_end = 2400 + 21 + 2,
	.vtotal = 2400 + 21 + 2 + 9,
	.vrefresh = 90,
};

#if defined(CONFIG_MTK_PANEL_EXT)
static struct mtk_panel_params ext_params = {
	.pll_clk = 190,
	.cust_esd_check = 1,
	.esd_check_enable = 1,
	.lcm_esd_check_table[0] = {
		.cmd = 0x0A, .count = 1, .para_list[0] = 0x9C, .mask_list[0] = 0x9C,
	},
	.lcm_esd_check_table[1] = {
		.cmd = 0xEE, .count = 1, .para_list[0] = 0x00, .mask_list[0] = 0x80,
	},
	.lcm_esd_check_table[2] = {
		.cmd = 0xEA, .count = 1, .para_list[0] = 0x00, .mask_list[0] = 0x80,
	},
	.output_mode = MTK_PANEL_DSC_SINGLE_PORT,
	.dsc_params = {
		.enable = 1,
		.ver = 17,
		.slice_mode = 1,
		.rgb_swap = 0,
		.dsc_cfg = 34,
		.rct_on = 1,
		.bit_per_channel = 8,
		.dsc_line_buf_depth = 9,
		.bp_enable = 1,
		.bit_per_pixel = 128,
		.pic_height = 2400,
		.pic_width = 1080,
		.slice_height = 48,
		.slice_width = 540,
		.chunk_size = 540,
		.xmit_delay = 512,
		.dec_delay = 594,
		.scale_value = 32,
		.increment_interval = 1059,
		.decrement_interval = 7,
		.line_bpg_offset = 15,
		.nfl_bpg_offset = 654,
		.slice_bpg_offset = 543,
		.initial_offset = 6144,
		.final_offset = 4336,
		.flatness_minqp = 3,
		.flatness_maxqp = 12,
		.rc_model_size = 8192,
		.rc_edge_factor = 6,
		.rc_quant_incr_limit0 = 11,
		.rc_quant_incr_limit1 = 11,
		.rc_tgt_offset_hi = 3,
		.rc_tgt_offset_lo = 3,
		},
	.data_rate = 380,
	.hbm_en_time = 2,
	.hbm_dis_time = 1,
};

static struct mtk_panel_params ext_params_90hz = {
	.pll_clk = 285,
	.cust_esd_check = 1,
	.esd_check_enable = 1,
	.lcm_esd_check_table[0] = {
        .cmd = 0x0A, .count = 1, .para_list[0] = 0x9C, .mask_list[0] = 0x9C,
	},
	.lcm_esd_check_table[1] = {
        .cmd = 0xEE, .count = 1, .para_list[0] = 0x00, .mask_list[0] = 0x80,
	},
	.lcm_esd_check_table[2] = {
        .cmd = 0xEA, .count = 1, .para_list[0] = 0x00, .mask_list[0] = 0x80,
	},
	.output_mode = MTK_PANEL_DSC_SINGLE_PORT,
    .dsc_params = {
        .enable = 1,
        .ver = 17,
        .slice_mode = 1,
        .rgb_swap = 0,
        .dsc_cfg = 34,
        .rct_on = 1,
        .bit_per_channel = 8,
        .dsc_line_buf_depth = 9,
        .bp_enable = 1,
        .bit_per_pixel = 128,
        .pic_height = 2400,
        .pic_width = 1080,
        .slice_height = 48,
        .slice_width = 540,
        .chunk_size = 540,
        .xmit_delay = 512,
        .dec_delay = 594,
        .scale_value = 32,
        .increment_interval = 1059,
        .decrement_interval = 7,
        .line_bpg_offset = 15,
        .nfl_bpg_offset = 654,
        .slice_bpg_offset = 543,
        .initial_offset = 6144,
        .final_offset = 4336,
        .flatness_minqp = 3,
        .flatness_maxqp = 12,
        .rc_model_size = 8192,
        .rc_edge_factor = 6,
        .rc_quant_incr_limit0 = 11,
        .rc_quant_incr_limit1 = 11,
        .rc_tgt_offset_hi = 3,
        .rc_tgt_offset_lo = 3,
        },
    .hbm_en_time = 2,
	.hbm_dis_time = 1,
	.data_rate = 570,
};
static int mtk_panel_ext_param_set(struct drm_panel *panel,
			 unsigned int mode)
{
	struct mtk_panel_ext *ext = find_panel_ext(panel);
	int ret = 0;
	if (mode == 0)
		ext->params = &ext_params;
	else if (mode == 1)
		ext->params = &ext_params_90hz;
	else
		ret = 1;

	return ret;
}

static void mode_switch_60_to_90(struct drm_panel *panel,
	enum MTK_PANEL_MODE_SWITCH_STAGE stage)
{
	struct lcm *ctx = panel_to_lcm(panel);

	if (stage == BEFORE_DSI_POWERDOWN) {
		/* display off */
        lcm_dcs_write_seq_static(ctx, 0x28);
        msleep(10);
        lcm_dcs_write_seq_static(ctx, 0x10);
        msleep(120);
	} else if (stage == AFTER_DSI_POWERON) {
		/* display on switch to 90hz */
        lcm_dcs_write_seq_static(ctx, 0xF0, 0x5A, 0x5A);
        lcm_dcs_write_seq_static(ctx, 0x60, 0x10);
        lcm_dcs_write_seq_static(ctx, 0xF0, 0xA5, 0xA5);
    }
}

static void mode_switch_90_to_60(struct drm_panel *panel,
	enum MTK_PANEL_MODE_SWITCH_STAGE stage)
{
	struct lcm *ctx = panel_to_lcm(panel);

	if (stage == BEFORE_DSI_POWERDOWN) {
		/* display off */
        lcm_dcs_write_seq_static(ctx, 0x28);
        msleep(10);
        lcm_dcs_write_seq_static(ctx, 0x10);
        msleep(120);
	} else if (stage == AFTER_DSI_POWERON) {
		/* display on switch to 60hz */
		lcm_dcs_write_seq_static(ctx, 0xF0, 0x5A, 0x5A);
		lcm_dcs_write_seq_static(ctx, 0x60, 0x00);
		lcm_dcs_write_seq_static(ctx, 0xF0, 0xA5, 0xA5);
	}
}

static int mode_switch(struct drm_panel *panel, unsigned int cur_mode,
		unsigned int dst_mode, enum MTK_PANEL_MODE_SWITCH_STAGE stage)
{
	int ret = 0;

	if (cur_mode == 0 && dst_mode == 1) { /* 60 switch to 90 */
		mode_switch_60_to_90(panel, stage);
	} else if (cur_mode == 1 && dst_mode == 0) { /* 90 switch to 60 */
		mode_switch_90_to_60(panel, stage);
	} else
		ret = 1;

	return ret;
}

static int enter_aod(void *handle, int enter)
{
	/* enter & exit AOD cmd */
	return 0;
}

static int panel_ext_reset(struct drm_panel *panel, int on)
{
#if 0
	struct lcm *ctx = panel_to_lcm(panel);

	ctx->reset_gpio =
		devm_gpiod_get(ctx->dev, "reset", GPIOD_OUT_HIGH);
	gpiod_set_value(ctx->reset_gpio, on);
	devm_gpiod_put(ctx->dev, ctx->reset_gpio);
#endif
	return 0;
}

static unsigned long panel_doze_get_mode_flags(struct drm_panel *panel, int doze_en)
{
	unsigned long mode_flags;

	//DDPINFO("%s doze_en:%d\n", __func__, doze_en);
	if (doze_en) {
		mode_flags = MIPI_DSI_MODE_LPM
		       | MIPI_DSI_MODE_EOT_PACKET
		       | MIPI_DSI_CLOCK_NON_CONTINUOUS;
	} else {
		mode_flags = MIPI_DSI_MODE_VIDEO
		       | MIPI_DSI_MODE_VIDEO_BURST
		       | MIPI_DSI_MODE_LPM | MIPI_DSI_MODE_EOT_PACKET
		       | MIPI_DSI_CLOCK_NON_CONTINUOUS;
	}

	#ifdef VENDOR_EDIT
	/* Hujie@PSW.MM.DisplayDriver.AOD, 2019/12/10, add for keylog*/
	pr_err("debug for %s, mode flags =%d, doze_en = %d\n", __func__,mode_flags,doze_en);
	#endif
	return mode_flags;
}
/*static int panel_doze_enable(struct drm_panel *panel)
{
	struct lcm *ctx = panel_to_lcm(panel);

	DDPINFO("%s\n", __func__);

	//lcm_normal_to_aod(ctx);
	lcm_panel_init_cmd(ctx);
	return 0;
}*/

static int panel_doze_disable(void *dsi, dcs_write_gce cb, void *handle)
{
	//struct lcm *ctx = panel_to_lcm(panel);
	unsigned int i=0;
#ifdef VENDOR_EDIT
	/* Hujie@PSW.MM.DisplayDriver.AOD, 2019/12/10, add for keylog*/
	pr_err("debug for lcm %s\n", __func__);
#endif

	/* Switch back to VDO mode */
	for (i = 0; i < (sizeof(lcm_aod_to_normal) / sizeof(struct LCM_setting_table)); i++) {
		unsigned cmd;
		cmd = lcm_aod_to_normal[i].cmd;

		switch (cmd) {

			case REGFLAG_DELAY:
					msleep(lcm_aod_to_normal[i].count);
				break;

			case REGFLAG_UDELAY:
				udelay(lcm_aod_to_normal[i].count);
				break;

			case REGFLAG_END_OF_TABLE:
				break;

			default:
				cb(dsi, handle, lcm_aod_to_normal[i].para_list, lcm_aod_to_normal[i].count);
		}
	}

	return 0;
}

static struct LCM_setting_table lcm_normal_to_aod_sam[] = {
	{REGFLAG_CMD, 3, {0x9F, 0x5A,0x5A}},
	{REGFLAG_CMD, 1, {0x28}},
	{REGFLAG_CMD, 3, {0x9F, 0xA5,0xA5}},
#if 0
	/* TE vsync ON */
	{REGFLAG_CMD,2,{0x35,0x00}},
	{REGFLAG_DELAY,10,{}},
#endif
	/*AOD MODE Setting*/
	{REGFLAG_CMD,3,{0xF0, 0x5A,0x5A}},
	{REGFLAG_CMD,2,{0x60, 0x00}},
	{REGFLAG_CMD,2,{0x53, 0x23}},
	{REGFLAG_CMD,2,{0xB0, 0x32}},
	{REGFLAG_CMD,3,{0xF0, 0xA5,0xA5}},

	{REGFLAG_CMD,3,{0xF0, 0x5A,0x5A}},
	{REGFLAG_CMD,2,{0xB0, 0x07}},
	{REGFLAG_CMD,2,{0xF4, 0x5B}},
	{REGFLAG_CMD,3,{0xF0, 0xA5,0xA5}},
#if 0
	/*seed setting*/
	{REGFLAG_CMD,3,{0x81, 0x31, 0x02}},
	{REGFLAG_CMD,3,{0xF0, 0x5A, 0x5A}},
	{REGFLAG_CMD,2,{0xB0, 0x2B}},
	{REGFLAG_CMD,22,{0xB1,0xE4,0x05,0x00,0x07,0xE0,0x01,0x0C,0x02,0xF0,0x18,0xFA,0xE3,0xFB,0x0A,0xDF,0xF9,0xF5,0x01,0xFF,0xFF,0xFF}},
	{REGFLAG_CMD,2,{0xB0, 0x00}},
	{REGFLAG_CMD,3,{0xF0, 0xA5, 0xA5}},
	{REGFLAG_DELAY,100,{}},
#endif
	/* Image Data Write for AOD Mode */
	/* Display on */
	{REGFLAG_CMD, 3, {0x9F, 0x5A,0x5A}},
	{REGFLAG_CMD, 1, {0x29}},
	{REGFLAG_CMD, 1, {0x13}},
	{REGFLAG_CMD, 3, {0x9F, 0xA5,0xA5}},

	{REGFLAG_END_OF_TABLE, 0x00, {}}
};

static int panel_doze_enable(void *dsi, dcs_write_gce cb, void *handle)
{
	unsigned int i=0;
#ifdef VENDOR_EDIT
	/* Hujie@PSW.MM.DisplayDriver.AOD, 2019/12/10, add for keylog*/
	pr_err("debug for lcm %s\n", __func__);
#endif

	for (i = 0; i < (sizeof(lcm_normal_to_aod_sam) / sizeof(struct LCM_setting_table)); i++) {
		unsigned cmd;
		cmd = lcm_normal_to_aod_sam[i].cmd;

		switch (cmd) {

			case REGFLAG_DELAY:
				msleep(lcm_normal_to_aod_sam[i].count);
				break;

			case REGFLAG_UDELAY:
				udelay(lcm_normal_to_aod_sam[i].count);
				break;

			case REGFLAG_END_OF_TABLE:
				break;

			default:
				cb(dsi, handle, lcm_normal_to_aod_sam[i].para_list, lcm_normal_to_aod_sam[i].count);
		}
	}

	return 0;
}

static int panel_doze_enable_start(void *dsi, dcs_write_gce cb, void *handle)
{
	int cmd = 0;
#ifdef VENDOR_EDIT
/* Hujie@PSW.MM.DisplayDriver.AOD, 2019/12/10, add for keylog*/
	pr_err("debug for lcm %s\n", __func__);
#endif

	cmd = 0x28;
	cb(dsi, handle, &cmd, 1);
	cmd = 0x10;
	cb(dsi, handle, &cmd, 1);

	return 0;
}

static int panel_doze_enable_end(void *dsi, dcs_write_gce cb, void *handle)
{
	int cmd = 0;
	int send_buf[3];
#ifdef VENDOR_EDIT
/* Hujie@PSW.MM.DisplayDriver.AOD, 2019/12/10, add for keylog*/
	pr_err("debug for lcm %s\n", __func__);
#endif

	cmd = 0x29;
	cb(dsi, handle, &cmd, 1);
	send_buf[0] = 0xF0;
	send_buf[1] = 0x5A;
	send_buf[2] = 0x5A;
	cb(dsi, handle, send_buf, 3);
	send_buf[0] = 0xF2;
	send_buf[1] = 0x0F;
	cb(dsi, handle, send_buf, 2);
	send_buf[0] = 0xF0;
	send_buf[1] = 0xA5;
	send_buf[2] = 0xA5;
	cb(dsi, handle, send_buf, 3);

	return 0;
}

static int panel_doze_area_set(void *dsi, dcs_write_gce cb, void *handle)
{
	int cmd = 0;

#ifdef VENDOR_EDIT
/* Hujie@PSW.MM.DisplayDriver.AOD, 2019/12/10, add for keylog*/
	pr_err("debug for lcm %s\n", __func__);
#endif

#if 0
	cmd = 0x28;
	cb(dsi, handle, &cmd, 1);
	msleep(12);
#endif
	cb(dsi, handle, send_cmd, ARRAY_SIZE(send_cmd));

	//memset(send_cmd, 0, RAMLESS_AOD_PAYLOAD_SIZE);
	return 0;
}

static struct LCM_setting_table lcm_normal_power_on_seq[] = {

	/*sleep out*/
	{REGFLAG_CMD, 1, {0x11}},
	{REGFLAG_DELAY, 20, {}},
	{REGFLAG_CMD, 3, {0x9F, 0x5A, 0x5A}},
	/*TE sync on*/
	{REGFLAG_CMD, 2, {0x35, 0x00}},
	{REGFLAG_CMD, 3, {0x9F, 0xA5, 0xA5}},
	/* PPS setting */
	{REGFLAG_CMD, 3, {0x9F, 0x5A, 0x5A}},
	{REGFLAG_CMD, 2, {0x9D, 0x01}},
	{REGFLAG_CMD, 89, {0x9E, 0x11, 0x00, 0x00, 0x89, 0x30, 0x80, 0x09, 0x60, 0x04,\
								  0x38, 0x00, 0x30, 0x02, 0x1C, 0x02, 0x1C, 0x02, 0x00, 0x02,\
								  0x52, 0x00, 0x20, 0x04, 0x23, 0x00, 0x07, 0x00, 0x0F, 0x02,\
								  0x8E, 0x02, 0x1F, 0x18, 0x00, 0x10, 0xF0, 0x03, 0x0C, 0x20,\
								  0x00, 0x06, 0x0B, 0x0B, 0x33, 0x0E, 0x1C, 0x2A, 0x38, 0x46,\
								  0x54, 0x62, 0x69, 0x70, 0x77, 0x79, 0x7B, 0x7D, 0x7E, 0x01,\
								  0x02, 0x01, 0x00, 0x09, 0x40, 0x09, 0xBE, 0x19, 0xFC, 0x19,\
								  0xFA, 0x19, 0xF8, 0x1A, 0x38, 0x1A, 0x78, 0x1A, 0xB6, 0x2A,\
								  0xB6, 0x2A, 0xF4, 0x2A, 0xF4, 0x4B, 0x34, 0x63, 0x74}},
	{REGFLAG_CMD, 3, {0x9F, 0xA5, 0xA5}},
	/* CASET/PASET Setting */
	{REGFLAG_CMD, 5, {0x2A, 0x00, 0x00, 0x04, 0x37}},
	{REGFLAG_CMD, 5, {0x2B, 0x00, 0x00, 0x09, 0x5F}},
	/* TSP SYNC ON */
	{REGFLAG_CMD, 3, {0xF0, 0x5A, 0x5A}},
	{REGFLAG_CMD, 2, {0xB0, 0x05}},
	{REGFLAG_CMD, 4, {0xD7, 0x07, 0x02, 0x40}},
	{REGFLAG_CMD, 3, {0xF0, 0xA5, 0xA5}},
	/* Driver-IC Initial Code */
	{REGFLAG_CMD, 3, {0xF0, 0x5A, 0x5A}},
	{REGFLAG_CMD, 3, {0xF1, 0x5A, 0x5A}},
	{REGFLAG_CMD, 4, {0xE5, 0x13, 0xE8, 0xFC}},
	{REGFLAG_CMD, 2, {0xED, 0x97}},
	{REGFLAG_CMD, 2, {0xB0, 0x1A}},
	{REGFLAG_CMD, 2, {0xF9, 0x20}},
	{REGFLAG_CMD, 3, {0xF1, 0xA5, 0xA5}},
	{REGFLAG_CMD, 3, {0xF0, 0xA5, 0xA5}},
	/* Dimming setting */
	{REGFLAG_CMD, 3, {0xF0, 0x5A, 0x5A}},
	{REGFLAG_CMD, 2, {0xB0, 0x06}},
	{REGFLAG_CMD, 2, {0xB7, 0x01}},
	{REGFLAG_CMD, 2, {0xB0, 0x05}},
	{REGFLAG_CMD, 2, {0xB7, 0x13}},
	{REGFLAG_CMD, 3, {0xF0, 0xA5, 0xA5}},
	/* ACL OFF*/
	{REGFLAG_CMD, 3, {0xF0, 0x5A, 0x5A}},
	{REGFLAG_CMD, 2, {0x55, 0x00}},
	{REGFLAG_CMD, 3, {0xF0, 0xA5, 0xA5}},
	/* 60FPS Setting*/
	{REGFLAG_CMD, 3, {0xF0, 0x5A, 0x5A}},
	{REGFLAG_CMD, 2, {0x60, 0x00}},
	{REGFLAG_CMD, 3, {0xF0, 0xA5, 0xA5}},
	{REGFLAG_CMD, 2, {0x53, 0x20}},
	{REGFLAG_DELAY, 130, {}},
	{REGFLAG_CMD, 3, {0x9F, 0x5A, 0x5A}},
	/* Display On*/
	{REGFLAG_CMD, 1, {0x29}},
	{REGFLAG_CMD, 1, {0x13}},
	{REGFLAG_CMD, 3, {0x9F, 0xA5, 0xA5}},

	{REGFLAG_END_OF_TABLE, 0x00, {}}
};

static int panel_doze_post_disp_on(void *dsi, dcs_write_gce cb, void *handle)
{

	unsigned int i=0;
#ifdef VENDOR_EDIT
	/* Hujie@PSW.MM.DisplayDriver.AOD, 2019/12/10, add for keylog*/
	pr_err("debug for lcm %s\n", __func__);
#endif

	for (i = 0; i < (sizeof(lcm_normal_power_on_seq) / sizeof(struct LCM_setting_table)); i++) {
		unsigned cmd;
		cmd = lcm_normal_power_on_seq[i].cmd;

		switch (cmd) {

			case REGFLAG_DELAY:
				msleep(lcm_normal_power_on_seq[i].count);
				break;

			case REGFLAG_UDELAY:
				udelay(lcm_normal_power_on_seq[i].count);
				break;

			case REGFLAG_END_OF_TABLE:
				break;

			default:
				cb(dsi, handle, lcm_normal_power_on_seq[i].para_list, lcm_normal_power_on_seq[i].count);
		}
	}

	return 0;
}


static int panel_doze_post_disp_off(void *dsi, dcs_write_gce cb, void *handle)
{

	int cmd = 0;

#ifdef VENDOR_EDIT
	/* Hujie@PSW.MM.DisplayDriver.AOD, 2019/12/10, add for keylog*/
	pr_err("debug for lcm %s\n", __func__);
#endif

	cmd = 0x28;
	cb(dsi, handle, &cmd, 1);

	return 0;
}

static int oppo_lcm_dc_backlight(void *dsi, dcs_write_gce cb,void *handle,unsigned int level,int hbm_en)
{
	int i, k;
	struct LCM_setting_table *seed_table;
	int seed_alpha = oppo_seed_bright_to_alpha(level);
	if (!oppo_dc_enable_real || hbm_en || level >= OPPO_DC_BACKLIGHT_THRESHOLD ||
	    level < 4) {
		goto dc_disable;
	}
    
	if (oppo_dc_alpha == seed_alpha)
		goto dc_enable;

	seed_table = kmemdup(lcm_seed_setting, sizeof(lcm_seed_setting), GFP_KERNEL);
	if (!seed_table)
		goto dc_disable;

	for (i = 0; i < sizeof(lcm_seed_setting)/sizeof(lcm_seed_setting[0]); i++) {
		if ((seed_table[i].para_list[0] == 0xB1) && (seed_table[i].count == 22)){
			for (k = 1; k < seed_table[i].count; k++) {
				seed_table[i].para_list[k]= seed_table[i].para_list[k] * (255 - seed_alpha) / 255;
			}
		}
	}

	for (i = 0; i < sizeof(lcm_seed_setting)/sizeof(lcm_seed_setting[0]); i++){
		cb(dsi, handle, seed_table[i].para_list, seed_table[i].count);
	}

	kfree(seed_table);
	if (!oppo_dc_alpha)
		pr_err("Enter DC");

	oppo_dc_alpha = seed_alpha;

dc_enable:
	return OPPO_DC_BACKLIGHT_THRESHOLD;

dc_disable:
	if (oppo_dc_alpha) {
		for (i = 0; i < sizeof(lcm_seed_setting)/sizeof(lcm_seed_setting[0]); i++){
			cb(dsi, handle, lcm_seed_setting[i].para_list, lcm_seed_setting[i].count);
		}
		pr_err("exit DC");
	}

	oppo_dc_alpha = 0;
	return level;
}

static int lcm_setbacklight_cmdq(void *dsi, dcs_write_gce cb,
		void *handle, unsigned int level)
{
	unsigned int mapped_level = 0;
	char bl_tb0[] = {0x51, 0x03, 0xFF};
	char bl_tb1[] = {0x53, 0x20};
	if (!cb)
		return -1;

	mapped_level = oppo_lcm_dc_backlight(dsi,cb,handle, level, 0);
	bl_tb0[1] = mapped_level >> 8;
	bl_tb0[2] = mapped_level & 0xFF;
	DDPPR_ERR("flag_writ=%d,mapped_level=%d\n",flag_writ,mapped_level);

#ifdef VENDOR_EDIT
	/* Hujie@PSW.MM.DisplayDriver.AOD, 2019/12/10, add for keylog for backlight value check*/
	pr_err("debug for display panel backlight value,func:=%s,level :=%d, mapped_level := %d\n", __func__, level, mapped_level);
#endif

	/*add for global hbm*/
	if(level <= 1023){
	    if(flag_writ == 3||flag_writ == 0){
			bl_tb1[1] = 0x20;
			cb(dsi, handle, bl_tb1, ARRAY_SIZE(bl_tb1));
			flag_hbm = 0;
		}
	    cb(dsi, handle, bl_tb0, ARRAY_SIZE(bl_tb0));
	} else if(level >= 1024 && level <= 2047){
	    if(flag_writ == 0||flag_writ == 3){
			bl_tb1[1] = 0xE0;
			cb(dsi, handle, bl_tb1, ARRAY_SIZE(bl_tb1));
			flag_hbm = 1;
			}
	    cb(dsi, handle, bl_tb0, ARRAY_SIZE(bl_tb0));
	}

	return 0;

}

static int lcm_panel_poweron(struct drm_panel *panel)
{
	struct lcm *ctx = panel_to_lcm(panel);
	int ret;

#ifdef VENDOR_EDIT
	/* Hujie@PSW.MM.DisplayDriver.AOD, 2019/12/10, add for keylog*/
	pr_err("debug for lcm %s\n", __func__);
#endif

	if (ctx->prepared)
		return 0;

	ctx->bias_gpio = devm_gpiod_get(ctx->dev, "bias", GPIOD_OUT_HIGH);
	gpiod_set_value(ctx->bias_gpio, 1);
	devm_gpiod_put(ctx->dev, ctx->bias_gpio);
	msleep(5);
    lcm_panel_pm8008_ldo1_enable(ctx->dev);
    msleep(10);
	lcm_panel_ldo3_enable(ctx->dev);
	msleep(10);
	ctx->reset_gpio = devm_gpiod_get(ctx->dev, "reset", GPIOD_OUT_HIGH);
	gpiod_set_value(ctx->reset_gpio, 1);
	msleep(5);
	gpiod_set_value(ctx->reset_gpio, 0);
	msleep(5);
	gpiod_set_value(ctx->reset_gpio, 1);
	msleep(10);
	devm_gpiod_put(ctx->dev, ctx->reset_gpio);
	msleep(20);

	ret = ctx->error;
	if (ret < 0)
		lcm_unprepare(panel);
	return 0;
}

static int lcm_panel_poweroff(struct drm_panel *panel)
{
	struct lcm *ctx = panel_to_lcm(panel);
	int ret;

#ifdef VENDOR_EDIT
	/* Hujie@PSW.MM.DisplayDriver.AOD, 2019/12/10, add for keylog*/
	pr_err("debug for lcm %s  ctx->prepared %d \n", __func__,ctx->prepared);
#endif

	if (ctx->prepared)
		return 0;

	ctx->reset_gpio = devm_gpiod_get(ctx->dev, "reset", GPIOD_OUT_HIGH);
	gpiod_set_value(ctx->reset_gpio, 0);
	devm_gpiod_put(ctx->dev, ctx->reset_gpio);
	msleep(5);
    lcm_panel_pm8008_ldo1_disable(ctx->dev);
    msleep(5);
	lcm_panel_ldo3_disable(ctx->dev);
	msleep(10);
	ctx->bias_gpio = devm_gpiod_get(ctx->dev, "bias", GPIOD_OUT_HIGH);
	gpiod_set_value(ctx->bias_gpio, 0);
    msleep(5);
	devm_gpiod_put(ctx->dev, ctx->bias_gpio);

	ret = ctx->error;
	if (ret < 0)
		lcm_unprepare(panel);

	msleep(110);
	return 0;
}

static int lcm_panel_disp_off(void *dsi, dcs_write_gce cb, void *handle)
{
	int cmd = 0;

	DDPINFO("%s\n", __func__);

	cmd = 0x28;
	cb(dsi, handle, &cmd, 1);
	msleep(10);

	cmd = 0x10;
	cb(dsi, handle, &cmd, 1);
	msleep(120);

	return 0;
}


static void lcm_setbrightness(void *dsi,
			      dcs_write_gce cb, void *handle, unsigned int level)
{
	unsigned int BL_MSB = 0;
	unsigned int BL_LSB = 0;
	unsigned int hbm_brightness = 0;
	int i = 0;

	printk("[soso] %s level is %d\n", __func__, level);

	if (level > BRIGHTNESS_HALF) {
		hbm_brightness = level - 1024;
		BL_LSB = hbm_brightness >> 8;
		BL_MSB = hbm_brightness & 0xFF;

		lcm_setbrightness_hbm[0].para_list[1] = BL_LSB;
		lcm_setbrightness_hbm[0].para_list[2] = BL_MSB;

		for (i = 0; i < sizeof(lcm_setbrightness_hbm)/sizeof(struct LCM_setting_table); i++){
			cb(dsi, handle, lcm_setbrightness_hbm[i].para_list, lcm_setbrightness_hbm[i].count);
		}
	} else {
		BL_LSB = level >> 8;
		BL_MSB = level & 0xFF;

		lcm_setbrightness_normal[1].para_list[1] = BL_LSB;
		lcm_setbrightness_normal[1].para_list[2] = BL_MSB;

		for (i = 0; i < sizeof(lcm_setbrightness_normal)/sizeof(struct LCM_setting_table); i++){
			cb(dsi, handle, lcm_setbrightness_normal[i].para_list, lcm_setbrightness_normal[i].count);
		}
	}
}

static int lcm_set_hbm(void *dsi, dcs_write_gce cb,
		void *handle, unsigned int hbm_mode)
{
	int i = 0;
	int level = 0;
	if (!cb)
		return -1;

	pr_err("oppo_display_brightness= %ld, hbm_mode=%u\n", oppo_display_brightness, hbm_mode);

	if(hbm_mode == 1) {
		oppo_lcm_dc_backlight(dsi,cb,handle, oppo_display_brightness, 1);
		for (i = 0; i < sizeof(lcm_finger_HBM_on_setting)/sizeof(struct LCM_setting_table); i++){
			cb(dsi, handle, lcm_finger_HBM_on_setting[i].para_list, lcm_finger_HBM_on_setting[i].count);
		}
	} else if (hbm_mode == 0) {
		level = oppo_lcm_dc_backlight(dsi,cb,handle, oppo_display_brightness, 0);
		lcm_setbrightness(dsi, cb, handle, level);
		printk("[soso] %s : %d ! backlight %d !\n",__func__, hbm_mode, level);
	}

	return 0;
}

static int panel_hbm_set_cmdq(struct drm_panel *panel, void *dsi,
			      dcs_write_gce cb, void *handle, bool en)
{
	//char hbm_tb[] = {0x53, 0xe0};
	struct lcm *ctx = panel_to_lcm(panel);
	int i = 0;
	int level = 0;
	if (!cb)
		return -1;
	if (ctx->hbm_en == en)
		goto done;

	pr_err("debug for oppo_display_brightness= %ld, en=%u\n", oppo_display_brightness, en);

	if(en == 1) {
		oppo_lcm_dc_backlight(dsi,cb,handle, oppo_display_brightness, 1);
		for (i = 0; i < sizeof(lcm_finger_HBM_on_setting)/sizeof(struct LCM_setting_table); i++){
			cb(dsi, handle, lcm_finger_HBM_on_setting[i].para_list, lcm_finger_HBM_on_setting[i].count);
		}
	} else if (en == 0) {
		level = oppo_lcm_dc_backlight(dsi,cb,handle, oppo_display_brightness, 0);
		lcm_setbrightness(dsi, cb, handle, level);
		printk("[soso] %s : %d ! backlight %d !\n",__func__, en, level);
		flag_hbm = 0;
	}
	ctx->hbm_en = en;
	ctx->hbm_wait = true;
done:
	return 0;
}

static void panel_hbm_get_state(struct drm_panel *panel, bool *state)
{
	struct lcm *ctx = panel_to_lcm(panel);

	*state = ctx->hbm_en;
}

static void panel_hbm_set_state(struct drm_panel *panel, bool state)
{
	struct lcm *ctx = panel_to_lcm(panel);

	ctx->hbm_en = state;
}

static void panel_hbm_get_wait_state(struct drm_panel *panel, bool *wait)
{
	struct lcm *ctx = panel_to_lcm(panel);

	*wait = ctx->hbm_wait;
}

static bool panel_hbm_set_wait_state(struct drm_panel *panel, bool wait)
{
	struct lcm *ctx = panel_to_lcm(panel);
	bool old = ctx->hbm_wait;

	ctx->hbm_wait = wait;
	return old;
}

static bool panel_no_video_cmd_switch_state(struct drm_panel *panel)
{
	struct lcm *ctx = panel_to_lcm(panel);
	ctx->cv_state = true;

    return ctx->cv_state;
}

static struct mtk_panel_funcs ext_funcs = {
	.reset = panel_ext_reset,
	.set_backlight_cmdq = lcm_setbacklight_cmdq,
	.doze_enable = panel_doze_enable,
	.doze_disable = panel_doze_disable,
	.doze_post_disp_on = panel_doze_post_disp_on,
	.set_hbm = lcm_set_hbm,
	.panel_poweron = lcm_panel_poweron,
	.panel_poweroff = lcm_panel_poweroff,
	.panel_disp_off = lcm_panel_disp_off,
	.hbm_set_cmdq = panel_hbm_set_cmdq,
	.hbm_get_state = panel_hbm_get_state,
	.hbm_set_state = panel_hbm_set_state,
	.hbm_get_wait_state = panel_hbm_get_wait_state,
	.hbm_set_wait_state = panel_hbm_set_wait_state,
	.panel_no_cv_switch = panel_no_video_cmd_switch_state,
	.ext_param_set = mtk_panel_ext_param_set,
	.mode_switch = mode_switch,

};
#endif

struct panel_desc {
	const struct drm_display_mode *modes;
	unsigned int num_modes;

	unsigned int bpc;

	struct {
		unsigned int width;
		unsigned int height;
	} size;

	struct {
		unsigned int prepare;
		unsigned int enable;
		unsigned int disable;
		unsigned int unprepare;
	} delay;
};

static int lcm_get_modes(struct drm_panel *panel)
{
	struct drm_display_mode *mode;
	struct drm_display_mode *mode2;

	mode = drm_mode_duplicate(panel->drm, &default_mode);
	if (!mode) {
		dev_err(panel->drm->dev, "failed to add mode %ux%ux@%u\n",
			default_mode.hdisplay, default_mode.vdisplay,
			default_mode.vrefresh);
		return -ENOMEM;
	}

	drm_mode_set_name(mode);
	mode->type = DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED;
	drm_mode_probed_add(panel->connector, mode);

	mode2 = drm_mode_duplicate(panel->drm, &performance_mode);
	pr_err("debug for lcm_get_modes= %s, en=%u\n", __func__, mode);
	if (!mode2) {
		dev_err(panel->drm->dev, "failed to add mode %ux%ux@%u\n",
			performance_mode.hdisplay,
			performance_mode.vdisplay,
			performance_mode.vrefresh);
		return -ENOMEM;
	}

	drm_mode_set_name(mode2);
	mode2->type = DRM_MODE_TYPE_DRIVER;
	drm_mode_probed_add(panel->connector, mode2);

	panel->connector->display_info.width_mm = 67;
	panel->connector->display_info.height_mm = 149;

	return 1;
}

static const struct drm_panel_funcs lcm_drm_funcs = {
	.disable = lcm_disable,
	.unprepare = lcm_unprepare,
	.prepare = lcm_prepare,
	.enable = lcm_enable,
	.get_modes = lcm_get_modes,
};

static int lcm_probe(struct mipi_dsi_device *dsi)
{
	struct device *dev = &dsi->dev;
	struct lcm *ctx;
	struct device_node *backlight;
	int ret;
	ctx = devm_kzalloc(dev, sizeof(struct lcm), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;

	mipi_dsi_set_drvdata(dsi, ctx);

	ctx->dev = dev;
	dsi->lanes = 4;
	dsi->format = MIPI_DSI_FMT_RGB888;

#if (LCM_DSI_CMD_MODE)
	dsi->mode_flags = MIPI_DSI_MODE_LPM | MIPI_DSI_MODE_EOT_PACKET
			 | MIPI_DSI_CLOCK_NON_CONTINUOUS;
#else
	dsi->mode_flags = MIPI_DSI_MODE_VIDEO | MIPI_DSI_MODE_VIDEO_BURST
			 | MIPI_DSI_MODE_LPM | MIPI_DSI_MODE_EOT_PACKET
			 | MIPI_DSI_CLOCK_NON_CONTINUOUS;

#endif
	backlight = of_parse_phandle(dev->of_node, "backlight", 0);
	if (backlight) {
		ctx->backlight = of_find_backlight_by_node(backlight);
		of_node_put(backlight);

		if (!ctx->backlight)
			return -EPROBE_DEFER;
	}
	ctx->bias_gpio = devm_gpiod_get(ctx->dev, "bias", GPIOD_OUT_HIGH);
	gpiod_set_value(ctx->bias_gpio, 1);
	devm_gpiod_put(ctx->dev, ctx->bias_gpio);
	msleep(2);
	lcm_panel_pm8008_ldo1_enable(ctx->dev);
	msleep(2);
	lcm_panel_ldo3_enable(ctx->dev);
	msleep(5);

	ctx->reset_gpio = devm_gpiod_get(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(ctx->reset_gpio)) {
		dev_err(dev, "cannot get reset-gpios %ld\n",
			PTR_ERR(ctx->reset_gpio));
		return PTR_ERR(ctx->reset_gpio);
	}
	devm_gpiod_put(dev, ctx->reset_gpio);
	ctx->prepared = true;
	ctx->enabled = true;

	drm_panel_init(&ctx->panel);
	ctx->panel.dev = dev;
	ctx->panel.funcs = &lcm_drm_funcs;

	ret = drm_panel_add(&ctx->panel);
	if (ret < 0)
		return ret;

	ret = mipi_dsi_attach(dsi);
	if (ret < 0)
		drm_panel_remove(&ctx->panel);

#if defined(CONFIG_MTK_PANEL_EXT)
	ret = mtk_panel_ext_create(dev, &ext_params, &ext_funcs, &ctx->panel);
	if (ret < 0)
		return ret;
#endif

	ctx->hbm_en = false;
	pr_info("%s zjb+\n", __func__);

	register_device_proc("lcd", "AMS644VA04_MTK04", "samsung1024");
	pr_info("%s-\n", __func__);

	return ret;
}

static int lcm_remove(struct mipi_dsi_device *dsi)
{
	struct lcm *ctx = mipi_dsi_get_drvdata(dsi);

	mipi_dsi_detach(dsi);
	drm_panel_remove(&ctx->panel);

	return 0;
}

static const struct of_device_id lcm_of_match[] = {
	{ .compatible = "oppo19040_samsung_ana6705_1080p_dsi_cmd,lcm", },
	{ }
};

MODULE_DEVICE_TABLE(of, lcm_of_match);

static struct mipi_dsi_driver lcm_driver = {
	.probe = lcm_probe,
	.remove = lcm_remove,
	.driver = {
		.name = "oppo19040_samsung_ana6705_1080p_dsi_cmd,lcm",
		.owner = THIS_MODULE,
		.of_match_table = lcm_of_match,
	},
};

module_mipi_dsi_driver(lcm_driver);
