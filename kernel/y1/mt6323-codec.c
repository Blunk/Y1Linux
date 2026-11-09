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
 * The headphone jack: stock's ACCDET plug switch, EINT0 on GPIO135, read as a GPIO (low with a plug in, measured on
 * the owner's Y1, 2026-11-09). Each edge restarts a 250 ms quiet period (stock: 256 ms EINT debounce), then the
 * level is reported on the card's jack "Headphone" (input "Y2Audio Headphone", SW_HEADPHONE_INSERT), the Y2's
 * name, so Reborn follows it unchanged. Stock's ACCDET block (headset type, the hook key) is not used.
 */
#include <linux/delay.h>
#include <linux/devm-helpers.h>
#include <linux/gpio/consumer.h>
#include <linux/interrupt.h>
#include <linux/mfd/mt6397/core.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <sound/jack.h>
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

#define JACK_QUIET_MS	250

struct y1_codec {
	struct regmap *map;
	struct mutex lock;
	unsigned int volume;	/* 0..7 */
	bool on;
	struct gpio_desc *hp_det;	/* NULL: no jack described */
	int hp_irq;
	struct snd_soc_component *component;	/* set while the card is bound */
	struct snd_soc_jack jack;
	struct delayed_work jack_work;
	int jack_state;	/* -1 not read yet, 0 empty, 1 headphones */
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

/*
 * The speaker amplifier is fed from HPOUT, so headphones plugged in while the speaker plays hear the speaker's
 * gain. As on the Y2, a plug first turns the gain down to step 0 and the speaker off; Reborn then moves the
 * stream to the headphones with their own gain.
 */
static void y1_jack_quiet_speaker(struct y1_codec *c, struct snd_soc_card *card)
{
	struct snd_kcontrol *kctl;
	bool lowered;

	if (!snd_soc_dapm_get_pin_status(&card->dapm, "Speaker"))
		return;
	mutex_lock(&c->lock);
	lowered = c->volume != 0;
	c->volume = 0;
	if (lowered && c->on)
		regmap_update_bits(c->map, AUDTOP_CON5, 0x7700, 0);
	mutex_unlock(&c->lock);
	kctl = snd_soc_card_get_kcontrol(card, "Master Playback Volume");
	if (lowered && kctl)
		snd_ctl_notify(card->snd_card, SNDRV_CTL_EVENT_MASK_VALUE, &kctl->id);
	snd_soc_dapm_disable_pin(&card->dapm, "Speaker");
	snd_soc_dapm_sync(&card->dapm);
	dev_info(c->component->dev, "headphones plugged: speaker off\n");
}

static void y1_jack_read(struct y1_codec *c)
{
	int plugged = gpiod_get_value_cansleep(c->hp_det);

	if (plugged < 0 || plugged == c->jack_state)
		return;
	if (plugged)
		y1_jack_quiet_speaker(c, c->component->card);
	c->jack_state = plugged;
	snd_soc_jack_report(&c->jack, plugged ? SND_JACK_HEADPHONE : 0, SND_JACK_HEADPHONE);
}

static void y1_jack_work(struct work_struct *work)
{
	y1_jack_read(container_of(to_delayed_work(work), struct y1_codec, jack_work));
}

static irqreturn_t y1_jack_irq(int irq, void *data)
{
	struct y1_codec *c = data;

	mod_delayed_work(system_percpu_wq, &c->jack_work, msecs_to_jiffies(JACK_QUIET_MS));
	return IRQ_HANDLED;
}

/* The jack exists once the card does: read it at once, so Reborn starts with a known state, then follow edges. */
static int y1_codec_component_probe(struct snd_soc_component *component)
{
	struct y1_codec *c = snd_soc_component_get_drvdata(component);
	int ret;

	if (!c->hp_det)
		return 0;
	ret = snd_soc_card_jack_new(component->card, "Headphone", SND_JACK_HEADPHONE, &c->jack);
	if (ret)
		return ret;
	c->component = component;
	c->jack_state = -1;
	y1_jack_read(c);
	enable_irq(c->hp_irq);
	return 0;
}

static void y1_codec_component_remove(struct snd_soc_component *component)
{
	struct y1_codec *c = snd_soc_component_get_drvdata(component);

	if (!c->hp_det)
		return;
	disable_irq(c->hp_irq);
	cancel_delayed_work_sync(&c->jack_work);
	c->component = NULL;
}

static const struct snd_soc_component_driver y1_codec_component = {
	.name = "y1-mt6323-codec",
	.probe = y1_codec_component_probe,
	.remove = y1_codec_component_remove,
	.controls = y1_codec_controls, .num_controls = ARRAY_SIZE(y1_codec_controls),
	.dapm_widgets = y1_codec_widgets, .num_dapm_widgets = ARRAY_SIZE(y1_codec_widgets),
	.dapm_routes = y1_codec_routes, .num_dapm_routes = ARRAY_SIZE(y1_codec_routes),
};

/*
 * The GPIO request sets the pin to GPIO mode; the interrupt's request puts it back in EINT0 mode, the pin's mode
 * as LK leaves it. The pull stays as LK left it (stock: none). Off until the card's jack exists.
 */
static int y1_jack_init(struct platform_device *pdev, struct y1_codec *c)
{
	int ret;

	c->hp_det = devm_gpiod_get_optional(&pdev->dev, "hp-det", GPIOD_IN);
	if (IS_ERR_OR_NULL(c->hp_det))
		return PTR_ERR_OR_ZERO(c->hp_det);
	ret = devm_delayed_work_autocancel(&pdev->dev, &c->jack_work, y1_jack_work);
	if (ret)
		return ret;
	c->hp_irq = gpiod_to_irq(c->hp_det);
	if (c->hp_irq < 0)
		return dev_err_probe(&pdev->dev, c->hp_irq, "headphone jack: no interrupt\n");
	ret = devm_request_irq(&pdev->dev, c->hp_irq, y1_jack_irq, IRQF_TRIGGER_RISING | IRQF_TRIGGER_FALLING |
			       IRQF_NO_AUTOEN, "y1-headphone-jack", c);
	return ret ? dev_err_probe(&pdev->dev, ret, "headphone jack interrupt\n") : 0;
}

static int y1_codec_probe(struct platform_device *pdev)
{
	struct mt6397_chip *chip = dev_get_drvdata(pdev->dev.parent);
	struct y1_codec *c;
	int ret;

	if (!chip || !chip->regmap)
		return -EPROBE_DEFER;
	c = devm_kzalloc(&pdev->dev, sizeof(*c), GFP_KERNEL);
	if (!c)
		return -ENOMEM;
	c->map = chip->regmap;
	c->volume = 2; /* stock's power-up gain */
	mutex_init(&c->lock);
	platform_set_drvdata(pdev, c);
	ret = y1_jack_init(pdev, c);
	if (ret)
		return ret;
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
MODULE_DESCRIPTION("Innioasis Y1 MT6323 audio codec (headphones, headphone jack)");
MODULE_LICENSE("GPL");
