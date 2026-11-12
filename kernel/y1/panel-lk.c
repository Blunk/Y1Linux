// SPDX-License-Identifier: GPL-2.0-only
/*
 * Innioasis Y1: the ST7701S panel exactly as LK left it (docs/planning/Y1-DISPLAY-PLAN.md, stage B).
 * No reset, no DCS command, no supply: prepare, enable, disable and unprepare do nothing, so the panel
 * keeps LK's initialisation while Linux takes over the DSI link. The mode and link format are LK's
 * (stock st7701_hvga_dsi_vdo_boe lcm_get_params, and LK's DSI and MIPI TX registers, display stage A):
 * 480 x 360, sync-pulse video, 2 lanes, packed RGB888, EoT on, continuous clock; blanking in pixels
 * HSA 10, HBP 80, HFP 80, VSA 8, VBP 100, VFP 100. The 16.25 MHz pixel clock gives 195 Mbit/s per lane,
 * for which the MT8173-style MIPI TX code programs TXDIV0 2 and PCW 60, LK's own values (about 44 Hz).
 */
#include <linux/module.h>
#include <linux/of.h>
#include <drm/drm_mipi_dsi.h>
#include <drm/drm_modes.h>
#include <drm/drm_panel.h>
#include <drm/drm_probe_helper.h>

struct y1_lk_panel {
	struct drm_panel panel;
};

static const struct drm_display_mode y1_lk_mode = {
	.clock = 16250,
	.hdisplay = 480,
	.hsync_start = 480 + 80,
	.hsync_end = 480 + 80 + 10,
	.htotal = 480 + 80 + 10 + 80,
	.vdisplay = 360,
	.vsync_start = 360 + 100,
	.vsync_end = 360 + 100 + 8,
	.vtotal = 360 + 100 + 8 + 100,
	.type = DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED,
};

static int y1_lk_panel_get_modes(struct drm_panel *panel, struct drm_connector *connector)
{
	return drm_connector_helper_get_modes_fixed(connector, &y1_lk_mode);
}

static const struct drm_panel_funcs y1_lk_panel_funcs = {
	.get_modes = y1_lk_panel_get_modes,
};

static int y1_lk_panel_probe(struct mipi_dsi_device *dsi)
{
	struct device *dev = &dsi->dev;
	struct y1_lk_panel *ctx;
	int ret;

	ctx = devm_drm_panel_alloc(dev, struct y1_lk_panel, panel, &y1_lk_panel_funcs,
				   DRM_MODE_CONNECTOR_DSI);
	if (IS_ERR(ctx))
		return PTR_ERR(ctx);
	dsi->lanes = 2;
	dsi->format = MIPI_DSI_FMT_RGB888;
	dsi->mode_flags = MIPI_DSI_MODE_VIDEO | MIPI_DSI_MODE_VIDEO_SYNC_PULSE;
	mipi_dsi_set_drvdata(dsi, ctx);
	drm_panel_add(&ctx->panel);
	ret = mipi_dsi_attach(dsi);
	if (ret) {
		drm_panel_remove(&ctx->panel);
		return dev_err_probe(dev, ret, "DSI attach\n");
	}
	dev_info(dev, "LK's panel adopted as initialised: 480x360, no reset, no commands\n");
	return 0;
}

static void y1_lk_panel_remove(struct mipi_dsi_device *dsi)
{
	struct y1_lk_panel *ctx = mipi_dsi_get_drvdata(dsi);

	mipi_dsi_detach(dsi);
	drm_panel_remove(&ctx->panel);
}

static const struct of_device_id y1_lk_panel_of_match[] = {
	{ .compatible = "innioasis,y1-lk-panel" },
	{ }
};
MODULE_DEVICE_TABLE(of, y1_lk_panel_of_match);

static struct mipi_dsi_driver y1_lk_panel_driver = {
	.probe = y1_lk_panel_probe,
	.remove = y1_lk_panel_remove,
	.driver = {
		.name = "panel-innioasis-y1-lk",
		.of_match_table = y1_lk_panel_of_match,
	},
};
module_mipi_dsi_driver(y1_lk_panel_driver);

MODULE_DESCRIPTION("Innioasis Y1 panel as initialised by LK");
MODULE_LICENSE("GPL");
