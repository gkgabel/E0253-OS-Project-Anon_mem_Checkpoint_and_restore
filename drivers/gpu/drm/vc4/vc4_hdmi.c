// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2015 Broadcom
 * Copyright (c) 2014 The Linux Foundation. All rights reserved.
 * Copyright (C) 2013 Red Hat
 * Author: Rob Clark <robdclark@gmail.com>
 */

/**
 * DOC: VC4 Falcon HDMI module
 *
 * The HDMI core has a state machine and a PHY.  On BCM2835, most of
 * the unit operates off of the HSM clock from CPRMAN.  It also
 * internally uses the PLLH_PIX clock for the PHY.
 *
 * HDMI infoframes are kept within a small packet ram, where each
 * packet can be individually enabled for including in a frame.
 *
 * HDMI audio is implemented entirely within the HDMI IP block.  A
 * register in the HDMI encoder takes SPDIF frames from the DMA engine
 * and transfers them over an internal MAI (multi-channel audio
 * interconnect) bus to the encoder side for insertion into the video
 * blank regions.
 *
 * The driver's HDMI encoder does not yet support power management.
 * The HDMI encoder's power domain and the HSM/pixel clocks are kept
 * continuously running, and only the HDMI logic and packet ram are
 * powered off/on at disable/enable time.
 *
 * The driver does not yet support CEC control, though the HDMI
 * encoder block has CEC support.
 */

#include <drm/display/drm_hdmi_helper.h>
#include <drm/display/drm_scdc_helper.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_probe_helper.h>
#include <drm/drm_simple_kms_helper.h>
#include <linux/clk.h>
#include <linux/component.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/of_address.h>
#include <linux/of_gpio.h>
#include <linux/of_platform.h>
#include <linux/pm_runtime.h>
#include <linux/rational.h>
#include <linux/reset.h>
#include <sound/dmaengine_pcm.h>
#include <sound/hdmi-codec.h>
#include <sound/pcm_drm_eld.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>
#include "media/cec.h"
#include "vc4_drv.h"
#include "vc4_hdmi.h"
#include "vc4_hdmi_regs.h"
#include "vc4_regs.h"

#define VC5_HDMI_HORZA_HFP_SHIFT		16
#define VC5_HDMI_HORZA_HFP_MASK			VC4_MASK(28, 16)
#define VC5_HDMI_HORZA_VPOS			BIT(15)
#define VC5_HDMI_HORZA_HPOS			BIT(14)
#define VC5_HDMI_HORZA_HAP_SHIFT		0
#define VC5_HDMI_HORZA_HAP_MASK			VC4_MASK(13, 0)

#define VC5_HDMI_HORZB_HBP_SHIFT		16
#define VC5_HDMI_HORZB_HBP_MASK			VC4_MASK(26, 16)
#define VC5_HDMI_HORZB_HSP_SHIFT		0
#define VC5_HDMI_HORZB_HSP_MASK			VC4_MASK(10, 0)

#define VC5_HDMI_VERTA_VSP_SHIFT		24
#define VC5_HDMI_VERTA_VSP_MASK			VC4_MASK(28, 24)
#define VC5_HDMI_VERTA_VFP_SHIFT		16
#define VC5_HDMI_VERTA_VFP_MASK			VC4_MASK(22, 16)
#define VC5_HDMI_VERTA_VAL_SHIFT		0
#define VC5_HDMI_VERTA_VAL_MASK			VC4_MASK(12, 0)

#define VC5_HDMI_VERTB_VSPO_SHIFT		16
#define VC5_HDMI_VERTB_VSPO_MASK		VC4_MASK(29, 16)

#define VC4_HDMI_MISC_CONTROL_PIXEL_REP_SHIFT	0
#define VC4_HDMI_MISC_CONTROL_PIXEL_REP_MASK	VC4_MASK(3, 0)
#define VC5_HDMI_MISC_CONTROL_PIXEL_REP_SHIFT	0
#define VC5_HDMI_MISC_CONTROL_PIXEL_REP_MASK	VC4_MASK(3, 0)

#define VC5_HDMI_SCRAMBLER_CTL_ENABLE		BIT(0)

#define VC5_HDMI_DEEP_COLOR_CONFIG_1_INIT_PACK_PHASE_SHIFT	8
#define VC5_HDMI_DEEP_COLOR_CONFIG_1_INIT_PACK_PHASE_MASK	VC4_MASK(10, 8)

#define VC5_HDMI_DEEP_COLOR_CONFIG_1_COLOR_DEPTH_SHIFT		0
#define VC5_HDMI_DEEP_COLOR_CONFIG_1_COLOR_DEPTH_MASK		VC4_MASK(3, 0)

#define VC5_HDMI_GCP_CONFIG_GCP_ENABLE		BIT(31)

#define VC5_HDMI_GCP_WORD_1_GCP_SUBPACKET_BYTE_1_SHIFT	8
#define VC5_HDMI_GCP_WORD_1_GCP_SUBPACKET_BYTE_1_MASK	VC4_MASK(15, 8)

# define VC4_HD_M_SW_RST			BIT(2)
# define VC4_HD_M_ENABLE			BIT(0)

#define HSM_MIN_CLOCK_FREQ	120000000
#define CEC_CLOCK_FREQ 40000

#define HDMI_14_MAX_TMDS_CLK   (340 * 1000 * 1000)

static const char * const output_format_str[] = {
	[VC4_HDMI_OUTPUT_RGB]		= "RGB",
	[VC4_HDMI_OUTPUT_YUV420]	= "YUV 4:2:0",
	[VC4_HDMI_OUTPUT_YUV422]	= "YUV 4:2:2",
	[VC4_HDMI_OUTPUT_YUV444]	= "YUV 4:4:4",
};

static const char *vc4_hdmi_output_fmt_str(enum vc4_hdmi_output_format fmt)
{
	if (fmt >= ARRAY_SIZE(output_format_str))
		return "invalid";

	return output_format_str[fmt];
}

static unsigned long long
vc4_hdmi_encoder_compute_mode_clock(const struct drm_display_mode *mode,
				    unsigned int bpc, enum vc4_hdmi_output_format fmt);

static bool vc4_hdmi_mode_needs_scrambling(const struct drm_display_mode *mode,
					   unsigned int bpc,
					   enum vc4_hdmi_output_format fmt)
{
	unsigned long long clock = vc4_hdmi_encoder_compute_mode_clock(mode, bpc, fmt);

	return clock > HDMI_14_MAX_TMDS_CLK;
}

static bool vc4_hdmi_is_full_range_rgb(struct vc4_hdmi *vc4_hdmi,
				       const struct drm_display_mode *mode)
{
	struct drm_display_info *display = &vc4_hdmi->connector.display_info;

	return !display->is_hdmi ||
		drm_default_rgb_quant_range(mode) == HDMI_QUANTIZATION_RANGE_FULL;
}

static int vc4_hdmi_debugfs_regs(struct seq_file *m, void *unused)
{
	struct drm_info_node *node = (struct drm_info_node *)m->private;
	struct vc4_hdmi *vc4_hdmi = node->info_ent->data;
	struct drm_printer p = drm_seq_file_printer(m);

	drm_print_regset32(&p, &vc4_hdmi->hdmi_regset);
	drm_print_regset32(&p, &vc4_hdmi->hd_regset);
	drm_print_regset32(&p, &vc4_hdmi->cec_regset);
	drm_print_regset32(&p, &vc4_hdmi->csc_regset);
	drm_print_regset32(&p, &vc4_hdmi->dvp_regset);
	drm_print_regset32(&p, &vc4_hdmi->phy_regset);
	drm_print_regset32(&p, &vc4_hdmi->ram_regset);
	drm_print_regset32(&p, &vc4_hdmi->rm_regset);

	return 0;
}

static void vc4_hdmi_reset(struct vc4_hdmi *vc4_hdmi)
{
	unsigned long flags;

	spin_lock_irqsave(&vc4_hdmi->hw_lock, flags);

	HDMI_WRITE(HDMI_M_CTL, VC4_HD_M_SW_RST);
	udelay(1);
	HDMI_WRITE(HDMI_M_CTL, 0);

	HDMI_WRITE(HDMI_M_CTL, VC4_HD_M_ENABLE);

	HDMI_WRITE(HDMI_SW_RESET_CONTROL,
		   VC4_HDMI_SW_RESET_HDMI |
		   VC4_HDMI_SW_RESET_FORMAT_DETECT);

	HDMI_WRITE(HDMI_SW_RESET_CONTROL, 0);

	spin_unlock_irqrestore(&vc4_hdmi->hw_lock, flags);
}

static void vc5_hdmi_reset(struct vc4_hdmi *vc4_hdmi)
{
	unsigned long flags;

	reset_control_reset(vc4_hdmi->reset);

	spin_lock_irqsave(&vc4_hdmi->hw_lock, flags);

	HDMI_WRITE(HDMI_DVP_CTL, 0);

	HDMI_WRITE(HDMI_CLOCK_STOP,
		   HDMI_READ(HDMI_CLOCK_STOP) | VC4_DVP_HT_CLOCK_STOP_PIXEL);

	spin_unlock_irqrestore(&vc4_hdmi->hw_lock, flags);
}

#ifdef CONFIG_DRM_VC4_HDMI_CEC
static void vc4_hdmi_cec_update_clk_div(struct vc4_hdmi *vc4_hdmi)
{
	unsigned long cec_rate = clk_get_rate(vc4_hdmi->cec_clock);
	unsigned long flags;
	u16 clk_cnt;
	u32 value;

	spin_lock_irqsave(&vc4_hdmi->hw_lock, flags);

	value = HDMI_READ(HDMI_CEC_CNTRL_1);
	value &= ~VC4_HDMI_CEC_DIV_CLK_CNT_MASK;

	/*
	 * Set the clock divider: the hsm_clock rate and this divider
	 * setting will give a 40 kHz CEC clock.
	 */
	clk_cnt = cec_rate / CEC_CLOCK_FREQ;
	value |= clk_cnt << VC4_HDMI_CEC_DIV_CLK_CNT_SHIFT;
	HDMI_WRITE(HDMI_CEC_CNTRL_1, value);

	spin_unlock_irqrestore(&vc4_hdmi->hw_lock, flags);
}
#else
static void vc4_hdmi_cec_update_clk_div(struct vc4_hdmi *vc4_hdmi) {}
#endif

static void vc4_hdmi_enable_scrambling(struct drm_encoder *encoder);

static enum drm_connector_status
vc4_hdmi_connector_detect(struct drm_connector *connector, bool force)
{
	struct vc4_hdmi *vc4_hdmi = connector_to_vc4_hdmi(connector);
	bool connected = false;

	mutex_lock(&vc4_hdmi->mutex);

	WARN_ON(pm_runtime_resume_and_get(&vc4_hdmi->pdev->dev));

	if (vc4_hdmi->hpd_gpio) {
		if (gpiod_get_value_cansleep(vc4_hdmi->hpd_gpio))
			connected = true;
	} else {
		if (vc4_hdmi->variant->hp_detect &&
		    vc4_hdmi->variant->hp_detect(vc4_hdmi))
			connected = true;
	}

	if (connected) {
		if (connector->status != connector_status_connected) {
			struct edid *edid = drm_get_edid(connector, vc4_hdmi->ddc);

			if (edid) {
				cec_s_phys_addr_from_edid(vc4_hdmi->cec_adap, edid);
				kfree(edid);
			}
		}

		vc4_hdmi_enable_scrambling(&vc4_hdmi->encoder.base);
		pm_runtime_put(&vc4_hdmi->pdev->dev);
		mutex_unlock(&vc4_hdmi->mutex);
		return connector_status_connected;
	}

	cec_phys_addr_invalidate(vc4_hdmi->cec_adap);
	pm_runtime_put(&vc4_hdmi->pdev->dev);
	mutex_unlock(&vc4_hdmi->mutex);
	return connector_status_disconnected;
}

static void vc4_hdmi_connector_destroy(struct drm_connector *connector)
{
	drm_connector_unregister(connector);
	drm_connector_cleanup(connector);
}

static int vc4_hdmi_connector_get_modes(struct drm_connector *connector)
{
	struct vc4_hdmi *vc4_hdmi = connector_to_vc4_hdmi(connector);
	int ret = 0;
	struct edid *edid;

	mutex_lock(&vc4_hdmi->mutex);

	edid = drm_get_edid(connector, vc4_hdmi->ddc);
	cec_s_phys_addr_from_edid(vc4_hdmi->cec_adap, edid);
	if (!edid) {
		ret = -ENODEV;
		goto out;
	}

	drm_connector_update_edid_property(connector, edid);
	ret = drm_add_edid_modes(connector, edid);
	kfree(edid);

	if (vc4_hdmi->disable_4kp60) {
		struct drm_device *drm = connector->dev;
		struct drm_display_mode *mode;

		list_for_each_entry(mode, &connector->probed_modes, head) {
			if (vc4_hdmi_mode_needs_scrambling(mode, 8, VC4_HDMI_OUTPUT_RGB)) {
				drm_warn_once(drm, "The core clock cannot reach frequencies high enough to support 4k @ 60Hz.");
				drm_warn_once(drm, "Please change your config.txt file to add hdmi_enable_4kp60.");
			}
		}
	}

out:
	mutex_unlock(&vc4_hdmi->mutex);

	return ret;
}

static int vc4_hdmi_connector_atomic_check(struct drm_connector *connector,
					   struct drm_atomic_state *state)
{
	struct drm_connector_state *old_state =
		drm_atomic_get_old_connector_state(state, connector);
	struct drm_connector_state *new_state =
		drm_atomic_get_new_connector_state(state, connector);
	struct drm_crtc *crtc = new_state->crtc;

	if (!crtc)
		return 0;

	if (old_state->colorspace != new_state->colorspace ||
	    !drm_connector_atomic_hdr_metadata_equal(old_state, new_state)) {
		struct drm_crtc_state *crtc_state;

		crtc_state = drm_atomic_get_crtc_state(state, crtc);
		if (IS_ERR(crtc_state))
			return PTR_ERR(crtc_state);

		crtc_state->mode_changed = true;
	}

	return 0;
}

static void vc4_hdmi_connector_reset(struct drm_connector *connector)
{
	struct vc4_hdmi_connector_state *old_state =
		conn_state_to_vc4_hdmi_conn_state(connector->state);
	struct vc4_hdmi_connector_state *new_state =
		kzalloc(sizeof(*new_state), GFP_KERNEL);

	if (connector->state)
		__drm_atomic_helper_connector_destroy_state(connector->state);

	kfree(old_state);
	__drm_atomic_helper_connector_reset(connector, &new_state->base);

	if (!new_state)
		return;

	new_state->base.max_bpc = 8;
	new_state->base.max_requested_bpc = 8;
	new_state->output_format = VC4_HDMI_OUTPUT_RGB;
	drm_atomic_helper_connector_tv_reset(connector);
}

static struct drm_connector_state *
vc4_hdmi_connector_duplicate_state(struct drm_connector *connector)
{
	struct drm_connector_state *conn_state = connector->state;
	struct vc4_hdmi_connector_state *vc4_state = conn_state_to_vc4_hdmi_conn_state(conn_state);
	struct vc4_hdmi_connector_state *new_state;

	new_state = kzalloc(sizeof(*new_state), GFP_KERNEL);
	if (!new_state)
		return NULL;

	new_state->tmds_char_rate = vc4_state->tmds_char_rate;
	new_state->output_bpc = vc4_state->output_bpc;
	new_state->output_format = vc4_state->output_format;
	__drm_atomic_helper_connector_duplicate_state(connector, &new_state->base);

	return &new_state->base;
}

static const struct drm_connector_funcs vc4_hdmi_connector_funcs = {
	.detect = vc4_hdmi_connector_detect,
	.fill_modes = drm_helper_probe_single_connector_modes,
	.destroy = vc4_hdmi_connector_destroy,
	.reset = vc4_hdmi_connector_reset,
	.atomic_duplicate_state = vc4_hdmi_connector_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_connector_destroy_state,
};

static const struct drm_connector_helper_funcs vc4_hdmi_connector_helper_funcs = {
	.get_modes = vc4_hdmi_connector_get_modes,
	.atomic_check = vc4_hdmi_connector_atomic_check,
};

static int vc4_hdmi_connector_init(struct drm_device *dev,
				   struct vc4_hdmi *vc4_hdmi)
{
	struct drm_connector *connector = &vc4_hdmi->connector;
	struct drm_encoder *encoder = &vc4_hdmi->encoder.base;
	int ret;

	drm_connector_init_with_ddc(dev, connector,
				    &vc4_hdmi_connector_funcs,
				    DRM_MODE_CONNECTOR_HDMIA,
				    vc4_hdmi->ddc);
	drm_connector_helper_add(connector, &vc4_hdmi_connector_helper_funcs);

	/*
	 * Some of the properties below require access to state, like bpc.
	 * Allocate some default initial connector state with our reset helper.
	 */
	if (connector->funcs->reset)
		connector->funcs->reset(connector);

	/* Create and attach TV margin props to this connector. */
	ret = drm_mode_create_tv_margin_properties(dev);
	if (ret)
		return ret;

	ret = drm_mode_create_hdmi_colorspace_property(connector);
	if (ret)
		return ret;

	drm_connector_attach_colorspace_property(connector);
	drm_connector_attach_tv_margin_properties(connector);
	drm_connector_attach_max_bpc_property(connector, 8, 12);

	connector->polled = (DRM_CONNECTOR_POLL_CONNECT |
			     DRM_CONNECTOR_POLL_DISCONNECT);

	connector->interlace_allowed = 1;
	connector->doublescan_allowed = 0;
	connector->stereo_allowed = 1;

	if (vc4_hdmi->variant->supports_hdr)
		drm_connector_attach_hdr_output_metadata_property(connector);

	drm_connector_attach_encoder(connector, encoder);

	return 0;
}

static int vc4_hdmi_stop_packet(struct drm_encoder *encoder,
				enum hdmi_infoframe_type type,
				bool poll)
{
	struct vc4_hdmi *vc4_hdmi = encoder_to_vc4_hdmi(encoder);
	u32 packet_id = type - 0x80;
	unsigned long flags;

	spin_lock_irqsave(&vc4_hdmi->hw_lock, flags);
	HDMI_WRITE(HDMI_RAM_PACKET_CONFIG,
		   HDMI_READ(HDMI_RAM_PACKET_CONFIG) & ~BIT(packet_id));
	spin_unlock_irqrestore(&vc4_hdmi->hw_lock, flags);

	if (!poll)
		return 0;

	return wait_for(!(HDMI_READ(HDMI_RAM_PACKET_STATUS) &
			  BIT(packet_id)), 100);
}

static void vc4_hdmi_write_infoframe(struct drm_encoder *encoder,
				     union hdmi_infoframe *frame)
{
	struct vc4_hdmi *vc4_hdmi = encoder_to_vc4_hdmi(encoder);
	u32 packet_id = frame->any.type - 0x80;
	const struct vc4_hdmi_register *ram_packet_start =
		&vc4_hdmi->variant->registers[HDMI_RAM_PACKET_START];
	u32 packet_reg = ram_packet_start->offset + VC4_HDMI_PACKET_STRIDE * packet_id;
	u32 packet_reg_next = ram_packet_start->offset +
		VC4_HDMI_PACKET_STRIDE * (packet_id + 1);
	void __iomem *base = __vc4_hdmi_get_field_base(vc4_hdmi,
						       ram_packet_start->reg);
	uint8_t buffer[VC4_HDMI_PACKET_STRIDE] = {};
	unsigned long flags;
	ssize_t len, i;
	int ret;

	WARN_ONCE(!(HDMI_READ(HDMI_RAM_PACKET_CONFIG) &
		    VC4_HDMI_RAM_PACKET_ENABLE),
		  "Packet RAM has to be on to store the packet.");

	len = hdmi_infoframe_pack(frame, buffer, sizeof(buffer));
	if (len < 0)
		return;

	ret = vc4_hdmi_stop_packet(encoder, frame->any.type, true);
	if (ret) {
		DRM_ERROR("Failed to wait for infoframe to go idle: %d\n", ret);
		return;
	}

	spin_lock_irqsave(&vc4_hdmi->hw_lock, flags);

	for (i = 0; i < len; i += 7) {
		writel(buffer[i + 0] << 0 |
		       buffer[i + 1] << 8 |
		       buffer[i + 2] << 16,
		       base + packet_reg);
		packet_reg += 4;

		writel(buffer[i + 3] << 0 |
		       buffer[i + 4] << 8 |
		       buffer[i + 5] << 16 |
		       buffer[i + 6] << 24,
		       base + packet_reg);
		packet_reg += 4;
	}

	/*
	 * clear remainder of packet ram as it's included in the
	 * infoframe and triggers a checksum error on hdmi analyser
	 */
	for (; packet_reg < packet_reg_next; packet_reg += 4)
		writel(0, base + packet_reg);

	HDMI_WRITE(HDMI_RAM_PACKET_CONFIG,
		   HDMI_READ(HDMI_RAM_PACKET_CONFIG) | BIT(packet_id));

	spin_unlock_irqrestore(&vc4_hdmi->hw_lock, flags);

	ret = wait_for((HDMI_READ(HDMI_RAM_PACKET_STATUS) &
			BIT(packet_id)), 100);
	if (ret)
		DRM_ERROR("Failed to wait for infoframe to start: %d\n", ret);
}

static void vc4_hdmi_avi_infoframe_colorspace(struct hdmi_avi_infoframe *frame,
					      enum vc4_hdmi_output_format fmt)
{
	switch (fmt) {
	case VC4_HDMI_OUTPUT_RGB:
		frame->colorspace = HDMI_COLORSPACE_RGB;
		break;

	case VC4_HDMI_OUTPUT_YUV420:
		frame->colorspace = HDMI_COLORSPACE_YUV420;
		break;

	case VC4_HDMI_OUTPUT_YUV422:
		frame->colorspace = HDMI_COLORSPACE_YUV422;
		break;

	case VC4_HDMI_OUTPUT_YUV444:
		frame->colorspace = HDMI_COLORSPACE_YUV444;
		break;

	default:
		break;
	}
}

static void vc4_hdmi_set_avi_infoframe(struct drm_encoder *encoder)
{
	struct vc4_hdmi *vc4_hdmi = encoder_to_vc4_hdmi(encoder);
	struct drm_connector *connector = &vc4_hdmi->connector;
	struct drm_connector_state *cstate = connector->state;
	struct vc4_hdmi_connector_state *vc4_state =
		conn_state_to_vc4_hdmi_conn_state(cstate);
	const struct drm_display_mode *mode = &vc4_hdmi->saved_adjusted_mode;
	union hdmi_infoframe frame;
	int ret;

	lockdep_assert_held(&vc4_hdmi->mutex);

	ret = drm_hdmi_avi_infoframe_from_display_mode(&frame.avi,
						       connector, mode);
	if (ret < 0) {
		DRM_ERROR("couldn't fill AVI infoframe\n");
		return;
	}

	drm_hdmi_avi_infoframe_quant_range(&frame.avi,
					   connector, mode,
					   vc4_hdmi_is_full_range_rgb(vc4_hdmi, mode) ?
					   HDMI_QUANTIZATION_RANGE_FULL :
					   HDMI_QUANTIZATION_RANGE_LIMITED);
	drm_hdmi_avi_infoframe_colorimetry(&frame.avi, cstate);
	vc4_hdmi_avi_infoframe_colorspace(&frame.avi, vc4_state->output_format);
	drm_hdmi_avi_infoframe_bars(&frame.avi, cstate);

	vc4_hdmi_write_infoframe(encoder, &frame);
}

static void vc4_hdmi_set_spd_infoframe(struct drm_encoder *encoder)
{
	union hdmi_infoframe frame;
	int ret;

	ret = hdmi_spd_infoframe_init(&frame.spd, "Broadcom", "Videocore");
	if (ret < 0) {
		DRM_ERROR("couldn't fill SPD infoframe\n");
		return;
	}

	frame.spd.sdi = HDMI_SPD_SDI_PC;

	vc4_hdmi_write_infoframe(encoder, &frame);
}

static void vc4_hdmi_set_audio_infoframe(struct drm_encoder *encoder)
{
	struct vc4_hdmi *vc4_hdmi = encoder_to_vc4_hdmi(encoder);
	struct hdmi_audio_infoframe *audio = &vc4_hdmi->audio.infoframe;
	union hdmi_infoframe frame;

	memcpy(&frame.audio, audio, sizeof(*audio));

	if (vc4_hdmi->packet_ram_enabled)
		vc4_hdmi_write_infoframe(encoder, &frame);
}

static void vc4_hdmi_set_hdr_infoframe(struct drm_encoder *encoder)
{
	struct vc4_hdmi *vc4_hdmi = encoder_to_vc4_hdmi(encoder);
	struct drm_connector *connector = &vc4_hdmi->connector;
	struct drm_connector_state *conn_state = connector->state;
	union hdmi_infoframe frame;

	lockdep_assert_held(&vc4_hdmi->mutex);

	if (!vc4_hdmi->variant->supports_hdr)
		return;

	if (!conn_state->hdr_output_metadata)
		return;

	if (drm_hdmi_infoframe_set_hdr_metadata(&frame.drm, conn_state))
		return;

	vc4_hdmi_write_infoframe(encoder, &frame);
}

static void vc4_hdmi_set_infoframes(struct drm_encoder *encoder)
{
	struct vc4_hdmi *vc4_hdmi = encoder_to_vc4_hdmi(encoder);

	lockdep_assert_held(&vc4_hdmi->mutex);

	vc4_hdmi_set_avi_infoframe(encoder);
	vc4_hdmi_set_spd_infoframe(encoder);
	/*
	 * If audio was streaming, then we need to reenabled the audio
	 * infoframe here during encoder_enable.
	 */
	if (vc4_hdmi->audio.streaming)
		vc4_hdmi_set_audio_infoframe(encoder);

	vc4_hdmi_set_hdr_infoframe(encoder);
}

static bool vc4_hdmi_supports_scrambling(struct drm_encoder *encoder,
					 struct drm_display_mode *mode)
{
	struct vc4_hdmi *vc4_hdmi = encoder_to_vc4_hdmi(encoder);
	struct drm_display_info *display = &vc4_hdmi->connector.display_info;

	lockdep_assert_held(&vc4_hdmi->mutex);

	if (!display->is_hdmi)
		return false;

	if (!display->hdmi.scdc.supported ||
	    !display->hdmi.scdc.scrambling.supported)
		return false;

	return true;
}

#define SCRAMBLING_POLLING_DELAY_MS	1000

static void vc4_hdmi_enable_scrambling(struct drm_encoder *encoder)
{
	struct vc4_hdmi *vc4_hdmi = encoder_to_vc4_hdmi(encoder);
	struct drm_display_mode *mode = &vc4_hdmi->saved_adjusted_mode;
	unsigned long flags;

	lockdep_assert_held(&vc4_hdmi->mutex);

	if (!vc4_hdmi_supports_scrambling(encoder, mode))
		return;

	if (!vc4_hdmi_mode_needs_scrambling(mode,
					    vc4_hdmi->output_bpc,
					    vc4_hdmi->output_format))
		return;

	drm_scdc_set_high_tmds_clock_ratio(vc4_hdmi->ddc, true);
	drm_scdc_set_scrambling(vc4_hdmi->ddc, true);

	spin_lock_irqsave(&vc4_hdmi->hw_lock, flags);
	HDMI_WRITE(HDMI_SCRAMBLER_CTL, HDMI_READ(HDMI_SCRAMBLER_CTL) |
		   VC5_HDMI_SCRAMBLER_CTL_ENABLE);
	spin_unlock_irqrestore(&vc4_hdmi->hw_lock, flags);

	vc4_hdmi->scdc_enabled = true;

	queue_delayed_work(system_wq, &vc4_hdmi->scrambling_work,
			   msecs_to_jiffies(SCRAMBLING_POLLING_DELAY_MS));
}

static void vc4_hdmi_disable_scrambling(struct drm_encoder *encoder)
{
	struct vc4_hdmi *vc4_hdmi = encoder_to_vc4_hdmi(encoder);
	unsigned long flags;

	lockdep_assert_held(&vc4_hdmi->mutex);

	if (!vc4_hdmi->scdc_enabled)
		return;

	vc4_hdmi->scdc_enabled = false;

	if (delayed_work_pending(&vc4_hdmi->scrambling_work))
		cancel_delayed_work_sync(&vc4_hdmi->scrambling_work);

	spin_lock_irqsave(&vc4_hdmi->hw_lock, flags);
	HDMI_WRITE(HDMI_SCRAMBLER_CTL, HDMI_READ(HDMI_SCRAMBLER_CTL) &
		   ~VC5_HDMI_SCRAMBLER_CTL_ENABLE);
	spin_unlock_irqrestore(&vc4_hdmi->hw_lock, flags);

	drm_scdc_set_scrambling(vc4_hdmi->ddc, false);
	drm_scdc_set_high_tmds_clock_ratio(vc4_hdmi->ddc, false);
}

static void vc4_hdmi_scrambling_wq(struct work_struct *work)
{
	struct vc4_hdmi *vc4_hdmi = container_of(to_delayed_work(work),
						 struct vc4_hdmi,
						 scrambling_work);

	if (drm_scdc_get_scrambling_status(vc4_hdmi->ddc))
		return;

	drm_scdc_set_high_tmds_clock_ratio(vc4_hdmi->ddc, true);
	drm_scdc_set_scrambling(vc4_hdmi->ddc, true);

	queue_delayed_work(system_wq, &vc4_hdmi->scrambling_work,
			   msecs_to_jiffies(SCRAMBLING_POLLING_DELAY_MS));
}

static void vc4_hdmi_encoder_post_crtc_disable(struct drm_encoder *encoder,
					       struct drm_atomic_state *state)
{
	struct vc4_hdmi *vc4_hdmi = encoder_to_vc4_hdmi(encoder);
	unsigned long flags;

	mutex_lock(&vc4_hdmi->mutex);

	vc4_hdmi->packet_ram_enabled = false;

	spin_lock_irqsave(&vc4_hdmi->hw_lock, flags);

	HDMI_WRITE(HDMI_RAM_PACKET_CONFIG, 0);

	HDMI_WRITE(HDMI_VID_CTL, HDMI_READ(HDMI_VID_CTL) | VC4_HD_VID_CTL_CLRRGB);

	spin_unlock_irqrestore(&vc4_hdmi->hw_lock, flags);

	mdelay(1);

	spin_lock_irqsave(&vc4_hdmi->hw_lock, flags);
	HDMI_WRITE(HDMI_VID_CTL,
		   HDMI_READ(HDMI_VID_CTL) & ~VC4_HD_VID_CTL_ENABLE);
	spin_unlock_irqrestore(&vc4_hdmi->hw_lock, flags);

	vc4_hdmi_disable_scrambling(encoder);

	mutex_unlock(&vc4_hdmi->mutex);
}

static void vc4_hdmi_encoder_post_crtc_powerdown(struct drm_encoder *encoder,
						 struct drm_atomic_state *state)
{
	struct vc4_hdmi *vc4_hdmi = encoder_to_vc4_hdmi(encoder);
	unsigned long flags;
	int ret;

	mutex_lock(&vc4_hdmi->mutex);

	spin_lock_irqsave(&vc4_hdmi->hw_lock, flags);
	HDMI_WRITE(HDMI_VID_CTL,
		   HDMI_READ(HDMI_VID_CTL) | VC4_HD_VID_CTL_BLANKPIX);
	spin_unlock_irqrestore(&vc4_hdmi->hw_lock, flags);

	if (vc4_hdmi->variant->phy_disable)
		vc4_hdmi->variant->phy_disable(vc4_hdmi);

	clk_disable_unprepare(vc4_hdmi->pixel_bvb_clock);
	clk_disable_unprepare(vc4_hdmi->pixel_clock);

	ret = pm_runtime_put(&vc4_hdmi->pdev->dev);
	if (ret < 0)
		DRM_ERROR("Failed to release power domain: %d\n", ret);

	mutex_unlock(&vc4_hdmi->mutex);
}

static void vc4_hdmi_csc_setup(struct vc4_hdmi *vc4_hdmi,
			       struct drm_connector_state *state,
			       const struct drm_display_mode *mode)
{
	unsigned long flags;
	u32 csc_ctl;

	spin_lock_irqsave(&vc4_hdmi->hw_lock, flags);

	csc_ctl = VC4_SET_FIELD(VC4_HD_CSC_CTL_ORDER_BGR,
				VC4_HD_CSC_CTL_ORDER);

	if (!vc4_hdmi_is_full_range_rgb(vc4_hdmi, mode)) {
		/* CEA VICs other than #1 requre limited range RGB
		 * output unless overridden by an AVI infoframe.
		 * Apply a colorspace conversion to squash 0-255 down
		 * to 16-235.  The matrix here is:
		 *
		 * [ 0      0      0.8594 16]
		 * [ 0      0.8594 0      16]
		 * [ 0.8594 0      0      16]
		 * [ 0      0      0       1]
		 */
		csc_ctl |= VC4_HD_CSC_CTL_ENABLE;
		csc_ctl |= VC4_HD_CSC_CTL_RGB2YCC;
		csc_ctl |= VC4_SET_FIELD(VC4_HD_CSC_CTL_MODE_CUSTOM,
					 VC4_HD_CSC_CTL_MODE);

		HDMI_WRITE(HDMI_CSC_12_11, (0x000 << 16) | 0x000);
		HDMI_WRITE(HDMI_CSC_14_13, (0x100 << 16) | 0x6e0);
		HDMI_WRITE(HDMI_CSC_22_21, (0x6e0 << 16) | 0x000);
		HDMI_WRITE(HDMI_CSC_24_23, (0x100 << 16) | 0x000);
		HDMI_WRITE(HDMI_CSC_32_31, (0x000 << 16) | 0x6e0);
		HDMI_WRITE(HDMI_CSC_34_33, (0x100 << 16) | 0x000);
	}

	/* The RGB order applies even when CSC is disabled. */
	HDMI_WRITE(HDMI_CSC_CTL, csc_ctl);

	spin_unlock_irqrestore(&vc4_hdmi->hw_lock, flags);
}

/*
 * If we need to output Full Range RGB, then use the unity matrix
 *
 * [ 1      0      0      0]
 * [ 0      1      0      0]
 * [ 0      0      1      0]
 *
 * Matrix is signed 2p13 fixed point, with signed 9p6 offsets
 */
static const u16 vc5_hdmi_csc_full_rgb_unity[3][4] = {
	{ 0x2000, 0x0000, 0x0000, 0x0000 },
	{ 0x0000, 0x2000, 0x0000, 0x0000 },
	{ 0x0000, 0x0000, 0x2000, 0x0000 },
};

/*
 * CEA VICs other than #1 require limited range RGB output unless
 * overridden by an AVI infoframe. Apply a colorspace conversion to
 * squash 0-255 down to 16-235. The matrix here is:
 *
 * [ 0.8594 0      0      16]
 * [ 0      0.8594 0      16]
 * [ 0      0      0.8594 16]
 *
 * Matrix is signed 2p13 fixed point, with signed 9p6 offsets
 */
static const u16 vc5_hdmi_csc_full_rgb_to_limited_rgb[3][4] = {
	{ 0x1b80, 0x0000, 0x0000, 0x0400 },
	{ 0x0000, 0x1b80, 0x0000, 0x0400 },
	{ 0x0000, 0x0000, 0x1b80, 0x0400 },
};

/*
 * Conversion between Full Range RGB and Full Range YUV422 using the
 * BT.709 Colorspace
 *
 *
 * [  0.181906  0.611804  0.061758  16  ]
 * [ -0.100268 -0.337232  0.437500  128 ]
 * [  0.437500 -0.397386 -0.040114  128 ]
 *
 * Matrix is signed 2p13 fixed point, with signed 9p6 offsets
 */
static const u16 vc5_hdmi_csc_full_rgb_to_limited_yuv422_bt709[3][4] = {
	{ 0x05d2, 0x1394, 0x01fa, 0x0400 },
	{ 0xfccc, 0xf536, 0x0e00, 0x2000 },
	{ 0x0e00, 0xf34a, 0xfeb8, 0x2000 },
};

/*
 * Conversion between Full Range RGB and Full Range YUV444 using the
 * BT.709 Colorspace
 *
 * [ -0.100268 -0.337232  0.437500  128 ]
 * [  0.437500 -0.397386 -0.040114  128 ]
 * [  0.181906  0.611804  0.061758  16  ]
 *
 * Matrix is signed 2p13 fixed point, with signed 9p6 offsets
 */
static const u16 vc5_hdmi_csc_full_rgb_to_limited_yuv444_bt709[3][4] = {
	{ 0xfccc, 0xf536, 0x0e00, 0x2000 },
	{ 0x0e00, 0xf34a, 0xfeb8, 0x2000 },
	{ 0x05d2, 0x1394, 0x01fa, 0x0400 },
};

static void vc5_hdmi_set_csc_coeffs(struct vc4_hdmi *vc4_hdmi,
				    const u16 coeffs[3][4])
{
	lockdep_assert_held(&vc4_hdmi->hw_lock);

	HDMI_WRITE(HDMI_CSC_12_11, (coeffs[0][1] << 16) | coeffs[0][0]);
	HDMI_WRITE(HDMI_CSC_14_13, (coeffs[0][3] << 16) | coeffs[0][2]);
	HDMI_WRITE(HDMI_CSC_22_21, (coeffs[1][1] << 16) | coeffs[1][0]);
	HDMI_WRITE(HDMI_CSC_24_23, (coeffs[1][3] << 16) | coeffs[1][2]);
	HDMI_WRITE(HDMI_CSC_32_31, (coeffs[2][1] << 16) | coeffs[2][0]);
	HDMI_WRITE(HDMI_CSC_34_33, (coeffs[2][3] << 16) | coeffs[2][2]);
}

static void vc5_hdmi_csc_setup(struct vc4_hdmi *vc4_hdmi,
			       struct drm_connector_state *state,
			       const struct drm_display_mode *mode)
{
	struct vc4_hdmi_connector_state *vc4_state =
		conn_state_to_vc4_hdmi_conn_state(state);
	unsigned long flags;
	u32 if_cfg = 0;
	u32 if_xbar = 0x543210;
	u32 csc_chan_ctl = 0;
	u32 csc_ctl = VC5_MT_CP_CSC_CTL_ENABLE | VC4_SET_FIELD(VC4_HD_CSC_CTL_MODE_CUSTOM,
							       VC5_MT_CP_CSC_CTL_MODE);

	spin_lock_irqsave(&vc4_hdmi->hw_lock, flags);

	switch (vc4_state->output_format) {
	case VC4_HDMI_OUTPUT_YUV444:
		vc5_hdmi_set_csc_coeffs(vc4_hdmi, vc5_hdmi_csc_full_rgb_to_limited_yuv444_bt709);
		break;

	case VC4_HDMI_OUTPUT_YUV422:
		csc_ctl |= VC4_SET_FIELD(VC5_MT_CP_CSC_CTL_FILTER_MODE_444_TO_422_STANDARD,
					 VC5_MT_CP_CSC_CTL_FILTER_MODE_444_TO_422) |
			VC5_MT_CP_CSC_CTL_USE_444_TO_422 |
			VC5_MT_CP_CSC_CTL_USE_RNG_SUPPRESSION;

		csc_chan_ctl |= VC4_SET_FIELD(VC5_MT_CP_CHANNEL_CTL_OUTPUT_REMAP_LEGACY_STYLE,
					      VC5_MT_CP_CHANNEL_CTL_OUTPUT_REMAP);

		if_cfg |= VC4_SET_FIELD(VC5_DVP_HT_VEC_INTERFACE_CFG_SEL_422_FORMAT_422_LEGACY,
					VC5_DVP_HT_VEC_INTERFACE_CFG_SEL_422);

		vc5_hdmi_set_csc_coeffs(vc4_hdmi, vc5_hdmi_csc_full_rgb_to_limited_yuv422_bt709);
		break;

	case VC4_HDMI_OUTPUT_RGB:
		if_xbar = 0x354021;

		if (!vc4_hdmi_is_full_range_rgb(vc4_hdmi, mode))
			vc5_hdmi_set_csc_coeffs(vc4_hdmi, vc5_hdmi_csc_full_rgb_to_limited_rgb);
		else
			vc5_hdmi_set_csc_coeffs(vc4_hdmi, vc5_hdmi_csc_full_rgb_unity);
		break;

	default:
		break;
	}

	HDMI_WRITE(HDMI_VEC_INTERFACE_CFG, if_cfg);
	HDMI_WRITE(HDMI_VEC_INTERFACE_XBAR, if_xbar);
	HDMI_WRITE(HDMI_CSC_CHANNEL_CTL, csc_chan_ctl);
	HDMI_WRITE(HDMI_CSC_CTL, csc_ctl);

	spin_unlock_irqrestore(&vc4_hdmi->hw_lock, flags);
}

static void vc4_hdmi_set_timings(struct vc4_hdmi *vc4_hdmi,
				 struct drm_connector_state *state,
				 struct drm_display_mode *mode)
{
	bool hsync_pos = mode->flags & DRM_MODE_FLAG_PHSYNC;
	bool vsync_pos = mode->flags & DRM_MODE_FLAG_PVSYNC;
	bool interlaced = mode->flags & DRM_MODE_FLAG_INTERLACE;
	u32 pixel_rep = (mode->flags & DRM_MODE_FLAG_DBLCLK) ? 2 : 1;
	u32 verta = (VC4_SET_FIELD(mode->crtc_vsync_end - mode->crtc_vsync_start,
				   VC4_HDMI_VERTA_VSP) |
		     VC4_SET_FIELD(mode->crtc_vsync_start - mode->crtc_vdisplay,
				   VC4_HDMI_VERTA_VFP) |
		     VC4_SET_FIELD(mode->crtc_vdisplay, VC4_HDMI_VERTA_VAL));
	u32 vertb = (VC4_SET_FIELD(0, VC4_HDMI_VERTB_VSPO) |
		     VC4_SET_FIELD(mode->crtc_vtotal - mode->crtc_vsync_end +
				   interlaced,
				   VC4_HDMI_VERTB_VBP));
	u32 vertb_even = (VC4_SET_FIELD(0, VC4_HDMI_VERTB_VSPO) |
			  VC4_SET_FIELD(mode->crtc_vtotal -
					mode->crtc_vsync_end,
					VC4_HDMI_VERTB_VBP));
	unsigned long flags;
	u32 reg;

	spin_lock_irqsave(&vc4_hdmi->hw_lock, flags);

	HDMI_WRITE(HDMI_HORZA,
		   (vsync_pos ? VC4_HDMI_HORZA_VPOS : 0) |
		   (hsync_pos ? VC4_HDMI_HORZA_HPOS : 0) |
		   VC4_SET_FIELD(mode->hdisplay * pixel_rep,
				 VC4_HDMI_HORZA_HAP));

	HDMI_WRITE(HDMI_HORZB,
		   VC4_SET_FIELD((mode->htotal -
				  mode->hsync_end) * pixel_rep,
				 VC4_HDMI_HORZB_HBP) |
		   VC4_SET_FIELD((mode->hsync_end -
				  mode->hsync_start) * pixel_rep,
				 VC4_HDMI_HORZB_HSP) |
		   VC4_SET_FIELD((mode->hsync_start -
				  mode->hdisplay) * pixel_rep,
				 VC4_HDMI_HORZB_HFP));

	HDMI_WRITE(HDMI_VERTA0, verta);
	HDMI_WRITE(HDMI_VERTA1, verta);

	HDMI_WRITE(HDMI_VERTB0, vertb_even);
	HDMI_WRITE(HDMI_VERTB1, vertb);

	reg = HDMI_READ(HDMI_MISC_CONTROL);
	reg &= ~VC4_HDMI_MISC_CONTROL_PIXEL_REP_MASK;
	reg |= VC4_SET_FIELD(pixel_rep - 1, VC4_HDMI_MISC_CONTROL_PIXEL_REP);
	HDMI_WRITE(HDMI_MISC_CONTROL, reg);

	spin_unlock_irqrestore(&vc4_hdmi->hw_lock, flags);
}

static void vc5_hdmi_set_timings(struct vc4_hdmi *vc4_hdmi,
				 struct drm_connector_state *state,
				 struct drm_display_mode *mode)
{
	const struct vc4_hdmi_connector_state *vc4_state =
		conn_state_to_vc4_hdmi_conn_state(state);
	bool hsync_pos = mode->flags & DRM_MODE_FLAG_PHSYNC;
	bool vsync_pos = mode->flags & DRM_MODE_FLAG_PVSYNC;
	bool interlaced = mode->flags & DRM_MODE_FLAG_INTERLACE;
	u32 pixel_rep = (mode->flags & DRM_MODE_FLAG_DBLCLK) ? 2 : 1;
	u32 verta = (VC4_SET_FIELD(mode->crtc_vsync_end - mode->crtc_vsync_start,
				   VC5_HDMI_VERTA_VSP) |
		     VC4_SET_FIELD(mode->crtc_vsync_start - mode->crtc_vdisplay,
				   VC5_HDMI_VERTA_VFP) |
		     VC4_SET_FIELD(mode->crtc_vdisplay, VC5_HDMI_VERTA_VAL));
	u32 vertb = (VC4_SET_FIELD(mode->htotal >> (2 - pixel_rep),
				   VC5_HDMI_VERTB_VSPO) |
		     VC4_SET_FIELD(mode->crtc_vtotal - mode->crtc_vsync_end,
				   VC4_HDMI_VERTB_VBP));
	u32 vertb_even = (VC4_SET_FIELD(0, VC5_HDMI_VERTB_VSPO) |
			  VC4_SET_FIELD(mode->crtc_vtotal -
					mode->crtc_vsync_end - interlaced,
					VC4_HDMI_VERTB_VBP));
	unsigned long flags;
	unsigned char gcp;
	bool gcp_en;
	u32 reg;

	spin_lock_irqsave(&vc4_hdmi->hw_lock, flags);

	HDMI_WRITE(HDMI_HORZA,
		   (vsync_pos ? VC5_HDMI_HORZA_VPOS : 0) |
		   (hsync_pos ? VC5_HDMI_HORZA_HPOS : 0) |
		   VC4_SET_FIELD(mode->hdisplay * pixel_rep,
				 VC5_HDMI_HORZA_HAP) |
		   VC4_SET_FIELD((mode->hsync_start -
				  mode->hdisplay) * pixel_rep,
				 VC5_HDMI_HORZA_HFP));

	HDMI_WRITE(HDMI_HORZB,
		   VC4_SET_FIELD((mode->htotal -
				  mode->hsync_end) * pixel_rep,
				 VC5_HDMI_HORZB_HBP) |
		   VC4_SET_FIELD((mode->hsync_end -
				  mode->hsync_start) * pixel_rep,
				 VC5_HDMI_HORZB_HSP));

	HDMI_WRITE(HDMI_VERTA0, verta);
	HDMI_WRITE(HDMI_VERTA1, verta);

	HDMI_WRITE(HDMI_VERTB0, vertb_even);
	HDMI_WRITE(HDMI_VERTB1, vertb);

	switch (vc4_state->output_bpc) {
	case 12:
		gcp = 6;
		gcp_en = true;
		break;
	case 10:
		gcp = 5;
		gcp_en = true;
		break;
	case 8:
	default:
		gcp = 4;
		gcp_en = false;
		break;
	}

	/*
	 * YCC422 is always 36-bit and not considered deep colour so
	 * doesn't signal in GCP.
	 */
	if (vc4_state->output_format == VC4_HDMI_OUTPUT_YUV422) {
		gcp = 4;
		gcp_en = false;
	}

	reg = HDMI_READ(HDMI_DEEP_COLOR_CONFIG_1);
	reg &= ~(VC5_HDMI_DEEP_COLOR_CONFIG_1_INIT_PACK_PHASE_MASK |
		 VC5_HDMI_DEEP_COLOR_CONFIG_1_COLOR_DEPTH_MASK);
	reg |= VC4_SET_FIELD(2, VC5_HDMI_DEEP_COLOR_CONFIG_1_INIT_PACK_PHASE) |
	       VC4_SET_FIELD(gcp, VC5_HDMI_DEEP_COLOR_CONFIG_1_COLOR_DEPTH);
	HDMI_WRITE(HDMI_DEEP_COLOR_CONFIG_1, reg);

	reg = HDMI_READ(HDMI_GCP_WORD_1);
	reg &= ~VC5_HDMI_GCP_WORD_1_GCP_SUBPACKET_BYTE_1_MASK;
	reg |= VC4_SET_FIELD(gcp, VC5_HDMI_GCP_WORD_1_GCP_SUBPACKET_BYTE_1);
	HDMI_WRITE(HDMI_GCP_WORD_1, reg);

	reg = HDMI_READ(HDMI_GCP_CONFIG);
	reg &= ~VC5_HDMI_GCP_CONFIG_GCP_ENABLE;
	reg |= gcp_en ? VC5_HDMI_GCP_CONFIG_GCP_ENABLE : 0;
	HDMI_WRITE(HDMI_GCP_CONFIG, reg);

	reg = HDMI_READ(HDMI_MISC_CONTROL);
	reg &= ~VC5_HDMI_MISC_CONTROL_PIXEL_REP_MASK;
	reg |= VC4_SET_FIELD(pixel_rep - 1, VC5_HDMI_MISC_CONTROL_PIXEL_REP);
	HDMI_WRITE(HDMI_MISC_CONTROL, reg);

	HDMI_WRITE(HDMI_CLOCK_STOP, 0);

	spin_unlock_irqrestore(&vc4_hdmi->hw_lock, flags);
}

static void vc4_hdmi_recenter_fifo(struct vc4_hdmi *vc4_hdmi)
{
	unsigned long flags;
	u32 drift;
	int ret;

	spin_lock_irqsave(&vc4_hdmi->hw_lock, flags);

	drift = HDMI_READ(HDMI_FIFO_CTL);
	drift &= VC4_HDMI_FIFO_VALID_WRITE_MASK;

	HDMI_WRITE(HDMI_FIFO_CTL,
		   drift & ~VC4_HDMI_FIFO_CTL_RECENTER);
	HDMI_WRITE(HDMI_FIFO_CTL,
		   drift | VC4_HDMI_FIFO_CTL_RECENTER);

	spin_unlock_irqrestore(&vc4_hdmi->hw_lock, flags);

	usleep_range(1000, 1100);

	spin_lock_irqsave(&vc4_hdmi->hw_lock, flags);

	HDMI_WRITE(HDMI_FIFO_CTL,
		   drift & ~VC4_HDMI_FIFO_CTL_RECENTER);
	HDMI_WRITE(HDMI_FIFO_CTL,
		   drift | VC4_HDMI_FIFO_CTL_RECENTER);

	spin_unlock_irqrestore(&vc4_hdmi->hw_lock, flags);

	ret = wait_for(HDMI_READ(HDMI_FIFO_CTL) &
		       VC4_HDMI_FIFO_CTL_RECENTER_DONE, 1);
	WARN_ONCE(ret, "Timeout waiting for "
		  "VC4_HDMI_FIFO_CTL_RECENTER_DONE");
}

static void vc4_hdmi_encoder_pre_crtc_configure(struct drm_encoder *encoder,
						struct drm_atomic_state *state)
{
	struct vc4_hdmi *vc4_hdmi = encoder_to_vc4_hdmi(encoder);
	struct drm_connector *connector = &vc4_hdmi->connector;
	struct drm_connector_state *conn_state =
		drm_atomic_get_new_connector_state(state, connector);
	struct vc4_hdmi_connector_state *vc4_conn_state =
		conn_state_to_vc4_hdmi_conn_state(conn_state);
	struct drm_display_mode *mode = &vc4_hdmi->saved_adjusted_mode;
	unsigned long tmds_char_rate = vc4_conn_state->tmds_char_rate;
	unsigned long bvb_rate, hsm_rate;
	unsigned long flags;
	int ret;

	mutex_lock(&vc4_hdmi->mutex);

	/*
	 * As stated in RPi's vc4 firmware "HDMI state machine (HSM) clock must
	 * be faster than pixel clock, infinitesimally faster, tested in
	 * simulation. Otherwise, exact value is unimportant for HDMI
	 * operation." This conflicts with bcm2835's vc4 documentation, which
	 * states HSM's clock has to be at least 108% of the pixel clock.
	 *
	 * Real life tests reveal that vc4's firmware statement holds up, and
	 * users are able to use pixel clocks closer to HSM's, namely for
	 * 1920x1200@60Hz. So it was decided to have leave a 1% margin between
	 * both clocks. Which, for RPi0-3 implies a maximum pixel clock of
	 * 162MHz.
	 *
	 * Additionally, the AXI clock needs to be at least 25% of
	 * pixel clock, but HSM ends up being the limiting factor.
	 */
	hsm_rate = max_t(unsigned long, 120000000, (tmds_char_rate / 100) * 101);
	ret = clk_set_min_rate(vc4_hdmi->hsm_clock, hsm_rate);
	if (ret) {
		DRM_ERROR("Failed to set HSM clock rate: %d\n", ret);
		goto out;
	}

	ret = pm_runtime_resume_and_get(&vc4_hdmi->pdev->dev);
	if (ret < 0) {
		DRM_ERROR("Failed to retain power domain: %d\n", ret);
		goto out;
	}

	ret = clk_set_rate(vc4_hdmi->pixel_clock, tmds_char_rate);
	if (ret) {
		DRM_ERROR("Failed to set pixel clock rate: %d\n", ret);
		goto err_put_runtime_pm;
	}

	ret = clk_prepare_enable(vc4_hdmi->pixel_clock);
	if (ret) {
		DRM_ERROR("Failed to turn on pixel clock: %d\n", ret);
		goto err_put_runtime_pm;
	}


	vc4_hdmi_cec_update_clk_div(vc4_hdmi);

	if (tmds_char_rate > 297000000)
		bvb_rate = 300000000;
	else if (tmds_char_rate > 148500000)
		bvb_rate = 150000000;
	else
		bvb_rate = 75000000;

	ret = clk_set_min_rate(vc4_hdmi->pixel_bvb_clock, bvb_rate);
	if (ret) {
		DRM_ERROR("Failed to set pixel bvb clock rate: %d\n", ret);
		goto err_disable_pixel_clock;
	}

	ret = clk_prepare_enable(vc4_hdmi->pixel_bvb_clock);
	if (ret) {
		DRM_ERROR("Failed to turn on pixel bvb clock: %d\n", ret);
		goto err_disable_pixel_clock;
	}

	if (vc4_hdmi->variant->phy_init)
		vc4_hdmi->variant->phy_init(vc4_hdmi, vc4_conn_state);

	spin_lock_irqsave(&vc4_hdmi->hw_lock, flags);

	HDMI_WRITE(HDMI_SCHEDULER_CONTROL,
		   HDMI_READ(HDMI_SCHEDULER_CONTROL) |
		   VC4_HDMI_SCHEDULER_CONTROL_MANUAL_FORMAT |
		   VC4_HDMI_SCHEDULER_CONTROL_IGNORE_VSYNC_PREDICTS);

	spin_unlock_irqrestore(&vc4_hdmi->hw_lock, flags);

	if (vc4_hdmi->variant->set_timings)
		vc4_hdmi->variant->set_timings(vc4_hdmi, conn_state, mode);

	mutex_unlock(&vc4_hdmi->mutex);

	return;

err_disable_pixel_clock:
	clk_disable_unprepare(vc4_hdmi->pixel_clock);
err_put_runtime_pm:
	pm_runtime_put(&vc4_hdmi->pdev->dev);
out:
	mutex_unlock(&vc4_hdmi->mutex);
	return;
}

static void vc4_hdmi_encoder_pre_crtc_enable(struct drm_encoder *encoder,
					     struct drm_atomic_state *state)
{
	struct vc4_hdmi *vc4_hdmi = encoder_to_vc4_hdmi(encoder);
	struct drm_connector *connector = &vc4_hdmi->connector;
	struct drm_display_mode *mode = &vc4_hdmi->saved_adjusted_mode;
	struct drm_connector_state *conn_state =
		drm_atomic_get_new_connector_state(state, connector);
	unsigned long flags;

	mutex_lock(&vc4_hdmi->mutex);

	if (vc4_hdmi->variant->csc_setup)
		vc4_hdmi->variant->csc_setup(vc4_hdmi, conn_state, mode);

	spin_lock_irqsave(&vc4_hdmi->hw_lock, flags);
	HDMI_WRITE(HDMI_FIFO_CTL, VC4_HDMI_FIFO_CTL_MASTER_SLAVE_N);
	spin_unlock_irqrestore(&vc4_hdmi->hw_lock, flags);

	mutex_unlock(&vc4_hdmi->mutex);
}

static void vc4_hdmi_encoder_post_crtc_enable(struct drm_encoder *encoder,
					      struct drm_atomic_state *state)
{
	struct vc4_hdmi *vc4_hdmi = encoder_to_vc4_hdmi(encoder);
	struct drm_display_mode *mode = &vc4_hdmi->saved_adjusted_mode;
	struct drm_display_info *display = &vc4_hdmi->connector.display_info;
	bool hsync_pos = mode->flags & DRM_MODE_FLAG_PHSYNC;
	bool vsync_pos = mode->flags & DRM_MODE_FLAG_PVSYNC;
	unsigned long flags;
	int ret;

	mutex_lock(&vc4_hdmi->mutex);

	spin_lock_irqsave(&vc4_hdmi->hw_lock, flags);

	HDMI_WRITE(HDMI_VID_CTL,
		   VC4_HD_VID_CTL_ENABLE |
		   VC4_HD_VID_CTL_CLRRGB |
		   VC4_HD_VID_CTL_UNDERFLOW_ENABLE |
		   VC4_HD_VID_CTL_FRAME_COUNTER_RESET |
		   (vsync_pos ? 0 : VC4_HD_VID_CTL_VSYNC_LOW) |
		   (hsync_pos ? 0 : VC4_HD_VID_CTL_HSYNC_LOW));

	HDMI_WRITE(HDMI_VID_CTL,
		   HDMI_READ(HDMI_VID_CTL) & ~VC4_HD_VID_CTL_BLANKPIX);

	if (display->is_hdmi) {
		HDMI_WRITE(HDMI_SCHEDULER_CONTROL,
			   HDMI_READ(HDMI_SCHEDULER_CONTROL) |
			   VC4_HDMI_SCHEDULER_CONTROL_MODE_HDMI);

		spin_unlock_irqrestore(&vc4_hdmi->hw_lock, flags);

		ret = wait_for(HDMI_READ(HDMI_SCHEDULER_CONTROL) &
			       VC4_HDMI_SCHEDULER_CONTROL_HDMI_ACTIVE, 1000);
		WARN_ONCE(ret, "Timeout waiting for "
			  "VC4_HDMI_SCHEDULER_CONTROL_HDMI_ACTIVE\n");
	} else {
		HDMI_WRITE(HDMI_RAM_PACKET_CONFIG,
			   HDMI_READ(HDMI_RAM_PACKET_CONFIG) &
			   ~(VC4_HDMI_RAM_PACKET_ENABLE));
		HDMI_WRITE(HDMI_SCHEDULER_CONTROL,
			   HDMI_READ(HDMI_SCHEDULER_CONTROL) &
			   ~VC4_HDMI_SCHEDULER_CONTROL_MODE_HDMI);

		spin_unlock_irqrestore(&vc4_hdmi->hw_lock, flags);

		ret = wait_for(!(HDMI_READ(HDMI_SCHEDULER_CONTROL) &
				 VC4_HDMI_SCHEDULER_CONTROL_HDMI_ACTIVE), 1000);
		WARN_ONCE(ret, "Timeout waiting for "
			  "!VC4_HDMI_SCHEDULER_CONTROL_HDMI_ACTIVE\n");
	}

	if (display->is_hdmi) {
		spin_lock_irqsave(&vc4_hdmi->hw_lock, flags);

		WARN_ON(!(HDMI_READ(HDMI_SCHEDULER_CONTROL) &
			  VC4_HDMI_SCHEDULER_CONTROL_HDMI_ACTIVE));

		HDMI_WRITE(HDMI_RAM_PACKET_CONFIG,
			   VC4_HDMI_RAM_PACKET_ENABLE);

		spin_unlock_irqrestore(&vc4_hdmi->hw_lock, flags);
		vc4_hdmi->packet_ram_enabled = true;

		vc4_hdmi_set_infoframes(encoder);
	}

	vc4_hdmi_recenter_fifo(vc4_hdmi);
	vc4_hdmi_enable_scrambling(encoder);

	mutex_unlock(&vc4_hdmi->mutex);
}

static void vc4_hdmi_encoder_atomic_mode_set(struct drm_encoder *encoder,
					     struct drm_crtc_state *crtc_state,
					     struct drm_connector_state *conn_state)
{
	struct vc4_hdmi *vc4_hdmi = encoder_to_vc4_hdmi(encoder);
	struct vc4_hdmi_connector_state *vc4_state =
		conn_state_to_vc4_hdmi_conn_state(conn_state);

	mutex_lock(&vc4_hdmi->mutex);
	drm_mode_copy(&vc4_hdmi->saved_adjusted_mode,
		      &crtc_state->adjusted_mode);
	vc4_hdmi->output_bpc = vc4_state->output_bpc;
	vc4_hdmi->output_format = vc4_state->output_format;
	mutex_unlock(&vc4_hdmi->mutex);
}

static bool
vc4_hdmi_sink_supports_format_bpc(const struct vc4_hdmi *vc4_hdmi,
				  const struct drm_display_info *info,
				  const struct drm_display_mode *mode,
				  unsigned int format, unsigned int bpc)
{
	struct drm_device *dev = vc4_hdmi->connector.dev;
	u8 vic = drm_match_cea_mode(mode);

	if (vic == 1 && bpc != 8) {
		drm_dbg(dev, "VIC1 requires a bpc of 8, got %u\n", bpc);
		return false;
	}

	if (!info->is_hdmi &&
	    (format != VC4_HDMI_OUTPUT_RGB || bpc != 8)) {
		drm_dbg(dev, "DVI Monitors require an RGB output at 8 bpc\n");
		return false;
	}

	switch (format) {
	case VC4_HDMI_OUTPUT_RGB:
		drm_dbg(dev, "RGB Format, checking the constraints.\n");

		if (!(info->color_formats & DRM_COLOR_FORMAT_RGB444))
			return false;

		if (bpc == 10 && !(info->edid_hdmi_rgb444_dc_modes & DRM_EDID_HDMI_DC_30)) {
			drm_dbg(dev, "10 BPC but sink doesn't support Deep Color 30.\n");
			return false;
		}

		if (bpc == 12 && !(info->edid_hdmi_rgb444_dc_modes & DRM_EDID_HDMI_DC_36)) {
			drm_dbg(dev, "12 BPC but sink doesn't support Deep Color 36.\n");
			return false;
		}

		drm_dbg(dev, "RGB format supported in that configuration.\n");

		return true;

	case VC4_HDMI_OUTPUT_YUV422:
		drm_dbg(dev, "YUV422 format, checking the constraints.\n");

		if (!(info->color_formats & DRM_COLOR_FORMAT_YCBCR422)) {
			drm_dbg(dev, "Sink doesn't support YUV422.\n");
			return false;
		}

		if (bpc != 12) {
			drm_dbg(dev, "YUV422 only supports 12 bpc.\n");
			return false;
		}

		drm_dbg(dev, "YUV422 format supported in that configuration.\n");

		return true;

	case VC4_HDMI_OUTPUT_YUV444:
		drm_dbg(dev, "YUV444 format, checking the constraints.\n");

		if (!(info->color_formats & DRM_COLOR_FORMAT_YCBCR444)) {
			drm_dbg(dev, "Sink doesn't support YUV444.\n");
			return false;
		}

		if (bpc == 10 && !(info->edid_hdmi_ycbcr444_dc_modes & DRM_EDID_HDMI_DC_30)) {
			drm_dbg(dev, "10 BPC but sink doesn't support Deep Color 30.\n");
			return false;
		}

		if (bpc == 12 && !(info->edid_hdmi_ycbcr444_dc_modes & DRM_EDID_HDMI_DC_36)) {
			drm_dbg(dev, "12 BPC but sink doesn't support Deep Color 36.\n");
			return false;
		}

		drm_dbg(dev, "YUV444 format supported in that configuration.\n");

		return true;
	}

	return false;
}

static enum drm_mode_status
vc4_hdmi_encoder_clock_valid(const struct vc4_hdmi *vc4_hdmi,
			     unsigned long long clock)
{
	const struct drm_connector *connector = &vc4_hdmi->connector;
	const struct drm_display_info *info = &connector->display_info;

	if (clock > vc4_hdmi->variant->max_pixel_clock)
		return MODE_CLOCK_HIGH;

	if (vc4_hdmi->disable_4kp60 && clock > HDMI_14_MAX_TMDS_CLK)
		return MODE_CLOCK_HIGH;

	if (info->max_tmds_clock && clock > (info->max_tmds_clock * 1000))
		return MODE_CLOCK_HIGH;

	return MODE_OK;
}

static unsigned long long
vc4_hdmi_encoder_compute_mode_clock(const struct drm_display_mode *mode,
				    unsigned int bpc,
				    enum vc4_hdmi_output_format fmt)
{
	unsigned long long clock = mode->clock * 1000ULL;

	if (mode->flags & DRM_MODE_FLAG_DBLCLK)
		clock = clock * 2;

	if (fmt == VC4_HDMI_OUTPUT_YUV422)
		bpc = 8;

	clock = clock * bpc;
	do_div(clock, 8);

	return clock;
}

static int
vc4_hdmi_encoder_compute_clock(const struct vc4_hdmi *vc4_hdmi,
			       struct vc4_hdmi_connector_state *vc4_state,
			       const struct drm_display_mode *mode,
			       unsigned int bpc, unsigned int fmt)
{
	unsigned long long clock;

	clock = vc4_hdmi_encoder_compute_mode_clock(mode, bpc, fmt);
	if (vc4_hdmi_encoder_clock_valid(vc4_hdmi, clock) != MODE_OK)
		return -EINVAL;

	vc4_state->tmds_char_rate = clock;

	return 0;
}

static int
vc4_hdmi_encoder_compute_format(const struct vc4_hdmi *vc4_hdmi,
				struct vc4_hdmi_connector_state *vc4_state,
				const struct drm_display_mode *mode,
				unsigned int bpc)
{
	struct drm_device *dev = vc4_hdmi->connector.dev;
	const struct drm_connector *connector = &vc4_hdmi->connector;
	const struct drm_display_info *info = &connector->display_info;
	unsigned int format;

	drm_dbg(dev, "Trying with an RGB output\n");

	format = VC4_HDMI_OUTPUT_RGB;
	if (vc4_hdmi_sink_supports_format_bpc(vc4_hdmi, info, mode, format, bpc)) {
		int ret;

		ret = vc4_hdmi_encoder_compute_clock(vc4_hdmi, vc4_state,
						     mode, bpc, format);
		if (!ret) {
			vc4_state->output_format = format;
			return 0;
		}
	}

	drm_dbg(dev, "Failed, Trying with an YUV422 output\n");

	format = VC4_HDMI_OUTPUT_YUV422;
	if (vc4_hdmi_sink_supports_format_bpc(vc4_hdmi, info, mode, format, bpc)) {
		int ret;

		ret = vc4_hdmi_encoder_compute_clock(vc4_hdmi, vc4_state,
						     mode, bpc, format);
		if (!ret) {
			vc4_state->output_format = format;
			return 0;
		}
	}

	drm_dbg(dev, "Failed. No Format Supported for that bpc count.\n");

	return -EINVAL;
}

static int
vc4_hdmi_encoder_compute_config(const struct vc4_hdmi *vc4_hdmi,
				struct vc4_hdmi_connector_state *vc4_state,
				const struct drm_display_mode *mode)
{
	struct drm_device *dev = vc4_hdmi->connector.dev;
	struct drm_connector_state *conn_state = &vc4_state->base;
	unsigned int max_bpc = clamp_t(unsigned int, conn_state->max_bpc, 8, 12);
	unsigned int bpc;
	int ret;

	for (bpc = max_bpc; bpc >= 8; bpc -= 2) {
		drm_dbg(dev, "Trying with a %d bpc output\n", bpc);

		ret = vc4_hdmi_encoder_compute_format(vc4_hdmi, vc4_state,
						      mode, bpc);
		if (ret)
			continue;

		vc4_state->output_bpc = bpc;

		drm_dbg(dev,
			"Mode %ux%u @ %uHz: Found configuration: bpc: %u, fmt: %s, clock: %llu\n",
			mode->hdisplay, mode->vdisplay, drm_mode_vrefresh(mode),
			vc4_state->output_bpc,
			vc4_hdmi_output_fmt_str(vc4_state->output_format),
			vc4_state->tmds_char_rate);

		break;
	}

	return ret;
}

#define WIFI_2_4GHz_CH1_MIN_FREQ	2400000000ULL
#define WIFI_2_4GHz_CH1_MAX_FREQ	2422000000ULL

static int vc4_hdmi_encoder_atomic_check(struct drm_encoder *encoder,
					 struct drm_crtc_state *crtc_state,
					 struct drm_connector_state *conn_state)
{
	struct vc4_hdmi *vc4_hdmi = encoder_to_vc4_hdmi(encoder);
	struct drm_connector *connector = &vc4_hdmi->connector;
	struct drm_connector_state *old_conn_state =
		drm_atomic_get_old_connector_state(conn_state->state, connector);
	struct vc4_hdmi_connector_state *old_vc4_state =
		conn_state_to_vc4_hdmi_conn_state(old_conn_state);
	struct vc4_hdmi_connector_state *vc4_state = conn_state_to_vc4_hdmi_conn_state(conn_state);
	struct drm_display_mode *mode = &crtc_state->adjusted_mode;
	unsigned long long tmds_char_rate = mode->clock * 1000;
	unsigned long long tmds_bit_rate;
	int ret;

	if (vc4_hdmi->variant->unsupported_odd_h_timings) {
		if (mode->flags & DRM_MODE_FLAG_DBLCLK) {
			/* Only try to fixup DBLCLK modes to get 480i and 576i
			 * working.
			 * A generic solution for all modes with odd horizontal
			 * timing values seems impossible based on trying to
			 * solve it for 1366x768 monitors.
			 */
			if ((mode->hsync_start - mode->hdisplay) & 1)
				mode->hsync_start--;
			if ((mode->hsync_end - mode->hsync_start) & 1)
				mode->hsync_end--;
		}

		/* Now check whether we still have odd values remaining */
		if ((mode->hdisplay % 2) || (mode->hsync_start % 2) ||
		    (mode->hsync_end % 2) || (mode->htotal % 2))
			return -EINVAL;
	}

	/*
	 * The 1440p@60 pixel rate is in the same range than the first
	 * WiFi channel (between 2.4GHz and 2.422GHz with 22MHz
	 * bandwidth). Slightly lower the frequency to bring it out of
	 * the WiFi range.
	 */
	tmds_bit_rate = tmds_char_rate * 10;
	if (vc4_hdmi->disable_wifi_frequencies &&
	    (tmds_bit_rate >= WIFI_2_4GHz_CH1_MIN_FREQ &&
	     tmds_bit_rate <= WIFI_2_4GHz_CH1_MAX_FREQ)) {
		mode->clock = 238560;
		tmds_char_rate = mode->clock * 1000;
	}

	ret = vc4_hdmi_encoder_compute_config(vc4_hdmi, vc4_state, mode);
	if (ret)
		return ret;

	/* vc4_hdmi_encoder_compute_config may have changed output_bpc and/or output_format */
	if (vc4_state->output_bpc != old_vc4_state->output_bpc ||
	    vc4_state->output_format != old_vc4_state->output_format)
		crtc_state->mode_changed = true;

	return 0;
}

static enum drm_mode_status
vc4_hdmi_encoder_mode_valid(struct drm_encoder *encoder,
			    const struct drm_display_mode *mode)
{
	struct vc4_hdmi *vc4_hdmi = encoder_to_vc4_hdmi(encoder);

	if (vc4_hdmi->variant->unsupported_odd_h_timings &&
	    !(mode->flags & DRM_MODE_FLAG_DBLCLK) &&
	    ((mode->hdisplay % 2) || (mode->hsync_start % 2) ||
	     (mode->hsync_end % 2) || (mode->htotal % 2)))
		return MODE_H_ILLEGAL;

	return vc4_hdmi_encoder_clock_valid(vc4_hdmi, mode->clock * 1000);
}

static const struct drm_encoder_helper_funcs vc4_hdmi_encoder_helper_funcs = {
	.atomic_check = vc4_hdmi_encoder_atomic_check,
	.atomic_mode_set = vc4_hdmi_encoder_atomic_mode_set,
	.mode_valid = vc4_hdmi_encoder_mode_valid,
};

static u32 vc4_hdmi_channel_map(struct vc4_hdmi *vc4_hdmi, u32 channel_mask)
{
	int i;
	u32 channel_map = 0;

	for (i = 0; i < 8; i++) {
		if (channel_mask & BIT(i))
			channel_map |= i << (3 * i);
	}
	return channel_map;
}

static u32 vc5_hdmi_channel_map(struct vc4_hdmi *vc4_hdmi, u32 channel_mask)
{
	int i;
	u32 channel_map = 0;

	for (i = 0; i < 8; i++) {
		if (channel_mask & BIT(i))
			channel_map |= i << (4 * i);
	}
	return channel_map;
}

static bool vc5_hdmi_hp_detect(struct vc4_hdmi *vc4_hdmi)
{
	unsigned long flags;
	u32 hotplug;

	spin_lock_irqsave(&vc4_hdmi->hw_lock, flags);
	hotplug = HDMI_READ(HDMI_HOTPLUG);
	spin_unlock_irqrestore(&vc4_hdmi->hw_lock, flags);

	return !!(hotplug & VC4_HDMI_HOTPLUG_CONNECTED);
}

/* HDMI audio codec callbacks */
static void vc4_hdmi_audio_set_mai_clock(struct vc4_hdmi *vc4_hdmi,
					 unsigned int samplerate)
{
	u32 hsm_clock = clk_get_rate(vc4_hdmi->audio_clock);
	unsigned long flags;
	unsigned long n, m;

	rational_best_approximation(hsm_clock, samplerate,
				    VC4_HD_MAI_SMP_N_MASK >>
				    VC4_HD_MAI_SMP_N_SHIFT,
				    (VC4_HD_MAI_SMP_M_MASK >>
				     VC4_HD_MAI_SMP_M_SHIFT) + 1,
				    &n, &m);

	spin_lock_irqsave(&vc4_hdmi->hw_lock, flags);
	HDMI_WRITE(HDMI_MAI_SMP,
		   VC4_SET_FIELD(n, VC4_HD_MAI_SMP_N) |
		   VC4_SET_FIELD(m - 1, VC4_HD_MAI_SMP_M));
	spin_unlock_irqrestore(&vc4_hdmi->hw_lock, flags);
}

static void vc4_hdmi_set_n_cts(struct vc4_hdmi *vc4_hdmi, unsigned int samplerate)
{
	const struct drm_display_mode *mode = &vc4_hdmi->saved_adjusted_mode;
	u32 n, cts;
	u64 tmp;

	lockdep_assert_held(&vc4_hdmi->mutex);
	lockdep_assert_held(&vc4_hdmi->hw_lock);

	n = 128 * samplerate / 1000;
	tmp = (u64)(mode->clock * 1000) * n;
	do_div(tmp, 128 * samplerate);
	cts = tmp;

	HDMI_WRITE(HDMI_CRP_CFG,
		   VC4_HDMI_CRP_CFG_EXTERNAL_CTS_EN |
		   VC4_SET_FIELD(n, VC4_HDMI_CRP_CFG_N));

	/*
	 * We could get slightly more accurate clocks in some cases by
	 * providing a CTS_1 value.  The two CTS values are alternated
	 * between based on the period fields
	 */
	HDMI_WRITE(HDMI_CTS_0, cts);
	HDMI_WRITE(HDMI_CTS_1, cts);
}

static inline struct vc4_hdmi *dai_to_hdmi(struct snd_soc_dai *dai)
{
	struct snd_soc_card *card = snd_soc_dai_get_drvdata(dai);

	return snd_soc_card_get_drvdata(card);
}

static bool vc4_hdmi_audio_can_stream(struct vc4_hdmi *vc4_hdmi)
{
	struct drm_display_info *display = &vc4_hdmi->connector.display_info;

	lockdep_assert_held(&vc4_hdmi->mutex);

	/*
	 * If the encoder is currently in DVI mode, treat the codec DAI
	 * as missing.
	 */
	if (!display->is_hdmi)
		return false;

	return true;
}

static int vc4_hdmi_audio_startup(struct device *dev, void *data)
{
	struct vc4_hdmi *vc4_hdmi = dev_get_drvdata(dev);
	unsigned long flags;

	mutex_lock(&vc4_hdmi->mutex);

	if (!vc4_hdmi_audio_can_stream(vc4_hdmi)) {
		mutex_unlock(&vc4_hdmi->mutex);
		return -ENODEV;
	}

	vc4_hdmi->audio.streaming = true;

	spin_lock_irqsave(&vc4_hdmi->hw_lock, flags);
	HDMI_WRITE(HDMI_MAI_CTL,
		   VC4_HD_MAI_CTL_RESET |
		   VC4_HD_MAI_CTL_FLUSH |
		   VC4_HD_MAI_CTL_DLATE |
		   VC4_HD_MAI_CTL_ERRORE |
		   VC4_HD_MAI_CTL_ERRORF);
	spin_unlock_irqrestore(&vc4_hdmi->hw_lock, flags);

	if (vc4_hdmi->variant->phy_rng_enable)
		vc4_hdmi->variant->phy_rng_enable(vc4_hdmi);

	mutex_unlock(&vc4_hdmi->mutex);

	return 0;
}

static void vc4_hdmi_audio_reset(struct vc4_hdmi *vc4_hdmi)
{
	struct drm_encoder *encoder = &vc4_hdmi->encoder.base;
	struct device *dev = &vc4_hdmi->pdev->dev;
	unsigned long flags;
	int ret;

	lockdep_assert_held(&vc4_hdmi->mutex);

	vc4_hdmi->audio.streaming = false;
	ret = vc4_hdmi_stop_packet(encoder, HDMI_INFOFRAME_TYPE_AUDIO, false);
	if (ret)
		dev_err(dev, "Failed to stop audio infoframe: %d\n", ret);

	spin_lock_irqsave(&vc4_hdmi->hw_lock, flags);

	HDMI_WRITE(HDMI_MAI_CTL, VC4_HD_MAI_CTL_RESET);
	HDMI_WRITE(HDMI_MAI_CTL, VC4_HD_MAI_CTL_ERRORF);
	HDMI_WRITE(HDMI_MAI_CTL, VC4_HD_MAI_CTL_FLUSH);

	spin_unlock_irqrestore(&vc4_hdmi->hw_lock, flags);
}

static void vc4_hdmi_audio_shutdown(struct device *dev, void *data)
{
	struct vc4_hdmi *vc4_hdmi = dev_get_drvdata(dev);
	unsigned long flags;

	mutex_lock(&vc4_hdmi->mutex);

	spin_lock_irqsave(&vc4_hdmi->hw_lock, flags);

	HDMI_WRITE(HDMI_MAI_CTL,
		   VC4_HD_MAI_CTL_DLATE |
		   VC4_HD_MAI_CTL_ERRORE |
		   VC4_HD_MAI_CTL_ERRORF);

	spin_unlock_irqrestore(&vc4_hdmi->hw_lock, flags);

	if (vc4_hdmi->variant->phy_rng_disable)
		vc4_hdmi->variant->phy_rng_disable(vc4_hdmi);

	vc4_hdmi->audio.streaming = false;
	vc4_hdmi_audio_reset(vc4_hdmi);

	mutex_unlock(&vc4_hdmi->mutex);
}

static int sample_rate_to_mai_fmt(int samplerate)
{
	switch (samplerate) {
	case 8000:
		return VC4_HDMI_MAI_SAMPLE_RATE_8000;
	case 11025:
		return VC4_HDMI_MAI_SAMPLE_RATE_11025;
	case 12000:
		return VC4_HDMI_MAI_SAMPLE_RATE_12000;
	case 16000:
		return VC4_HDMI_MAI_SAMPLE_RATE_16000;
	case 22050:
		return VC4_HDMI_MAI_SAMPLE_RATE_22050;
	case 24000:
		return VC4_HDMI_MAI_SAMPLE_RATE_24000;
	case 32000:
		return VC4_HDMI_MAI_SAMPLE_RATE_32000;
	case 44100:
		return VC4_HDMI_MAI_SAMPLE_RATE_44100;
	case 48000:
		return VC4_HDMI_MAI_SAMPLE_RATE_48000;
	case 64000:
		return VC4_HDMI_MAI_SAMPLE_RATE_64000;
	case 88200:
		return VC4_HDMI_MAI_SAMPLE_RATE_88200;
	case 96000:
		return VC4_HDMI_MAI_SAMPLE_RATE_96000;
	case 128000:
		return VC4_HDMI_MAI_SAMPLE_RATE_128000;
	case 176400:
		return VC4_HDMI_MAI_SAMPLE_RATE_176400;
	case 192000:
		return VC4_HDMI_MAI_SAMPLE_RATE_192000;
	default:
		return VC4_HDMI_MAI_SAMPLE_RATE_NOT_INDICATED;
	}
}

/* HDMI audio codec callbacks */
static int vc4_hdmi_audio_prepare(struct device *dev, void *data,
				  struct hdmi_codec_daifmt *daifmt,
				  struct hdmi_codec_params *params)
{
	struct vc4_hdmi *vc4_hdmi = dev_get_drvdata(dev);
	struct drm_encoder *encoder = &vc4_hdmi->encoder.base;
	unsigned int sample_rate = params->sample_rate;
	unsigned int channels = params->channels;
	unsigned long flags;
	u32 audio_packet_config, channel_mask;
	u32 channel_map;
	u32 mai_audio_format;
	u32 mai_sample_rate;

	dev_dbg(dev, "%s: %u Hz, %d bit, %d channels\n", __func__,
		sample_rate, params->sample_width, channels);

	mutex_lock(&vc4_hdmi->mutex);

	if (!vc4_hdmi_audio_can_stream(vc4_hdmi)) {
		mutex_unlock(&vc4_hdmi->mutex);
		return -EINVAL;
	}

	vc4_hdmi_audio_set_mai_clock(vc4_hdmi, sample_rate);

	spin_lock_irqsave(&vc4_hdmi->hw_lock, flags);
	HDMI_WRITE(HDMI_MAI_CTL,
		   VC4_SET_FIELD(channels, VC4_HD_MAI_CTL_CHNUM) |
		   VC4_HD_MAI_CTL_WHOLSMP |
		   VC4_HD_MAI_CTL_CHALIGN |
		   VC4_HD_MAI_CTL_ENABLE);

	mai_sample_rate = sample_rate_to_mai_fmt(sample_rate);
	if (params->iec.status[0] & IEC958_AES0_NONAUDIO &&
	    params->channels == 8)
		mai_audio_format = VC4_HDMI_MAI_FORMAT_HBR;
	else
		mai_audio_format = VC4_HDMI_MAI_FORMAT_PCM;
	HDMI_WRITE(HDMI_MAI_FMT,
		   VC4_SET_FIELD(mai_sample_rate,
				 VC4_HDMI_MAI_FORMAT_SAMPLE_RATE) |
		   VC4_SET_FIELD(mai_audio_format,
				 VC4_HDMI_MAI_FORMAT_AUDIO_FORMAT));

	/* The B frame identifier should match the value used by alsa-lib (8) */
	audio_packet_config =
		VC4_HDMI_AUDIO_PACKET_ZERO_DATA_ON_SAMPLE_FLAT |
		VC4_HDMI_AUDIO_PACKET_ZERO_DATA_ON_INACTIVE_CHANNELS |
		VC4_SET_FIELD(0x8, VC4_HDMI_AUDIO_PACKET_B_FRAME_IDENTIFIER);

	channel_mask = GENMASK(channels - 1, 0);
	audio_packet_config |= VC4_SET_FIELD(channel_mask,
					     VC4_HDMI_AUDIO_PACKET_CEA_MASK);

	/* Set the MAI threshold */
	HDMI_WRITE(HDMI_MAI_THR,
		   VC4_SET_FIELD(0x08, VC4_HD_MAI_THR_PANICHIGH) |
		   VC4_SET_FIELD(0x08, VC4_HD_MAI_THR_PANICLOW) |
		   VC4_SET_FIELD(0x06, VC4_HD_MAI_THR_DREQHIGH) |
		   VC4_SET_FIELD(0x08, VC4_HD_MAI_THR_DREQLOW));

	HDMI_WRITE(HDMI_MAI_CONFIG,
		   VC4_HDMI_MAI_CONFIG_BIT_REVERSE |
		   VC4_HDMI_MAI_CONFIG_FORMAT_REVERSE |
		   VC4_SET_FIELD(channel_mask, VC4_HDMI_MAI_CHANNEL_MASK));

	channel_map = vc4_hdmi->variant->channel_map(vc4_hdmi, channel_mask);
	HDMI_WRITE(HDMI_MAI_CHANNEL_MAP, channel_map);
	HDMI_WRITE(HDMI_AUDIO_PACKET_CONFIG, audio_packet_config);

	vc4_hdmi_set_n_cts(vc4_hdmi, sample_rate);

	spin_unlock_irqrestore(&vc4_hdmi->hw_lock, flags);

	memcpy(&vc4_hdmi->audio.infoframe, &params->cea, sizeof(params->cea));
	vc4_hdmi_set_audio_infoframe(encoder);

	mutex_unlock(&vc4_hdmi->mutex);

	return 0;
}

static const struct snd_soc_component_driver vc4_hdmi_audio_cpu_dai_comp = {
	.name = "vc4-hdmi-cpu-dai-component",
	.legacy_dai_naming = 1,
};

static int vc4_hdmi_audio_cpu_dai_probe(struct snd_soc_dai *dai)
{
	struct vc4_hdmi *vc4_hdmi = dai_to_hdmi(dai);

	snd_soc_dai_init_dma_data(dai, &vc4_hdmi->audio.dma_data, NULL);

	return 0;
}

static struct snd_soc_dai_driver vc4_hdmi_audio_cpu_dai_drv = {
	.name = "vc4-hdmi-cpu-dai",
	.probe  = vc4_hdmi_audio_cpu_dai_probe,
	.playback = {
		.stream_name = "Playback",
		.channels_min = 1,
		.channels_max = 8,
		.rates = SNDRV_PCM_RATE_32000 | SNDRV_PCM_RATE_44100 |
			 SNDRV_PCM_RATE_48000 | SNDRV_PCM_RATE_88200 |
			 SNDRV_PCM_RATE_96000 | SNDRV_PCM_RATE_176400 |
			 SNDRV_PCM_RATE_192000,
		.formats = SNDRV_PCM_FMTBIT_IEC958_SUBFRAME_LE,
	},
};

static const struct snd_dmaengine_pcm_config pcm_conf = {
	.chan_names[SNDRV_PCM_STREAM_PLAYBACK] = "audio-rx",
	.prepare_slave_config = snd_dmaengine_pcm_prepare_slave_config,
};

static int vc4_hdmi_audio_get_eld(struct device *dev, void *data,
				  uint8_t *buf, size_t len)
{
	struct vc4_hdmi *vc4_hdmi = dev_get_drvdata(dev);
	struct drm_connector *connector = &vc4_hdmi->connector;

	mutex_lock(&vc4_hdmi->mutex);
	memcpy(buf, connector->eld, min(sizeof(connector->eld), len));
	mutex_unlock(&vc4_hdmi->mutex);

	return 0;
}

static const struct hdmi_codec_ops vc4_hdmi_codec_ops = {
	.get_eld = vc4_hdmi_audio_get_eld,
	.prepare = vc4_hdmi_audio_prepare,
	.audio_shutdown = vc4_hdmi_audio_shutdown,
	.audio_startup = vc4_hdmi_audio_startup,
};

static struct hdmi_codec_pdata vc4_hdmi_codec_pdata = {
	.ops = &vc4_hdmi_codec_ops,
	.max_i2s_channels = 8,
	.i2s = 1,
};

static int vc4_hdmi_audio_init(struct vc4_hdmi *vc4_hdmi)
{
	const struct vc4_hdmi_register *mai_data =
		&vc4_hdmi->variant->registers[HDMI_MAI_DATA];
	struct snd_soc_dai_link *dai_link = &vc4_hdmi->audio.link;
	struct snd_soc_card *card = &vc4_hdmi->audio.card;
	struct device *dev = &vc4_hdmi->pdev->dev;
	struct platform_device *codec_pdev;
	const __be32 *addr;
	int index, len;
	int ret;

	if (!of_find_property(dev->of_node, "dmas", &len) || !len) {
		dev_warn(dev,
			 "'dmas' DT property is missing or empty, no HDMI audio\n");
		return 0;
	}

	if (mai_data->reg != VC4_HD) {
		WARN_ONCE(true, "MAI isn't in the HD block\n");
		return -EINVAL;
	}

	/*
	 * Get the physical address of VC4_HD_MAI_DATA. We need to retrieve
	 * the bus address specified in the DT, because the physical address
	 * (the one returned by platform_get_resource()) is not appropriate
	 * for DMA transfers.
	 * This VC/MMU should probably be exposed to avoid this kind of hacks.
	 */
	index = of_property_match_string(dev->of_node, "reg-names", "hd");
	/* Before BCM2711, we don't have a named register range */
	if (index < 0)
		index = 1;

	addr = of_get_address(dev->of_node, index, NULL, NULL);

	vc4_hdmi->audio.dma_data.addr = be32_to_cpup(addr) + mai_data->offset;
	vc4_hdmi->audio.dma_data.addr_width = DMA_SLAVE_BUSWIDTH_4_BYTES;
	vc4_hdmi->audio.dma_data.maxburst = 2;

	ret = devm_snd_dmaengine_pcm_register(dev, &pcm_conf, 0);
	if (ret) {
		dev_err(dev, "Could not register PCM component: %d\n", ret);
		return ret;
	}

	ret = devm_snd_soc_register_component(dev, &vc4_hdmi_audio_cpu_dai_comp,
					      &vc4_hdmi_audio_cpu_dai_drv, 1);
	if (ret) {
		dev_err(dev, "Could not register CPU DAI: %d\n", ret);
		return ret;
	}

	codec_pdev = platform_device_register_data(dev, HDMI_CODEC_DRV_NAME,
						   PLATFORM_DEVID_AUTO,
						   &vc4_hdmi_codec_pdata,
						   sizeof(vc4_hdmi_codec_pdata));
	if (IS_ERR(codec_pdev)) {
		dev_err(dev, "Couldn't register the HDMI codec: %ld\n", PTR_ERR(codec_pdev));
		return PTR_ERR(codec_pdev);
	}
	vc4_hdmi->audio.codec_pdev = codec_pdev;

	dai_link->cpus		= &vc4_hdmi->audio.cpu;
	dai_link->codecs	= &vc4_hdmi->audio.codec;
	dai_link->platforms	= &vc4_hdmi->audio.platform;

	dai_link->num_cpus	= 1;
	dai_link->num_codecs	= 1;
	dai_link->num_platforms	= 1;

	dai_link->name = "MAI";
	dai_link->stream_name = "MAI PCM";
	dai_link->codecs->dai_name = "i2s-hifi";
	dai_link->cpus->dai_name = dev_name(dev);
	dai_link->codecs->name = dev_name(&codec_pdev->dev);
	dai_link->platforms->name = dev_name(dev);

	card->dai_link = dai_link;
	card->num_links = 1;
	card->name = vc4_hdmi->variant->card_name;
	card->driver_name = "vc4-hdmi";
	card->dev = dev;
	card->owner = THIS_MODULE;

	/*
	 * Be careful, snd_soc_register_card() calls dev_set_drvdata() and
	 * stores a pointer to the snd card object in dev->driver_data. This
	 * means we cannot use it for something else. The hdmi back-pointer is
	 * now stored in card->drvdata and should be retrieved with
	 * snd_soc_card_get_drvdata() if needed.
	 */
	snd_soc_card_set_drvdata(card, vc4_hdmi);
	ret = devm_snd_soc_register_card(dev, card);
	if (ret)
		dev_err_probe(dev, ret, "Could not register sound card\n");

	return ret;

}

static void vc4_hdmi_audio_exit(struct vc4_hdmi *vc4_hdmi)
{
	platform_device_unregister(vc4_hdmi->audio.codec_pdev);
	vc4_hdmi->audio.codec_pdev = NULL;
}

static irqreturn_t vc4_hdmi_hpd_irq_thread(int irq, void *priv)
{
	struct vc4_hdmi *vc4_hdmi = priv;
	struct drm_connector *connector = &vc4_hdmi->connector;
	struct drm_device *dev = connector->dev;

	if (dev && dev->registered)
		drm_connector_helper_hpd_irq_event(connector);

	return IRQ_HANDLED;
}

static int vc4_hdmi_hotplug_init(struct vc4_hdmi *vc4_hdmi)
{
	struct drm_connector *connector = &vc4_hdmi->connector;
	struct platform_device *pdev = vc4_hdmi->pdev;
	int ret;

	if (vc4_hdmi->variant->external_irq_controller) {
		unsigned int hpd_con = platform_get_irq_byname(pdev, "hpd-connected");
		unsigned int hpd_rm = platform_get_irq_byname(pdev, "hpd-removed");

		ret = request_threaded_irq(hpd_con,
					   NULL,
					   vc4_hdmi_hpd_irq_thread, IRQF_ONESHOT,
					   "vc4 hdmi hpd connected", vc4_hdmi);
		if (ret)
			return ret;

		ret = request_threaded_irq(hpd_rm,
					   NULL,
					   vc4_hdmi_hpd_irq_thread, IRQF_ONESHOT,
					   "vc4 hdmi hpd disconnected", vc4_hdmi);
		if (ret) {
			free_irq(hpd_con, vc4_hdmi);
			return ret;
		}

		connector->polled = DRM_CONNECTOR_POLL_HPD;
	}

	return 0;
}

static void vc4_hdmi_hotplug_exit(struct vc4_hdmi *vc4_hdmi)
{
	struct platform_device *pdev = vc4_hdmi->pdev;

	if (vc4_hdmi->variant->external_irq_controller) {
		free_irq(platform_get_irq_byname(pdev, "hpd-connected"), vc4_hdmi);
		free_irq(platform_get_irq_byname(pdev, "hpd-removed"), vc4_hdmi);
	}
}

#ifdef CONFIG_DRM_VC4_HDMI_CEC
static irqreturn_t vc4_cec_irq_handler_rx_thread(int irq, void *priv)
{
	struct vc4_hdmi *vc4_hdmi = priv;

	if (vc4_hdmi->cec_rx_msg.len)
		cec_received_msg(vc4_hdmi->cec_adap,
				 &vc4_hdmi->cec_rx_msg);

	return IRQ_HANDLED;
}

static irqreturn_t vc4_cec_irq_handler_tx_thread(int irq, void *priv)
{
	struct vc4_hdmi *vc4_hdmi = priv;

	if (vc4_hdmi->cec_tx_ok) {
		cec_transmit_done(vc4_hdmi->cec_adap, CEC_TX_STATUS_OK,
				  0, 0, 0, 0);
	} else {
		/*
		 * This CEC implementation makes 1 retry, so if we
		 * get a NACK, then that means it made 2 attempts.
		 */
		cec_transmit_done(vc4_hdmi->cec_adap, CEC_TX_STATUS_NACK,
				  0, 2, 0, 0);
	}
	return IRQ_HANDLED;
}

static irqreturn_t vc4_cec_irq_handler_thread(int irq, void *priv)
{
	struct vc4_hdmi *vc4_hdmi = priv;
	irqreturn_t ret;

	if (vc4_hdmi->cec_irq_was_rx)
		ret = vc4_cec_irq_handler_rx_thread(irq, priv);
	else
		ret = vc4_cec_irq_handler_tx_thread(irq, priv);

	return ret;
}

static void vc4_cec_read_msg(struct vc4_hdmi *vc4_hdmi, u32 cntrl1)
{
	struct drm_device *dev = vc4_hdmi->connector.dev;
	struct cec_msg *msg = &vc4_hdmi->cec_rx_msg;
	unsigned int i;

	lockdep_assert_held(&vc4_hdmi->hw_lock);

	msg->len = 1 + ((cntrl1 & VC4_HDMI_CEC_REC_WRD_CNT_MASK) >>
					VC4_HDMI_CEC_REC_WRD_CNT_SHIFT);

	if (msg->len > 16) {
		drm_err(dev, "Attempting to read too much data (%d)\n", msg->len);
		return;
	}

	for (i = 0; i < msg->len; i += 4) {
		u32 val = HDMI_READ(HDMI_CEC_RX_DATA_1 + (i >> 2));

		msg->msg[i] = val & 0xff;
		msg->msg[i + 1] = (val >> 8) & 0xff;
		msg->msg[i + 2] = (val >> 16) & 0xff;
		msg->msg[i + 3] = (val >> 24) & 0xff;
	}
}

static irqreturn_t vc4_cec_irq_handler_tx_bare_locked(struct vc4_hdmi *vc4_hdmi)
{
	u32 cntrl1;

	lockdep_assert_held(&vc4_hdmi->hw_lock);

	cntrl1 = HDMI_READ(HDMI_CEC_CNTRL_1);
	vc4_hdmi->cec_tx_ok = cntrl1 & VC4_HDMI_CEC_TX_STATUS_GOOD;
	cntrl1 &= ~VC4_HDMI_CEC_START_XMIT_BEGIN;
	HDMI_WRITE(HDMI_CEC_CNTRL_1, cntrl1);

	return IRQ_WAKE_THREAD;
}

static irqreturn_t vc4_cec_irq_handler_tx_bare(int irq, void *priv)
{
	struct vc4_hdmi *vc4_hdmi = priv;
	irqreturn_t ret;

	spin_lock(&vc4_hdmi->hw_lock);
	ret = vc4_cec_irq_handler_tx_bare_locked(vc4_hdmi);
	spin_unlock(&vc4_hdmi->hw_lock);

	return ret;
}

static irqreturn_t vc4_cec_irq_handler_rx_bare_locked(struct vc4_hdmi *vc4_hdmi)
{
	u32 cntrl1;

	lockdep_assert_held(&vc4_hdmi->hw_lock);

	vc4_hdmi->cec_rx_msg.len = 0;
	cntrl1 = HDMI_READ(HDMI_CEC_CNTRL_1);
	vc4_cec_read_msg(vc4_hdmi, cntrl1);
	cntrl1 |= VC4_HDMI_CEC_CLEAR_RECEIVE_OFF;
	HDMI_WRITE(HDMI_CEC_CNTRL_1, cntrl1);
	cntrl1 &= ~VC4_HDMI_CEC_CLEAR_RECEIVE_OFF;

	HDMI_WRITE(HDMI_CEC_CNTRL_1, cntrl1);

	return IRQ_WAKE_THREAD;
}

static irqreturn_t vc4_cec_irq_handler_rx_bare(int irq, void *priv)
{
	struct vc4_hdmi *vc4_hdmi = priv;
	irqreturn_t ret;

	spin_lock(&vc4_hdmi->hw_lock);
	ret = vc4_cec_irq_handler_rx_bare_locked(vc4_hdmi);
	spin_unlock(&vc4_hdmi->hw_lock);

	return ret;
}

static irqreturn_t vc4_cec_irq_handler(int irq, void *priv)
{
	struct vc4_hdmi *vc4_hdmi = priv;
	u32 stat = HDMI_READ(HDMI_CEC_CPU_STATUS);
	irqreturn_t ret;
	u32 cntrl5;

	if (!(stat & VC4_HDMI_CPU_CEC))
		return IRQ_NONE;

	spin_lock(&vc4_hdmi->hw_lock);
	cntrl5 = HDMI_READ(HDMI_CEC_CNTRL_5);
	vc4_hdmi->cec_irq_was_rx = cntrl5 & VC4_HDMI_CEC_RX_CEC_INT;
	if (vc4_hdmi->cec_irq_was_rx)
		ret = vc4_cec_irq_handler_rx_bare_locked(vc4_hdmi);
	else
		ret = vc4_cec_irq_handler_tx_bare_locked(vc4_hdmi);

	HDMI_WRITE(HDMI_CEC_CPU_CLEAR, VC4_HDMI_CPU_CEC);
	spin_unlock(&vc4_hdmi->hw_lock);

	return ret;
}

static int vc4_hdmi_cec_enable(struct cec_adapter *adap)
{
	struct vc4_hdmi *vc4_hdmi = cec_get_drvdata(adap);
	/* clock period in microseconds */
	const u32 usecs = 1000000 / CEC_CLOCK_FREQ;
	unsigned long flags;
	u32 val;
	int ret;

	/*
	 * NOTE: This function should really take vc4_hdmi->mutex, but doing so
	 * results in a reentrancy since cec_s_phys_addr_from_edid() called in
	 * .detect or .get_modes might call .adap_enable, which leads to this
	 * function being called with that mutex held.
	 *
	 * Concurrency is not an issue for the moment since we don't share any
	 * state with KMS, so we can ignore the lock for now, but we need to
	 * keep it in mind if we were to change that assumption.
	 */

	ret = pm_runtime_resume_and_get(&vc4_hdmi->pdev->dev);
	if (ret)
		return ret;

	spin_lock_irqsave(&vc4_hdmi->hw_lock, flags);

	val = HDMI_READ(HDMI_CEC_CNTRL_5);
	val &= ~(VC4_HDMI_CEC_TX_SW_RESET | VC4_HDMI_CEC_RX_SW_RESET |
		 VC4_HDMI_CEC_CNT_TO_4700_US_MASK |
		 VC4_HDMI_CEC_CNT_TO_4500_US_MASK);
	val |= ((4700 / usecs) << VC4_HDMI_CEC_CNT_TO_4700_US_SHIFT) |
	       ((4500 / usecs) << VC4_HDMI_CEC_CNT_TO_4500_US_SHIFT);

	HDMI_WRITE(HDMI_CEC_CNTRL_5, val |
		   VC4_HDMI_CEC_TX_SW_RESET | VC4_HDMI_CEC_RX_SW_RESET);
	HDMI_WRITE(HDMI_CEC_CNTRL_5, val);
	HDMI_WRITE(HDMI_CEC_CNTRL_2,
		   ((1500 / usecs) << VC4_HDMI_CEC_CNT_TO_1500_US_SHIFT) |
		   ((1300 / usecs) << VC4_HDMI_CEC_CNT_TO_1300_US_SHIFT) |
		   ((800 / usecs) << VC4_HDMI_CEC_CNT_TO_800_US_SHIFT) |
		   ((600 / usecs) << VC4_HDMI_CEC_CNT_TO_600_US_SHIFT) |
		   ((400 / usecs) << VC4_HDMI_CEC_CNT_TO_400_US_SHIFT));
	HDMI_WRITE(HDMI_CEC_CNTRL_3,
		   ((2750 / usecs) << VC4_HDMI_CEC_CNT_TO_2750_US_SHIFT) |
		   ((2400 / usecs) << VC4_HDMI_CEC_CNT_TO_2400_US_SHIFT) |
		   ((2050 / usecs) << VC4_HDMI_CEC_CNT_TO_2050_US_SHIFT) |
		   ((1700 / usecs) << VC4_HDMI_CEC_CNT_TO_1700_US_SHIFT));
	HDMI_WRITE(HDMI_CEC_CNTRL_4,
		   ((4300 / usecs) << VC4_HDMI_CEC_CNT_TO_4300_US_SHIFT) |
		   ((3900 / usecs) << VC4_HDMI_CEC_CNT_TO_3900_US_SHIFT) |
		   ((3600 / usecs) << VC4_HDMI_CEC_CNT_TO_3600_US_SHIFT) |
		   ((3500 / usecs) << VC4_HDMI_CEC_CNT_TO_3500_US_SHIFT));

	if (!vc4_hdmi->variant->external_irq_controller)
		HDMI_WRITE(HDMI_CEC_CPU_MASK_CLEAR, VC4_HDMI_CPU_CEC);

	spin_unlock_irqrestore(&vc4_hdmi->hw_lock, flags);

	return 0;
}

static int vc4_hdmi_cec_disable(struct cec_adapter *adap)
{
	struct vc4_hdmi *vc4_hdmi = cec_get_drvdata(adap);
	unsigned long flags;

	/*
	 * NOTE: This function should really take vc4_hdmi->mutex, but doing so
	 * results in a reentrancy since cec_s_phys_addr_from_edid() called in
	 * .detect or .get_modes might call .adap_enable, which leads to this
	 * function being called with that mutex held.
	 *
	 * Concurrency is not an issue for the moment since we don't share any
	 * state with KMS, so we can ignore the lock for now, but we need to
	 * keep it in mind if we were to change that assumption.
	 */

	spin_lock_irqsave(&vc4_hdmi->hw_lock, flags);

	if (!vc4_hdmi->variant->external_irq_controller)
		HDMI_WRITE(HDMI_CEC_CPU_MASK_SET, VC4_HDMI_CPU_CEC);

	HDMI_WRITE(HDMI_CEC_CNTRL_5, HDMI_READ(HDMI_CEC_CNTRL_5) |
		   VC4_HDMI_CEC_TX_SW_RESET | VC4_HDMI_CEC_RX_SW_RESET);

	spin_unlock_irqrestore(&vc4_hdmi->hw_lock, flags);

	pm_runtime_put(&vc4_hdmi->pdev->dev);

	return 0;
}

static int vc4_hdmi_cec_adap_enable(struct cec_adapter *adap, bool enable)
{
	if (enable)
		return vc4_hdmi_cec_enable(adap);
	else
		return vc4_hdmi_cec_disable(adap);
}

static int vc4_hdmi_cec_adap_log_addr(struct cec_adapter *adap, u8 log_addr)
{
	struct vc4_hdmi *vc4_hdmi = cec_get_drvdata(adap);
	unsigned long flags;

	/*
	 * NOTE: This function should really take vc4_hdmi->mutex, but doing so
	 * results in a reentrancy since cec_s_phys_addr_from_edid() called in
	 * .detect or .get_modes might call .adap_enable, which leads to this
	 * function being called with that mutex held.
	 *
	 * Concurrency is not an issue for the moment since we don't share any
	 * state with KMS, so we can ignore the lock for now, but we need to
	 * keep it in mind if we were to change that assumption.
	 */

	spin_lock_irqsave(&vc4_hdmi->hw_lock, flags);
	HDMI_WRITE(HDMI_CEC_CNTRL_1,
		   (HDMI_READ(HDMI_CEC_CNTRL_1) & ~VC4_HDMI_CEC_ADDR_MASK) |
		   (log_addr & 0xf) << VC4_HDMI_CEC_ADDR_SHIFT);
	spin_unlock_irqrestore(&vc4_hdmi->hw_lock, flags);

	return 0;
}

static int vc4_hdmi_cec_adap_transmit(struct cec_adapter *adap, u8 attempts,
				      u32 signal_free_time, struct cec_msg *msg)
{
	struct vc4_hdmi *vc4_hdmi = cec_get_drvdata(adap);
	struct drm_device *dev = vc4_hdmi->connector.dev;
	unsigned long flags;
	u32 val;
	unsigned int i;

	/*
	 * NOTE: This function should really take vc4_hdmi->mutex, but doing so
	 * results in a reentrancy since cec_s_phys_addr_from_edid() called in
	 * .detect or .get_modes might call .adap_enable, which leads to this
	 * function being called with that mutex held.
	 *
	 * Concurrency is not an issue for the moment since we don't share any
	 * state with KMS, so we can ignore the lock for now, but we need to
	 * keep it in mind if we were to change that assumption.
	 */

	if (msg->len > 16) {
		drm_err(dev, "Attempting to transmit too much data (%d)\n", msg->len);
		return -ENOMEM;
	}

	spin_lock_irqsave(&vc4_hdmi->hw_lock, flags);

	for (i = 0; i < msg->len; i += 4)
		HDMI_WRITE(HDMI_CEC_TX_DATA_1 + (i >> 2),
			   (msg->msg[i]) |
			   (msg->msg[i + 1] << 8) |
			   (msg->msg[i + 2] << 16) |
			   (msg->msg[i + 3] << 24));

	val = HDMI_READ(HDMI_CEC_CNTRL_1);
	val &= ~VC4_HDMI_CEC_START_XMIT_BEGIN;
	HDMI_WRITE(HDMI_CEC_CNTRL_1, val);
	val &= ~VC4_HDMI_CEC_MESSAGE_LENGTH_MASK;
	val |= (msg->len - 1) << VC4_HDMI_CEC_MESSAGE_LENGTH_SHIFT;
	val |= VC4_HDMI_CEC_START_XMIT_BEGIN;

	HDMI_WRITE(HDMI_CEC_CNTRL_1, val);

	spin_unlock_irqrestore(&vc4_hdmi->hw_lock, flags);

	return 0;
}

static const struct cec_adap_ops vc4_hdmi_cec_adap_ops = {
	.adap_enable = vc4_hdmi_cec_adap_enable,
	.adap_log_addr = vc4_hdmi_cec_adap_log_addr,
	.adap_transmit = vc4_hdmi_cec_adap_transmit,
};

static int vc4_hdmi_cec_init(struct vc4_hdmi *vc4_hdmi)
{
	struct cec_connector_info conn_info;
	struct platform_device *pdev = vc4_hdmi->pdev;
	struct device *dev = &pdev->dev;
	int ret;

	if (!of_find_property(dev->of_node, "interrupts", NULL)) {
		dev_warn(dev, "'interrupts' DT property is missing, no CEC\n");
		return 0;
	}

	vc4_hdmi->cec_adap = cec_allocate_adapter(&vc4_hdmi_cec_adap_ops,
						  vc4_hdmi, "vc4",
						  CEC_CAP_DEFAULTS |
						  CEC_CAP_CONNECTOR_INFO, 1);
	ret = PTR_ERR_OR_ZERO(vc4_hdmi->cec_adap);
	if (ret < 0)
		return ret;

	cec_fill_conn_info_from_drm(&conn_info, &vc4_hdmi->connector);
	cec_s_conn_info(vc4_hdmi->cec_adap, &conn_info);

	if (vc4_hdmi->variant->external_irq_controller) {
		ret = request_threaded_irq(platform_get_irq_byname(pdev, "cec-rx"),
					   vc4_cec_irq_handler_rx_bare,
					   vc4_cec_irq_handler_rx_thread, 0,
					   "vc4 hdmi cec rx", vc4_hdmi);
		if (ret)
			goto err_delete_cec_adap;

		ret = request_threaded_irq(platform_get_irq_byname(pdev, "cec-tx"),
					   vc4_cec_irq_handler_tx_bare,
					   vc4_cec_irq_handler_tx_thread, 0,
					   "vc4 hdmi cec tx", vc4_hdmi);
		if (ret)
			goto err_remove_cec_rx_handler;
	} else {
		ret = request_threaded_irq(platform_get_irq(pdev, 0),
					   vc4_cec_irq_handler,
					   vc4_cec_irq_handler_thread, 0,
					   "vc4 hdmi cec", vc4_hdmi);
		if (ret)
			goto err_delete_cec_adap;
	}

	ret = cec_register_adapter(vc4_hdmi->cec_adap, &pdev->dev);
	if (ret < 0)
		goto err_remove_handlers;

	return 0;

err_remove_handlers:
	if (vc4_hdmi->variant->external_irq_controller)
		free_irq(platform_get_irq_byname(pdev, "cec-tx"), vc4_hdmi);
	else
		free_irq(platform_get_irq(pdev, 0), vc4_hdmi);

err_remove_cec_rx_handler:
	if (vc4_hdmi->variant->external_irq_controller)
		free_irq(platform_get_irq_byname(pdev, "cec-rx"), vc4_hdmi);

err_delete_cec_adap:
	cec_delete_adapter(vc4_hdmi->cec_adap);

	return ret;
}

static void vc4_hdmi_cec_exit(struct vc4_hdmi *vc4_hdmi)
{
	struct platform_device *pdev = vc4_hdmi->pdev;

	if (vc4_hdmi->variant->external_irq_controller) {
		free_irq(platform_get_irq_byname(pdev, "cec-rx"), vc4_hdmi);
		free_irq(platform_get_irq_byname(pdev, "cec-tx"), vc4_hdmi);
	} else {
		free_irq(platform_get_irq(pdev, 0), vc4_hdmi);
	}

	cec_unregister_adapter(vc4_hdmi->cec_adap);
}
#else
static int vc4_hdmi_cec_init(struct vc4_hdmi *vc4_hdmi)
{
	return 0;
}

static void vc4_hdmi_cec_exit(struct vc4_hdmi *vc4_hdmi) {};
#endif

static int vc4_hdmi_build_regset(struct vc4_hdmi *vc4_hdmi,
				 struct debugfs_regset32 *regset,
				 enum vc4_hdmi_regs reg)
{
	const struct vc4_hdmi_variant *variant = vc4_hdmi->variant;
	struct debugfs_reg32 *regs, *new_regs;
	unsigned int count = 0;
	unsigned int i;

	regs = kcalloc(variant->num_registers, sizeof(*regs),
		       GFP_KERNEL);
	if (!regs)
		return -ENOMEM;

	for (i = 0; i < variant->num_registers; i++) {
		const struct vc4_hdmi_register *field =	&variant->registers[i];

		if (field->reg != reg)
			continue;

		regs[count].name = field->name;
		regs[count].offset = field->offset;
		count++;
	}

	new_regs = krealloc(regs, count * sizeof(*regs), GFP_KERNEL);
	if (!new_regs)
		return -ENOMEM;

	regset->base = __vc4_hdmi_get_field_base(vc4_hdmi, reg);
	regset->regs = new_regs;
	regset->nregs = count;

	return 0;
}

static int vc4_hdmi_init_resources(struct vc4_hdmi *vc4_hdmi)
{
	struct platform_device *pdev = vc4_hdmi->pdev;
	struct device *dev = &pdev->dev;
	int ret;

	vc4_hdmi->hdmicore_regs = vc4_ioremap_regs(pdev, 0);
	if (IS_ERR(vc4_hdmi->hdmicore_regs))
		return PTR_ERR(vc4_hdmi->hdmicore_regs);

	vc4_hdmi->hd_regs = vc4_ioremap_regs(pdev, 1);
	if (IS_ERR(vc4_hdmi->hd_regs))
		return PTR_ERR(vc4_hdmi->hd_regs);

	ret = vc4_hdmi_build_regset(vc4_hdmi, &vc4_hdmi->hd_regset, VC4_HD);
	if (ret)
		return ret;

	ret = vc4_hdmi_build_regset(vc4_hdmi, &vc4_hdmi->hdmi_regset, VC4_HDMI);
	if (ret)
		return ret;

	vc4_hdmi->pixel_clock = devm_clk_get(dev, "pixel");
	if (IS_ERR(vc4_hdmi->pixel_clock)) {
		ret = PTR_ERR(vc4_hdmi->pixel_clock);
		if (ret != -EPROBE_DEFER)
			DRM_ERROR("Failed to get pixel clock\n");
		return ret;
	}

	vc4_hdmi->hsm_clock = devm_clk_get(dev, "hdmi");
	if (IS_ERR(vc4_hdmi->hsm_clock)) {
		DRM_ERROR("Failed to get HDMI state machine clock\n");
		return PTR_ERR(vc4_hdmi->hsm_clock);
	}

	vc4_hdmi->audio_clock = vc4_hdmi->hsm_clock;
	vc4_hdmi->cec_clock = vc4_hdmi->hsm_clock;

	vc4_hdmi->hsm_rpm_clock = devm_clk_get(dev, "hdmi");
	if (IS_ERR(vc4_hdmi->hsm_rpm_clock)) {
		DRM_ERROR("Failed to get HDMI state machine clock\n");
		return PTR_ERR(vc4_hdmi->hsm_rpm_clock);
	}

	return 0;
}

static int vc5_hdmi_init_resources(struct vc4_hdmi *vc4_hdmi)
{
	struct platform_device *pdev = vc4_hdmi->pdev;
	struct device *dev = &pdev->dev;
	struct resource *res;
	int ret;

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "hdmi");
	if (!res)
		return -ENODEV;

	vc4_hdmi->hdmicore_regs = devm_ioremap(dev, res->start,
					       resource_size(res));
	if (!vc4_hdmi->hdmicore_regs)
		return -ENOMEM;

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "hd");
	if (!res)
		return -ENODEV;

	vc4_hdmi->hd_regs = devm_ioremap(dev, res->start, resource_size(res));
	if (!vc4_hdmi->hd_regs)
		return -ENOMEM;

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "cec");
	if (!res)
		return -ENODEV;

	vc4_hdmi->cec_regs = devm_ioremap(dev, res->start, resource_size(res));
	if (!vc4_hdmi->cec_regs)
		return -ENOMEM;

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "csc");
	if (!res)
		return -ENODEV;

	vc4_hdmi->csc_regs = devm_ioremap(dev, res->start, resource_size(res));
	if (!vc4_hdmi->csc_regs)
		return -ENOMEM;

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "dvp");
	if (!res)
		return -ENODEV;

	vc4_hdmi->dvp_regs = devm_ioremap(dev, res->start, resource_size(res));
	if (!vc4_hdmi->dvp_regs)
		return -ENOMEM;

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "phy");
	if (!res)
		return -ENODEV;

	vc4_hdmi->phy_regs = devm_ioremap(dev, res->start, resource_size(res));
	if (!vc4_hdmi->phy_regs)
		return -ENOMEM;

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "packet");
	if (!res)
		return -ENODEV;

	vc4_hdmi->ram_regs = devm_ioremap(dev, res->start, resource_size(res));
	if (!vc4_hdmi->ram_regs)
		return -ENOMEM;

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "rm");
	if (!res)
		return -ENODEV;

	vc4_hdmi->rm_regs = devm_ioremap(dev, res->start, resource_size(res));
	if (!vc4_hdmi->rm_regs)
		return -ENOMEM;

	vc4_hdmi->hsm_clock = devm_clk_get(dev, "hdmi");
	if (IS_ERR(vc4_hdmi->hsm_clock)) {
		DRM_ERROR("Failed to get HDMI state machine clock\n");
		return PTR_ERR(vc4_hdmi->hsm_clock);
	}

	vc4_hdmi->hsm_rpm_clock = devm_clk_get(dev, "hdmi");
	if (IS_ERR(vc4_hdmi->hsm_rpm_clock)) {
		DRM_ERROR("Failed to get HDMI state machine clock\n");
		return PTR_ERR(vc4_hdmi->hsm_rpm_clock);
	}

	vc4_hdmi->pixel_bvb_clock = devm_clk_get(dev, "bvb");
	if (IS_ERR(vc4_hdmi->pixel_bvb_clock)) {
		DRM_ERROR("Failed to get pixel bvb clock\n");
		return PTR_ERR(vc4_hdmi->pixel_bvb_clock);
	}

	vc4_hdmi->audio_clock = devm_clk_get(dev, "audio");
	if (IS_ERR(vc4_hdmi->audio_clock)) {
		DRM_ERROR("Failed to get audio clock\n");
		return PTR_ERR(vc4_hdmi->audio_clock);
	}

	vc4_hdmi->cec_clock = devm_clk_get(dev, "cec");
	if (IS_ERR(vc4_hdmi->cec_clock)) {
		DRM_ERROR("Failed to get CEC clock\n");
		return PTR_ERR(vc4_hdmi->cec_clock);
	}

	vc4_hdmi->reset = devm_reset_control_get(dev, NULL);
	if (IS_ERR(vc4_hdmi->reset)) {
		DRM_ERROR("Failed to get HDMI reset line\n");
		return PTR_ERR(vc4_hdmi->reset);
	}

	ret = vc4_hdmi_build_regset(vc4_hdmi, &vc4_hdmi->hdmi_regset, VC4_HDMI);
	if (ret)
		return ret;

	ret = vc4_hdmi_build_regset(vc4_hdmi, &vc4_hdmi->hd_regset, VC4_HD);
	if (ret)
		return ret;

	ret = vc4_hdmi_build_regset(vc4_hdmi, &vc4_hdmi->cec_regset, VC5_CEC);
	if (ret)
		return ret;

	ret = vc4_hdmi_build_regset(vc4_hdmi, &vc4_hdmi->csc_regset, VC5_CSC);
	if (ret)
		return ret;

	ret = vc4_hdmi_build_regset(vc4_hdmi, &vc4_hdmi->dvp_regset, VC5_DVP);
	if (ret)
		return ret;

	ret = vc4_hdmi_build_regset(vc4_hdmi, &vc4_hdmi->phy_regset, VC5_PHY);
	if (ret)
		return ret;

	ret = vc4_hdmi_build_regset(vc4_hdmi, &vc4_hdmi->ram_regset, VC5_RAM);
	if (ret)
		return ret;

	ret = vc4_hdmi_build_regset(vc4_hdmi, &vc4_hdmi->rm_regset, VC5_RM);
	if (ret)
		return ret;

	return 0;
}

static int vc4_hdmi_runtime_suspend(struct device *dev)
{
	struct vc4_hdmi *vc4_hdmi = dev_get_drvdata(dev);

	clk_disable_unprepare(vc4_hdmi->hsm_rpm_clock);

	return 0;
}

static int vc4_hdmi_runtime_resume(struct device *dev)
{
	struct vc4_hdmi *vc4_hdmi = dev_get_drvdata(dev);
	unsigned long __maybe_unused flags;
	u32 __maybe_unused value;
	unsigned long rate;
	int ret;

	/*
	 * The HSM clock is in the HDMI power domain, so we need to set
	 * its frequency while the power domain is active so that it
	 * keeps its rate.
	 */
	ret = clk_set_min_rate(vc4_hdmi->hsm_rpm_clock, HSM_MIN_CLOCK_FREQ);
	if (ret)
		return ret;

	ret = clk_prepare_enable(vc4_hdmi->hsm_rpm_clock);
	if (ret)
		return ret;

	/*
	 * Whenever the RaspberryPi boots without an HDMI monitor
	 * plugged in, the firmware won't have initialized the HSM clock
	 * rate and it will be reported as 0.
	 *
	 * If we try to access a register of the controller in such a
	 * case, it will lead to a silent CPU stall. Let's make sure we
	 * prevent such a case.
	 */
	rate = clk_get_rate(vc4_hdmi->hsm_rpm_clock);
	if (!rate) {
		ret = -EINVAL;
		goto err_disable_clk;
	}

	if (vc4_hdmi->variant->reset)
		vc4_hdmi->variant->reset(vc4_hdmi);

#ifdef CONFIG_DRM_VC4_HDMI_CEC
	spin_lock_irqsave(&vc4_hdmi->hw_lock, flags);
	value = HDMI_READ(HDMI_CEC_CNTRL_1);
	/* Set the logical address to Unregistered */
	value |= VC4_HDMI_CEC_ADDR_MASK;
	HDMI_WRITE(HDMI_CEC_CNTRL_1, value);
	spin_unlock_irqrestore(&vc4_hdmi->hw_lock, flags);

	vc4_hdmi_cec_update_clk_div(vc4_hdmi);

	if (!vc4_hdmi->variant->external_irq_controller) {
		spin_lock_irqsave(&vc4_hdmi->hw_lock, flags);
		HDMI_WRITE(HDMI_CEC_CPU_MASK_SET, 0xffffffff);
		spin_unlock_irqrestore(&vc4_hdmi->hw_lock, flags);
	}
#endif

	return 0;

err_disable_clk:
	clk_disable_unprepare(vc4_hdmi->hsm_clock);
	return ret;
}

static int vc4_hdmi_bind(struct device *dev, struct device *master, void *data)
{
	const struct vc4_hdmi_variant *variant = of_device_get_match_data(dev);
	struct platform_device *pdev = to_platform_device(dev);
	struct drm_device *drm = dev_get_drvdata(master);
	struct vc4_hdmi *vc4_hdmi;
	struct drm_encoder *encoder;
	struct device_node *ddc_node;
	int ret;

	vc4_hdmi = devm_kzalloc(dev, sizeof(*vc4_hdmi), GFP_KERNEL);
	if (!vc4_hdmi)
		return -ENOMEM;
	mutex_init(&vc4_hdmi->mutex);
	spin_lock_init(&vc4_hdmi->hw_lock);
	INIT_DELAYED_WORK(&vc4_hdmi->scrambling_work, vc4_hdmi_scrambling_wq);

	dev_set_drvdata(dev, vc4_hdmi);
	encoder = &vc4_hdmi->encoder.base;
	vc4_hdmi->encoder.type = variant->encoder_type;
	vc4_hdmi->encoder.pre_crtc_configure = vc4_hdmi_encoder_pre_crtc_configure;
	vc4_hdmi->encoder.pre_crtc_enable = vc4_hdmi_encoder_pre_crtc_enable;
	vc4_hdmi->encoder.post_crtc_enable = vc4_hdmi_encoder_post_crtc_enable;
	vc4_hdmi->encoder.post_crtc_disable = vc4_hdmi_encoder_post_crtc_disable;
	vc4_hdmi->encoder.post_crtc_powerdown = vc4_hdmi_encoder_post_crtc_powerdown;
	vc4_hdmi->pdev = pdev;
	vc4_hdmi->variant = variant;

	/*
	 * Since we don't know the state of the controller and its
	 * display (if any), let's assume it's always enabled.
	 * vc4_hdmi_disable_scrambling() will thus run at boot, make
	 * sure it's disabled, and avoid any inconsistency.
	 */
	if (variant->max_pixel_clock > HDMI_14_MAX_TMDS_CLK)
		vc4_hdmi->scdc_enabled = true;

	ret = variant->init_resources(vc4_hdmi);
	if (ret)
		return ret;

	ddc_node = of_parse_phandle(dev->of_node, "ddc", 0);
	if (!ddc_node) {
		DRM_ERROR("Failed to find ddc node in device tree\n");
		return -ENODEV;
	}

	vc4_hdmi->ddc = of_find_i2c_adapter_by_node(ddc_node);
	of_node_put(ddc_node);
	if (!vc4_hdmi->ddc) {
		DRM_DEBUG("Failed to get ddc i2c adapter by node\n");
		return -EPROBE_DEFER;
	}

	/* Only use the GPIO HPD pin if present in the DT, otherwise
	 * we'll use the HDMI core's register.
	 */
	vc4_hdmi->hpd_gpio = devm_gpiod_get_optional(dev, "hpd", GPIOD_IN);
	if (IS_ERR(vc4_hdmi->hpd_gpio)) {
		ret = PTR_ERR(vc4_hdmi->hpd_gpio);
		goto err_put_ddc;
	}

	vc4_hdmi->disable_wifi_frequencies =
		of_property_read_bool(dev->of_node, "wifi-2.4ghz-coexistence");

	if (variant->max_pixel_clock == 600000000) {
		struct vc4_dev *vc4 = to_vc4_dev(drm);
		long max_rate = clk_round_rate(vc4->hvs->core_clk, 550000000);

		if (max_rate < 550000000)
			vc4_hdmi->disable_4kp60 = true;
	}

	pm_runtime_enable(dev);

	/*
	 *  We need to have the device powered up at this point to call
	 *  our reset hook and for the CEC init.
	 */
	ret = pm_runtime_resume_and_get(dev);
	if (ret)
		goto err_disable_runtime_pm;

	if ((of_device_is_compatible(dev->of_node, "brcm,bcm2711-hdmi0") ||
	     of_device_is_compatible(dev->of_node, "brcm,bcm2711-hdmi1")) &&
	    HDMI_READ(HDMI_VID_CTL) & VC4_HD_VID_CTL_ENABLE) {
		clk_prepare_enable(vc4_hdmi->pixel_clock);
		clk_prepare_enable(vc4_hdmi->hsm_clock);
		clk_prepare_enable(vc4_hdmi->pixel_bvb_clock);
	}

	drm_simple_encoder_init(drm, encoder, DRM_MODE_ENCODER_TMDS);
	drm_encoder_helper_add(encoder, &vc4_hdmi_encoder_helper_funcs);

	ret = vc4_hdmi_connector_init(drm, vc4_hdmi);
	if (ret)
		goto err_destroy_encoder;

	ret = vc4_hdmi_hotplug_init(vc4_hdmi);
	if (ret)
		goto err_destroy_conn;

	ret = vc4_hdmi_cec_init(vc4_hdmi);
	if (ret)
		goto err_free_hotplug;

	ret = vc4_hdmi_audio_init(vc4_hdmi);
	if (ret)
		goto err_free_cec;

	vc4_debugfs_add_file(drm, variant->debugfs_name,
			     vc4_hdmi_debugfs_regs,
			     vc4_hdmi);

	pm_runtime_put_sync(dev);

	return 0;

err_free_cec:
	vc4_hdmi_cec_exit(vc4_hdmi);
err_free_hotplug:
	vc4_hdmi_hotplug_exit(vc4_hdmi);
err_destroy_conn:
	vc4_hdmi_connector_destroy(&vc4_hdmi->connector);
err_destroy_encoder:
	drm_encoder_cleanup(encoder);
	pm_runtime_put_sync(dev);
err_disable_runtime_pm:
	pm_runtime_disable(dev);
err_put_ddc:
	put_device(&vc4_hdmi->ddc->dev);

	return ret;
}

static void vc4_hdmi_unbind(struct device *dev, struct device *master,
			    void *data)
{
	struct vc4_hdmi *vc4_hdmi;

	/*
	 * ASoC makes it a bit hard to retrieve a pointer to the
	 * vc4_hdmi structure. Registering the card will overwrite our
	 * device drvdata with a pointer to the snd_soc_card structure,
	 * which can then be used to retrieve whatever drvdata we want
	 * to associate.
	 *
	 * However, that doesn't fly in the case where we wouldn't
	 * register an ASoC card (because of an old DT that is missing
	 * the dmas properties for example), then the card isn't
	 * registered and the device drvdata wouldn't be set.
	 *
	 * We can deal with both cases by making sure a snd_soc_card
	 * pointer and a vc4_hdmi structure are pointing to the same
	 * memory address, so we can treat them indistinctly without any
	 * issue.
	 */
	BUILD_BUG_ON(offsetof(struct vc4_hdmi_audio, card) != 0);
	BUILD_BUG_ON(offsetof(struct vc4_hdmi, audio) != 0);
	vc4_hdmi = dev_get_drvdata(dev);

	kfree(vc4_hdmi->hdmi_regset.regs);
	kfree(vc4_hdmi->hd_regset.regs);

	vc4_hdmi_audio_exit(vc4_hdmi);
	vc4_hdmi_cec_exit(vc4_hdmi);
	vc4_hdmi_hotplug_exit(vc4_hdmi);
	vc4_hdmi_connector_destroy(&vc4_hdmi->connector);
	drm_encoder_cleanup(&vc4_hdmi->encoder.base);

	pm_runtime_disable(dev);

	put_device(&vc4_hdmi->ddc->dev);
}

static const struct component_ops vc4_hdmi_ops = {
	.bind   = vc4_hdmi_bind,
	.unbind = vc4_hdmi_unbind,
};

static int vc4_hdmi_dev_probe(struct platform_device *pdev)
{
	return component_add(&pdev->dev, &vc4_hdmi_ops);
}

static int vc4_hdmi_dev_remove(struct platform_device *pdev)
{
	component_del(&pdev->dev, &vc4_hdmi_ops);
	return 0;
}

static const struct vc4_hdmi_variant bcm2835_variant = {
	.encoder_type		= VC4_ENCODER_TYPE_HDMI0,
	.debugfs_name		= "hdmi_regs",
	.card_name		= "vc4-hdmi",
	.max_pixel_clock	= 162000000,
	.registers		= vc4_hdmi_fields,
	.num_registers		= ARRAY_SIZE(vc4_hdmi_fields),

	.init_resources		= vc4_hdmi_init_resources,
	.csc_setup		= vc4_hdmi_csc_setup,
	.reset			= vc4_hdmi_reset,
	.set_timings		= vc4_hdmi_set_timings,
	.phy_init		= vc4_hdmi_phy_init,
	.phy_disable		= vc4_hdmi_phy_disable,
	.phy_rng_enable		= vc4_hdmi_phy_rng_enable,
	.phy_rng_disable	= vc4_hdmi_phy_rng_disable,
	.channel_map		= vc4_hdmi_channel_map,
	.supports_hdr		= false,
};

static const struct vc4_hdmi_variant bcm2711_hdmi0_variant = {
	.encoder_type		= VC4_ENCODER_TYPE_HDMI0,
	.debugfs_name		= "hdmi0_regs",
	.card_name		= "vc4-hdmi-0",
	.max_pixel_clock	= 600000000,
	.registers		= vc5_hdmi_hdmi0_fields,
	.num_registers		= ARRAY_SIZE(vc5_hdmi_hdmi0_fields),
	.phy_lane_mapping	= {
		PHY_LANE_0,
		PHY_LANE_1,
		PHY_LANE_2,
		PHY_LANE_CK,
	},
	.unsupported_odd_h_timings	= true,
	.external_irq_controller	= true,

	.init_resources		= vc5_hdmi_init_resources,
	.csc_setup		= vc5_hdmi_csc_setup,
	.reset			= vc5_hdmi_reset,
	.set_timings		= vc5_hdmi_set_timings,
	.phy_init		= vc5_hdmi_phy_init,
	.phy_disable		= vc5_hdmi_phy_disable,
	.phy_rng_enable		= vc5_hdmi_phy_rng_enable,
	.phy_rng_disable	= vc5_hdmi_phy_rng_disable,
	.channel_map		= vc5_hdmi_channel_map,
	.supports_hdr		= true,
	.hp_detect		= vc5_hdmi_hp_detect,
};

static const struct vc4_hdmi_variant bcm2711_hdmi1_variant = {
	.encoder_type		= VC4_ENCODER_TYPE_HDMI1,
	.debugfs_name		= "hdmi1_regs",
	.card_name		= "vc4-hdmi-1",
	.max_pixel_clock	= HDMI_14_MAX_TMDS_CLK,
	.registers		= vc5_hdmi_hdmi1_fields,
	.num_registers		= ARRAY_SIZE(vc5_hdmi_hdmi1_fields),
	.phy_lane_mapping	= {
		PHY_LANE_1,
		PHY_LANE_0,
		PHY_LANE_CK,
		PHY_LANE_2,
	},
	.unsupported_odd_h_timings	= true,
	.external_irq_controller	= true,

	.init_resources		= vc5_hdmi_init_resources,
	.csc_setup		= vc5_hdmi_csc_setup,
	.reset			= vc5_hdmi_reset,
	.set_timings		= vc5_hdmi_set_timings,
	.phy_init		= vc5_hdmi_phy_init,
	.phy_disable		= vc5_hdmi_phy_disable,
	.phy_rng_enable		= vc5_hdmi_phy_rng_enable,
	.phy_rng_disable	= vc5_hdmi_phy_rng_disable,
	.channel_map		= vc5_hdmi_channel_map,
	.supports_hdr		= true,
	.hp_detect		= vc5_hdmi_hp_detect,
};

static const struct of_device_id vc4_hdmi_dt_match[] = {
	{ .compatible = "brcm,bcm2835-hdmi", .data = &bcm2835_variant },
	{ .compatible = "brcm,bcm2711-hdmi0", .data = &bcm2711_hdmi0_variant },
	{ .compatible = "brcm,bcm2711-hdmi1", .data = &bcm2711_hdmi1_variant },
	{}
};

static const struct dev_pm_ops vc4_hdmi_pm_ops = {
	SET_RUNTIME_PM_OPS(vc4_hdmi_runtime_suspend,
			   vc4_hdmi_runtime_resume,
			   NULL)
};

struct platform_driver vc4_hdmi_driver = {
	.probe = vc4_hdmi_dev_probe,
	.remove = vc4_hdmi_dev_remove,
	.driver = {
		.name = "vc4_hdmi",
		.of_match_table = vc4_hdmi_dt_match,
		.pm = &vc4_hdmi_pm_ops,
	},
};
