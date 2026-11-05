// SPDX-License-Identifier: GPL-2.0-only
/*
 * Innioasis Y1: the MT6323 PMIC's audio codec, headphone playback (docs/planning/Y1-AUDIO-PLAN.md,
 * stage B). The sequences are the stock 3.0.2 HAL's (AudioPlatformDevice::AnalogOpen and
 * AudioMachineDevice::AnalogOpen/Close, stage 0), reached through the MT6323 regmap, so the Y1 PMIC
 * wrapper's write policy bounds every register and value. The DC trim is 0 (stock measures it once into
 * NVRAM); the headphone gain starts at step 2, stock's power-up value (step 0 was inaudible).
 * Stage C: the power-up and power-down are a DAPM DAC event, so DAPM powers the codec before the AW87559
 * speaker amplifier (an aux device fed from HPOUT) and down after it, the stock order.
 * Reborn (docs/planning/Y1-REBORN-PLAN.md, stage R3) sets a "Master" element by dB, so the gain is that
 * control with a dB scale: step 7 = 0 dB, 2 dB a step (the steps are the stock HAL's; the absolute level is
 * not measured). Reborn's -24 dB for headphones clamps to step 0.
 */
#include <linux/delay.h>
#include <linux/mfd/mt6397/core.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <sound/soc.h>
#include <sound/tlv.h>

#define TOP_CKPDN_SET	0x10a	/* set/clear registers of TOP_CKPDN1 (0x108); bit 8 the audio clock */
#define TOP_CKPDN_CLR	0x10c
#define AUD_CK		BIT(8)
#define ABB_AFE_CON0	0x4000
#define ABB_AFE_CON1	0x4002
#define ABB_AFE_CON3	0x4006	/* left DC trim */
#define ABB_AFE_CON4	0x4008	/* right DC trim */
#define ABB_AFE_CON10	0x4014
#define ABB_AFE_CON11	0x4016
#define NEWIF_CFG0	0x4024
#define AUDTOP_CON0	0x700
#define AUDTOP_CON4	0x708
#define AUDTOP_CON5	0x70a	/* bits 14:12 and 10:8: headphone gain, 7 loudest, 2 dB steps */
#define AUDTOP_CON6	0x70c

#define RATE_44100	9	/* ABB_AFE_CON1, as the AFE's fs code */
#define NEWIF_44100	7

struct y1_codec {
	struct regmap *map;
	struct mutex lock;
	unsigned int volume;	/* 0..7 */
	bool on;
};

static int y1_codec_power_up(struct y1_codec *c)
{
	struct regmap *map = c->map;
	unsigned int con11;
	int ret;

	/* Digital: clock, NEWIF, rate, trim, the handshake toggle, enable. */
	ret = regmap_write(map, TOP_CKPDN_CLR, AUD_CK);
	if (!ret) ret = regmap_write(map, NEWIF_CFG0, (NEWIF_44100 << 12) | 0x330);
	if (!ret) ret = regmap_update_bits(map, ABB_AFE_CON1, 0xf, RATE_44100);
	if (!ret) ret = regmap_write(map, ABB_AFE_CON3, 0);
	if (!ret) ret = regmap_write(map, ABB_AFE_CON4, 0);
	if (!ret) ret = regmap_update_bits(map, ABB_AFE_CON10, BIT(0), BIT(0));
	if (!ret) ret = regmap_read(map, ABB_AFE_CON11, &con11);
	if (!ret) ret = regmap_update_bits(map, ABB_AFE_CON11, 0x300, (~con11 & 3) << 8);
	if (!ret) ret = regmap_update_bits(map, ABB_AFE_CON0, BIT(0), BIT(0));
	/* Analog: bias, DAC and headphone driver, 10 ms, the final state with the chosen gain. */
	if (!ret) ret = regmap_write(map, AUDTOP_CON6, 0xF7F2);
	if (!ret) ret = regmap_update_bits(map, AUDTOP_CON0, 0xF000, 0x7000);
	if (!ret) ret = regmap_write(map, AUDTOP_CON5, 0x0014);
	if (!ret) ret = regmap_write(map, AUDTOP_CON4, 0x007C);
	if (ret)
		return ret;
	msleep(10);
	ret = regmap_write(map, AUDTOP_CON6, 0xF5BA);
	if (!ret) ret = regmap_write(map, AUDTOP_CON5, 0x0014 | (c->volume << 12) | (c->volume << 8));
	return ret;
}

static void y1_codec_power_down(struct y1_codec *c)
{
	struct regmap *map = c->map;

	regmap_write(map, AUDTOP_CON5, 0x0014);
	regmap_write(map, AUDTOP_CON6, 0xF7F2);
	regmap_write(map, AUDTOP_CON4, 0);
	regmap_update_bits(map, AUDTOP_CON0, BIT(12), 0);	/* no uplink exists */
	regmap_write(map, AUDTOP_CON6, 0x37E2);
	regmap_update_bits(map, ABB_AFE_CON0, BIT(0), 0);
	regmap_write(map, TOP_CKPDN_SET, AUD_CK);
}

/* DAPM runs this after the AFE's prepare (the ADDA path is on) and before the amplifier's power-up. */
static int y1_codec_dac_event(struct snd_soc_dapm_widget *w, struct snd_kcontrol *kcontrol, int event)
{
	struct y1_codec *c = snd_soc_component_get_drvdata(snd_soc_dapm_to_component(w->dapm));
	int ret = 0;

	mutex_lock(&c->lock);
	if (event == SND_SOC_DAPM_PRE_PMU && !c->on) {
		ret = y1_codec_power_up(c);
		if (ret)
			y1_codec_power_down(c);
		else
			c->on = true;
	} else if (event == SND_SOC_DAPM_POST_PMD && c->on) {
		y1_codec_power_down(c);
		c->on = false;
	}
	mutex_unlock(&c->lock);
	return ret;
}

static const struct snd_soc_dapm_widget y1_codec_widgets[] = {
	SND_SOC_DAPM_DAC_E("HP DAC", "Playback", SND_SOC_NOPM, 0, 0, y1_codec_dac_event,
			   SND_SOC_DAPM_PRE_PMU | SND_SOC_DAPM_POST_PMD),
	SND_SOC_DAPM_OUTPUT("HPOUT"),
};

static const struct snd_soc_dapm_route y1_codec_routes[] = {
	{ "HPOUT", NULL, "HP DAC" },
};

static struct snd_soc_dai_driver y1_codec_dai = {
	.name = "mt6323-hifi",
	.playback = { .stream_name = "Playback", .channels_min = 2, .channels_max = 2,
		      .rates = SNDRV_PCM_RATE_44100, .formats = SNDRV_PCM_FMTBIT_S16_LE },
};

static const DECLARE_TLV_DB_SCALE(y1_master_tlv, -1400, 200, 0);

static int y1_volume_info(struct snd_kcontrol *kcontrol, struct snd_ctl_elem_info *uinfo)
{
	uinfo->type = SNDRV_CTL_ELEM_TYPE_INTEGER;
	uinfo->count = 1;
	uinfo->value.integer.min = 0;
	uinfo->value.integer.max = 7;
	return 0;
}

static int y1_volume_get(struct snd_kcontrol *kcontrol, struct snd_ctl_elem_value *ucontrol)
{
	struct y1_codec *c = snd_soc_component_get_drvdata(snd_kcontrol_chip(kcontrol));

	ucontrol->value.integer.value[0] = READ_ONCE(c->volume);
	return 0;
}

/* Applied at once while playing (only the gain fields of AUDTOP_CON5 change), else at the next power-up. */
static int y1_volume_put(struct snd_kcontrol *kcontrol, struct snd_ctl_elem_value *ucontrol)
{
	struct y1_codec *c = snd_soc_component_get_drvdata(snd_kcontrol_chip(kcontrol));
	long volume = ucontrol->value.integer.value[0];
	int ret = 0, changed;

	if (volume < 0 || volume > 7)
		return -EINVAL;
	mutex_lock(&c->lock);
	changed = c->volume != volume;
	c->volume = volume;
	if (changed && c->on)
		ret = regmap_update_bits(c->map, AUDTOP_CON5, 0x7700, (volume << 12) | (volume << 8));
	mutex_unlock(&c->lock);
	return ret ? ret : changed;
}

static const struct snd_kcontrol_new y1_codec_controls[] = {
	{ .iface = SNDRV_CTL_ELEM_IFACE_MIXER, .name = "Master Playback Volume",
	  .access = SNDRV_CTL_ELEM_ACCESS_READWRITE | SNDRV_CTL_ELEM_ACCESS_TLV_READ,
	  .tlv.p = y1_master_tlv,
	  .info = y1_volume_info, .get = y1_volume_get, .put = y1_volume_put },
};

static const struct snd_soc_component_driver y1_codec_component = {
	.name = "y1-mt6323-codec",
	.controls = y1_codec_controls, .num_controls = ARRAY_SIZE(y1_codec_controls),
	.dapm_widgets = y1_codec_widgets, .num_dapm_widgets = ARRAY_SIZE(y1_codec_widgets),
	.dapm_routes = y1_codec_routes, .num_dapm_routes = ARRAY_SIZE(y1_codec_routes),
};

static int y1_codec_probe(struct platform_device *pdev)
{
	struct mt6397_chip *chip = dev_get_drvdata(pdev->dev.parent);
	struct y1_codec *c;

	if (!chip || !chip->regmap)
		return -EPROBE_DEFER;
	c = devm_kzalloc(&pdev->dev, sizeof(*c), GFP_KERNEL);
	if (!c)
		return -ENOMEM;
	c->map = chip->regmap;
	c->volume = 2; /* stock's power-up gain */
	mutex_init(&c->lock);
	platform_set_drvdata(pdev, c);
	/* Nothing is written at probe: the codec stays as LK left it until the first playback. */
	return devm_snd_soc_register_component(&pdev->dev, &y1_codec_component, &y1_codec_dai, 1);
}

static const struct of_device_id y1_codec_match[] = { { .compatible = "innioasis,y1-mt6323-codec" }, { } };
MODULE_DEVICE_TABLE(of, y1_codec_match);
static struct platform_driver y1_codec_driver = {
	.probe = y1_codec_probe,
	.driver = { .name = "y1-mt6323-codec", .of_match_table = y1_codec_match },
};
module_platform_driver(y1_codec_driver);
MODULE_DESCRIPTION("Innioasis Y1 MT6323 audio codec (headphones)");
MODULE_LICENSE("GPL");
