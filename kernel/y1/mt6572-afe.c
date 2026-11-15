// SPDX-License-Identifier: GPL-2.0-only
/*
 * Innioasis Y1: MT6572 AFE playback, DL1 -> internal ADDA -> the MT6323 codec
 * (docs/planning/Y1-AUDIO-PLAN.md, stage B). Built on the Y2's MT6582 AFE driver (kernel/audio/
 * mt6582-afe.c: IRQ, DMA and pointer handling, itself adapted from Chris Hendrickson's GPL-2.0 donor);
 * the register sequence is the stock 3.0.2 HAL's (AudioMTKStreamOut::write, stage 0): DL1 I05/I06 to
 * O03/O04 (CONN1 bit 21, CONN2 bit 6), ADDA DL SRC2, I2S_CON1, UL_DL_CON0, AFE on; the analog power-up is
 * the codec's prepare, which ASoC runs after this CPU DAI's; DL1 on at trigger, last, as stock.
 * 44.1 kHz S16 stereo only in this stage (48 kHz differs only in the rate codes).
 *
 * FM capture (docs/planning/Y1-FM-PLAN.md): the stock HAL's FM digital path (AudioMTKStreamInManager
 * Do_input_start, FM branch), register for register the Y2's (kernel/audio/mt6582-afe.c): the MT6627's
 * 32 kHz master I2S into the second I2S input as slave, the I2S ASRC to 48 kHz, I00/I01 -> O05/O06 -> AWB.
 * AFE_ON is held while either stream runs.
 */
#include <linux/clk.h>
#include <linux/dma-mapping.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>

#define AUDIO_TOP_CON0		0x000
#define AFE_DAC_CON0		0x010
#define AFE_DAC_CON1		0x014
#define AFE_I2S_CON		0x018
#define AFE_CONN1		0x024
#define AFE_CONN2		0x028
#define AFE_CONN4		0x030
#define AFE_I2S_CON1		0x034
#define AFE_DL1_BASE		0x040
#define AFE_DL1_CUR		0x044
#define AFE_DL1_END		0x048
#define AFE_AWB_BASE		0x070
#define AFE_AWB_END		0x078
#define AFE_AWB_CUR		0x07c
#define AFE_ADDA_DL_SRC2_CON0	0x108
#define AFE_ADDA_DL_SRC2_CON1	0x10c
#define AFE_ADDA_UL_DL_CON0	0x124
#define AFE_ADDA_PREDIS_CON0	0x260
#define AFE_ADDA_PREDIS_CON1	0x264
#define AFE_IRQ_CON		0x3a0
#define AFE_IRQ_STATUS		0x3a4
#define AFE_IRQ_CLR		0x3a8
#define AFE_IRQ_CNT1		0x3ac
#define AFE_IRQ_CNT2		0x3b0
#define AFE_STOCK_4B8		0x4b8	/* stock HAL init values, meaning not named (stage 0) */
#define AFE_STOCK_4BC		0x4bc
#define AFE_STOCK_4C0		0x4c0
#define AFE_STOCK_4C4		0x4c4	/* bit 4: DAC path; bit 8: the second I2S input's master clock */
#define AFE_ASRC_CON0		0x500
#define AFE_ASRC_CON13		0x550
#define AFE_ASRC_CON14		0x554
#define AFE_ASRC_CON15		0x558
#define AFE_ASRC_CON16		0x55c
#define AFE_ASRC_CON17		0x560
#define AFE_ASRC_CON20		0x56c

#define AFE_ON			BIT(0)
#define DL1_ON			BIT(1)
#define IRQ1			BIT(0)
#define IRQ2			BIT(1)
#define AWB_ON			BIT(6)
#define FS_44100		9
#define FS_48000		10
#define FM_RATE			48000
/* The I2S-in word (bit 31, I2S format, slave), the slave clock, the ASRC bypass, I00->O05 and I01->O06. */
#define I2S_IN_WORD		0x8000000cU
#define I2S_IN_MASTER_CLOCK	BIT(8)
#define ASRC_BYPASS		BIT(30)
#define ASRC_ON			0x41U
#define FM_ROUTE		(BIT(16) | BIT(22))
#define SRC2_44100		0x73001803
#define SRC2_CON1		0xf74f0000
#define PCM_MAX_BYTES		(256U * 1024)

struct y1_afe {
	struct device *dev;
	struct regmap *regmap;
	struct clk *clock;
	struct snd_pcm_substream *substream, *capture;
	spinlock_t lock;
	int irq;
	bool running, capturing;
	/* Streams holding AFE_ON: bit 0 playback, bit 1 FM capture (under the lock). */
	unsigned int users;
};

static const struct snd_pcm_hardware y1_afe_hw = {
	.info = SNDRV_PCM_INFO_MMAP | SNDRV_PCM_INFO_MMAP_VALID |
		SNDRV_PCM_INFO_INTERLEAVED | SNDRV_PCM_INFO_BLOCK_TRANSFER,
	.formats = SNDRV_PCM_FMTBIT_S16_LE,
	.rates = SNDRV_PCM_RATE_44100, .rate_min = 44100, .rate_max = 44100,
	.channels_min = 2, .channels_max = 2,
	.buffer_bytes_max = PCM_MAX_BYTES,
	.period_bytes_min = 512, .period_bytes_max = 128 * 1024,
	.periods_min = 2, .periods_max = 256,
};

/* Called with the lock held, or before the IRQ exists. */
static void y1_afe_stop(struct y1_afe *afe)
{
	regmap_update_bits(afe->regmap, AFE_IRQ_CON, IRQ1, 0);
	regmap_update_bits(afe->regmap, AFE_DAC_CON0, DL1_ON, 0);
	regmap_write(afe->regmap, AFE_IRQ_CLR, IRQ1);
	afe->running = false;
}

/* Called with the lock held, or before the IRQ exists. */
static void y1_capture_stop(struct y1_afe *afe)
{
	regmap_update_bits(afe->regmap, AFE_IRQ_CON, IRQ2, 0);
	regmap_update_bits(afe->regmap, AFE_DAC_CON0, AWB_ON, 0);
	regmap_write(afe->regmap, AFE_IRQ_CLR, IRQ2);
	afe->capturing = false;
}

/* AFE_ON while any stream holds it (stock SetAfeEnable keeps it on while another block runs). */
static int y1_afe_hold(struct y1_afe *afe, unsigned int user, bool hold)
{
	unsigned long flags;
	int ret = 0;

	spin_lock_irqsave(&afe->lock, flags);
	if (hold) {
		if (!afe->users)
			ret = regmap_update_bits(afe->regmap, AFE_DAC_CON0, AFE_ON, AFE_ON);
		if (!ret)
			afe->users |= user;
	} else if (afe->users & user) {
		afe->users &= ~user;
		if (!afe->users)
			ret = regmap_update_bits(afe->regmap, AFE_DAC_CON0, AFE_ON, 0);
	}
	spin_unlock_irqrestore(&afe->lock, flags);
	return ret;
}

static irqreturn_t y1_afe_irq(int irq, void *data)
{
	struct y1_afe *afe = data;
	struct snd_pcm_substream *substream = NULL, *capture = NULL;
	unsigned int status;
	unsigned long flags;

	spin_lock_irqsave(&afe->lock, flags);
	if (regmap_read(afe->regmap, AFE_IRQ_STATUS, &status) || !(status & 0xff)) {
		spin_unlock_irqrestore(&afe->lock, flags);
		return IRQ_NONE;
	}
	regmap_write(afe->regmap, AFE_IRQ_CLR, status & 0xff);
	if ((status & IRQ1) && afe->running)
		substream = afe->substream;
	if ((status & IRQ2) && afe->capturing)
		capture = afe->capture;
	spin_unlock_irqrestore(&afe->lock, flags);
	if (substream)
		snd_pcm_period_elapsed(substream);
	if (capture)
		snd_pcm_period_elapsed(capture);
	return IRQ_HANDLED;
}

static int y1_afe_startup(struct snd_pcm_substream *substream, struct snd_soc_dai *dai)
{
	struct y1_afe *afe = snd_soc_dai_get_drvdata(dai);
	int ret;

	snd_soc_set_runtime_hwparams(substream, &y1_afe_hw);
	ret = snd_pcm_hw_constraint_step(substream->runtime, 0, SNDRV_PCM_HW_PARAM_BUFFER_BYTES, 16);
	if (ret >= 0)
		ret = snd_pcm_hw_constraint_step(substream->runtime, 0, SNDRV_PCM_HW_PARAM_PERIOD_BYTES, 16);
	if (ret >= 0)
		ret = snd_pcm_hw_constraint_integer(substream->runtime, SNDRV_PCM_HW_PARAM_PERIODS);
	if (ret < 0)
		return ret;
	WRITE_ONCE(afe->substream, substream);
	return 0;
}

static int y1_afe_hw_params(struct snd_pcm_substream *substream, struct snd_pcm_hw_params *params,
			    struct snd_soc_dai *dai)
{
	struct y1_afe *afe = snd_soc_dai_get_drvdata(dai);
	struct snd_pcm_runtime *runtime = substream->runtime;
	unsigned int bytes = params_buffer_bytes(params);
	int ret;

	if (params_rate(params) != 44100 || params_channels(params) != 2 ||
	    params_format(params) != SNDRV_PCM_FORMAT_S16_LE || bytes < 1024 || bytes & 15 ||
	    runtime->dma_addr & 15 || runtime->dma_addr + bytes - 1 > 0xffffffffULL)
		return -EINVAL;
	ret = regmap_write(afe->regmap, AFE_DL1_BASE, lower_32_bits(runtime->dma_addr));
	if (!ret)
		ret = regmap_write(afe->regmap, AFE_DL1_END, lower_32_bits(runtime->dma_addr) + bytes - 1);
	/* DL1 rate, stereo (mono bit 21 clear). */
	if (!ret)
		ret = regmap_update_bits(afe->regmap, AFE_DAC_CON1, 0xf | BIT(21), FS_44100);
	return ret;
}

/* The stock HAL's ADDA path, before the codec's analog power-up. */
static int y1_afe_prepare(struct snd_pcm_substream *substream, struct snd_soc_dai *dai)
{
	struct y1_afe *afe = snd_soc_dai_get_drvdata(dai);
	struct regmap *map = afe->regmap;
	int ret;

	ret = regmap_update_bits(map, AFE_CONN1, BIT(21), BIT(21));		/* I05 -> O03 */
	if (!ret) ret = regmap_update_bits(map, AFE_CONN2, BIT(6), BIT(6));	/* I06 -> O04 */
	if (!ret) ret = regmap_write(map, AFE_ADDA_PREDIS_CON0, 0);
	if (!ret) ret = regmap_write(map, AFE_ADDA_PREDIS_CON1, 0);
	if (!ret) ret = regmap_write(map, AFE_ADDA_DL_SRC2_CON0, SRC2_44100);
	if (!ret) ret = regmap_write(map, AFE_ADDA_DL_SRC2_CON1, SRC2_CON1);
	if (!ret) ret = regmap_write(map, AFE_I2S_CON1, (FS_44100 << 8) | BIT(3));
	if (!ret) ret = regmap_update_bits(map, AFE_ADDA_DL_SRC2_CON0, BIT(0), BIT(0));
	if (!ret) ret = regmap_update_bits(map, AFE_I2S_CON1, BIT(0), BIT(0));
	if (!ret) ret = regmap_update_bits(map, AFE_ADDA_UL_DL_CON0, BIT(0), BIT(0));
	if (!ret) ret = regmap_update_bits(map, AFE_STOCK_4C4, BIT(4), 0);
	if (!ret) ret = y1_afe_hold(afe, BIT(0), true);
	return ret;
}

static int y1_afe_hw_free(struct snd_pcm_substream *substream, struct snd_soc_dai *dai)
{
	struct y1_afe *afe = snd_soc_dai_get_drvdata(dai);
	struct regmap *map = afe->regmap;
	unsigned long flags;

	spin_lock_irqsave(&afe->lock, flags);
	y1_afe_stop(afe);
	spin_unlock_irqrestore(&afe->lock, flags);
	synchronize_irq(afe->irq);
	/* The reverse of prepare. */
	y1_afe_hold(afe, BIT(0), false);
	regmap_update_bits(map, AFE_ADDA_UL_DL_CON0, BIT(0), 0);
	regmap_update_bits(map, AFE_I2S_CON1, BIT(0), 0);
	regmap_update_bits(map, AFE_ADDA_DL_SRC2_CON0, BIT(0), 0);
	regmap_update_bits(map, AFE_CONN2, BIT(6), 0);
	regmap_update_bits(map, AFE_CONN1, BIT(21), 0);
	return 0;
}

static void y1_afe_shutdown(struct snd_pcm_substream *substream, struct snd_soc_dai *dai)
{
	struct y1_afe *afe = snd_soc_dai_get_drvdata(dai);

	y1_afe_hw_free(substream, dai);
	WRITE_ONCE(afe->substream, NULL);
}

static int y1_afe_trigger(struct snd_pcm_substream *substream, int cmd, struct snd_soc_dai *dai)
{
	struct y1_afe *afe = snd_soc_dai_get_drvdata(dai);
	snd_pcm_uframes_t period = substream->runtime->period_size;
	unsigned long flags;
	int ret = 0;

	spin_lock_irqsave(&afe->lock, flags);
	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
	case SNDRV_PCM_TRIGGER_RESUME:
		if (!period || period > 0x3ffff) {
			ret = -EINVAL;
			break;
		}
		ret = regmap_update_bits(afe->regmap, AFE_IRQ_CNT1, 0x3ffff, period);
		if (!ret) ret = regmap_update_bits(afe->regmap, AFE_IRQ_CON, 0xf0 | IRQ1, FS_44100 << 4);
		if (!ret) ret = regmap_write(afe->regmap, AFE_IRQ_CLR, IRQ1);
		if (!ret) ret = regmap_update_bits(afe->regmap, AFE_DAC_CON0, DL1_ON, DL1_ON);
		if (!ret) ret = regmap_update_bits(afe->regmap, AFE_IRQ_CON, IRQ1, IRQ1);
		if (ret)
			y1_afe_stop(afe);
		else
			afe->running = true;
		break;
	case SNDRV_PCM_TRIGGER_STOP:
	case SNDRV_PCM_TRIGGER_SUSPEND:
		y1_afe_stop(afe);
		break;
	default:
		ret = -EINVAL;
	}
	spin_unlock_irqrestore(&afe->lock, flags);
	return ret;
}

static const struct snd_soc_dai_ops y1_afe_ops = {
	.startup = y1_afe_startup, .shutdown = y1_afe_shutdown, .hw_params = y1_afe_hw_params,
	.prepare = y1_afe_prepare, .hw_free = y1_afe_hw_free, .trigger = y1_afe_trigger,
};

static const struct snd_pcm_hardware y1_fm_hw = {
	.info = SNDRV_PCM_INFO_MMAP | SNDRV_PCM_INFO_MMAP_VALID |
		SNDRV_PCM_INFO_INTERLEAVED | SNDRV_PCM_INFO_BLOCK_TRANSFER,
	.formats = SNDRV_PCM_FMTBIT_S16_LE,
	.rates = SNDRV_PCM_RATE_48000, .rate_min = FM_RATE, .rate_max = FM_RATE,
	.channels_min = 2, .channels_max = 2,
	.buffer_bytes_max = PCM_MAX_BYTES,
	.period_bytes_min = 512, .period_bytes_max = 128 * 1024,
	.periods_min = 2, .periods_max = 256,
};

/* The reverse of y1_fm_input_on (stock SetFmDirectConnection(0): ASRC off and bypassed, I2S-in off;
 * the route is cleared too, stock leaves it). */
static void y1_fm_input_off(struct y1_afe *afe)
{
	regmap_update_bits(afe->regmap, AFE_CONN2, FM_ROUTE, 0);
	regmap_update_bits(afe->regmap, AFE_I2S_CON, BIT(0), 0);
	regmap_update_bits(afe->regmap, AFE_ASRC_CON0, ASRC_ON, 0);
	regmap_update_bits(afe->regmap, AFE_CONN4, ASRC_BYPASS, ASRC_BYPASS);
}

/* Stock order: Set2ndI2SInConfig(32000, slave), SetI2SASRCConfig(1, 48000), the ASRC on, the I2S-in
 * on, then I00 -> O05 / I01 -> O06. */
static int y1_fm_input_on(struct y1_afe *afe)
{
	static const unsigned int asrc[][2] = {
		{AFE_ASRC_CON14, 0x00600000}, {AFE_ASRC_CON15, 0x00400000},
		{AFE_ASRC_CON17, 0x00000cb2}, {AFE_ASRC_CON16, 0x00075987},
		{AFE_ASRC_CON20, 0x00001b00},
	};
	struct regmap *map = afe->regmap;
	int ret = regmap_update_bits(map, AFE_I2S_CON, ~BIT(0), I2S_IN_WORD);

	if (!ret) ret = regmap_update_bits(map, AFE_STOCK_4C4, I2S_IN_MASTER_CLOCK, 0);
	if (!ret) ret = regmap_update_bits(map, AFE_CONN4, ASRC_BYPASS, 0);
	/* The memif-I2S rate (DAC_CON1 bits 11:8) = the ASRC's output rate. */
	if (!ret) ret = regmap_update_bits(map, AFE_DAC_CON1, 0xf00, FS_48000 << 8);
	if (!ret) ret = regmap_update_bits(map, AFE_ASRC_CON13, BIT(16), 0);
	for (unsigned int i = 0; !ret && i < ARRAY_SIZE(asrc); i++)
		ret = regmap_write(map, asrc[i][0], asrc[i][1]);
	if (!ret) ret = regmap_update_bits(map, AFE_ASRC_CON0, ASRC_ON, ASRC_ON);
	if (!ret) ret = regmap_update_bits(map, AFE_I2S_CON, BIT(0), BIT(0));
	if (!ret) ret = regmap_update_bits(map, AFE_CONN2, FM_ROUTE, FM_ROUTE);
	if (ret)
		y1_fm_input_off(afe);
	return ret;
}

static int y1_fm_startup(struct snd_pcm_substream *substream, struct snd_soc_dai *dai)
{
	struct y1_afe *afe = snd_soc_dai_get_drvdata(dai);
	int ret;

	snd_soc_set_runtime_hwparams(substream, &y1_fm_hw);
	ret = snd_pcm_hw_constraint_step(substream->runtime, 0, SNDRV_PCM_HW_PARAM_BUFFER_BYTES, 16);
	if (ret >= 0)
		ret = snd_pcm_hw_constraint_step(substream->runtime, 0, SNDRV_PCM_HW_PARAM_PERIOD_BYTES, 16);
	if (ret >= 0)
		ret = snd_pcm_hw_constraint_integer(substream->runtime, SNDRV_PCM_HW_PARAM_PERIODS);
	if (ret < 0)
		return ret;
	WRITE_ONCE(afe->capture, substream);
	return 0;
}

static int y1_fm_hw_free(struct snd_pcm_substream *substream, struct snd_soc_dai *dai)
{
	struct y1_afe *afe = snd_soc_dai_get_drvdata(dai);
	unsigned long flags;

	spin_lock_irqsave(&afe->lock, flags);
	y1_capture_stop(afe);
	spin_unlock_irqrestore(&afe->lock, flags);
	synchronize_irq(afe->irq);
	y1_fm_input_off(afe);
	y1_afe_hold(afe, BIT(1), false);
	return 0;
}

static void y1_fm_shutdown(struct snd_pcm_substream *substream, struct snd_soc_dai *dai)
{
	struct y1_afe *afe = snd_soc_dai_get_drvdata(dai);

	y1_fm_hw_free(substream, dai);
	WRITE_ONCE(afe->capture, NULL);
}

static int y1_fm_hw_params(struct snd_pcm_substream *substream, struct snd_pcm_hw_params *params,
			   struct snd_soc_dai *dai)
{
	struct y1_afe *afe = snd_soc_dai_get_drvdata(dai);
	struct snd_pcm_runtime *runtime = substream->runtime;
	unsigned int bytes = params_buffer_bytes(params);
	int ret;

	if (params_rate(params) != FM_RATE || params_channels(params) != 2 ||
	    params_format(params) != SNDRV_PCM_FORMAT_S16_LE || bytes < 1024 || bytes & 15 ||
	    runtime->dma_addr & 15 || runtime->dma_addr + bytes - 1 > 0xffffffffULL)
		return -EINVAL;
	ret = regmap_write(afe->regmap, AFE_AWB_BASE, lower_32_bits(runtime->dma_addr));
	if (!ret)
		ret = regmap_write(afe->regmap, AFE_AWB_END, lower_32_bits(runtime->dma_addr) + bytes - 1);
	/* AWB rate (DAC_CON1 bits 15:12), stereo (mono bit 24 clear). */
	if (!ret)
		ret = regmap_update_bits(afe->regmap, AFE_DAC_CON1, (0xfU << 12) | BIT(24), FS_48000 << 12);
	if (!ret)
		ret = y1_fm_input_on(afe);
	if (!ret)
		ret = y1_afe_hold(afe, BIT(1), true);
	if (!ret)
		dev_info(afe->dev, "FM I2S in -> ASRC -> AWB %u Hz, %u bytes, period %u frames\n",
			 FM_RATE, bytes, params_period_size(params));
	return ret;
}

static int y1_fm_trigger(struct snd_pcm_substream *substream, int cmd, struct snd_soc_dai *dai)
{
	struct y1_afe *afe = snd_soc_dai_get_drvdata(dai);
	snd_pcm_uframes_t period = substream->runtime->period_size;
	unsigned long flags;
	int ret = 0;

	spin_lock_irqsave(&afe->lock, flags);
	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
	case SNDRV_PCM_TRIGGER_RESUME:
		if (!period || period > 0x3ffff) {
			ret = -EINVAL;
			break;
		}
		/* IRQ2 every period at the AWB rate (stock: 800 frames at 16 kHz, 50 ms). */
		ret = regmap_update_bits(afe->regmap, AFE_IRQ_CNT2, 0x3ffff, period);
		if (!ret) ret = regmap_update_bits(afe->regmap, AFE_IRQ_CON, 0xf00 | IRQ2, FS_48000 << 8);
		if (!ret) ret = regmap_write(afe->regmap, AFE_IRQ_CLR, IRQ2);
		if (!ret) ret = regmap_update_bits(afe->regmap, AFE_DAC_CON0, AWB_ON, AWB_ON);
		if (!ret) ret = regmap_update_bits(afe->regmap, AFE_IRQ_CON, IRQ2, IRQ2);
		if (ret)
			y1_capture_stop(afe);
		else
			afe->capturing = true;
		break;
	case SNDRV_PCM_TRIGGER_STOP:
	case SNDRV_PCM_TRIGGER_SUSPEND:
		y1_capture_stop(afe);
		break;
	default:
		ret = -EINVAL;
	}
	spin_unlock_irqrestore(&afe->lock, flags);
	return ret;
}

static const struct snd_soc_dai_ops y1_fm_ops = {
	.startup = y1_fm_startup, .shutdown = y1_fm_shutdown, .hw_params = y1_fm_hw_params,
	.hw_free = y1_fm_hw_free, .trigger = y1_fm_trigger,
};

static struct snd_soc_dai_driver y1_afe_dais[] = {
	{
		.name = "DL1-ADDA", .id = 0,
		.playback = { .stream_name = "Playback", .channels_min = 2, .channels_max = 2,
			      .rates = SNDRV_PCM_RATE_44100, .formats = SNDRV_PCM_FMTBIT_S16_LE },
		.ops = &y1_afe_ops,
	},
	{
		/* The link and its PCM are named after this DAI: Reborn looks for "Y2 FM Capture" in
		 * /proc/asound/pcm, as on the Y2. */
		.name = "Y2 FM Capture", .id = 1,
		.capture = { .stream_name = "FM Capture", .channels_min = 2, .channels_max = 2,
			     .rates = SNDRV_PCM_RATE_48000, .formats = SNDRV_PCM_FMTBIT_S16_LE },
		.ops = &y1_fm_ops,
	},
};

static snd_pcm_uframes_t y1_afe_pointer(struct snd_soc_component *component,
					struct snd_pcm_substream *substream)
{
	struct y1_afe *afe = snd_soc_component_get_drvdata(component);
	struct snd_pcm_runtime *runtime = substream->runtime;
	unsigned int base = lower_32_bits(runtime->dma_addr), cur;
	unsigned int reg = substream->stream == SNDRV_PCM_STREAM_CAPTURE ? AFE_AWB_CUR : AFE_DL1_CUR;

	/* CUR may be 0 before the first fetch; never report a position outside the ring. */
	if (regmap_read(afe->regmap, reg, &cur) || !cur || cur < base ||
	    cur - base >= runtime->dma_bytes)
		return 0;
	return bytes_to_frames(runtime, (cur - base) & ~3U);
}

static int y1_afe_sync_stop(struct snd_soc_component *component, struct snd_pcm_substream *substream)
{
	struct y1_afe *afe = snd_soc_component_get_drvdata(component);

	synchronize_irq(afe->irq);
	return 0;
}

static int y1_afe_pcm_new(struct snd_soc_component *component, struct snd_soc_pcm_runtime *rtd)
{
	return snd_pcm_set_managed_buffer_all(rtd->pcm, SNDRV_DMA_TYPE_DEV, component->dev, 0,
					      PCM_MAX_BYTES);
}

static const struct snd_soc_component_driver y1_afe_component = {
	.name = "y1-mt6572-afe", .pointer = y1_afe_pointer, .sync_stop = y1_afe_sync_stop,
	.pcm_construct = y1_afe_pcm_new,
};

static const struct regmap_config y1_afe_regmap = {
	.reg_bits = 32, .val_bits = 32, .reg_stride = 4, .max_register = AFE_ASRC_CON20, .fast_io = true,
};

static int y1_afe_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct y1_afe *afe;
	void __iomem *base;
	int ret;

	ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(32));
	if (ret)
		return ret;
	afe = devm_kzalloc(dev, sizeof(*afe), GFP_KERNEL);
	if (!afe)
		return -ENOMEM;
	afe->dev = dev;
	spin_lock_init(&afe->lock);
	platform_set_drvdata(pdev, afe);
	base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(base))
		return PTR_ERR(base);
	afe->regmap = devm_regmap_init_mmio(dev, base, &y1_afe_regmap);
	if (IS_ERR(afe->regmap))
		return PTR_ERR(afe->regmap);
	/* Stock AudDrv_Clk_On: MT_CG_AUDIO_SW_CG (topckgen CG1 bit 25), then AUDIO_TOP_CON0. The AFE is
	 * not read before this clock is on (stage A found it gated after LK). */
	afe->clock = devm_clk_get_enabled(dev, "audio");
	if (IS_ERR(afe->clock))
		return dev_err_probe(dev, PTR_ERR(afe->clock), "audio clock\n");
	afe->irq = platform_get_irq(pdev, 0);
	if (afe->irq < 0)
		return afe->irq;
	ret = regmap_write(afe->regmap, AUDIO_TOP_CON0, 0x60004000);
	/* Stock AudDrv_Reg_Reset, then the HAL's init values. */
	if (!ret) ret = regmap_write(afe->regmap, AFE_DAC_CON0, 0);
	if (!ret) ret = regmap_write(afe->regmap, AFE_DAC_CON1, 0);
	if (!ret) ret = regmap_write(afe->regmap, AFE_IRQ_CON, 0);
	if (!ret) ret = regmap_write(afe->regmap, AFE_IRQ_CLR, 0x3f);
	if (!ret) ret = regmap_write(afe->regmap, AFE_STOCK_4B8, 0x0300f872);
	if (!ret) ret = regmap_write(afe->regmap, AFE_STOCK_4BC, 0x9331802f);
	if (!ret) ret = regmap_write(afe->regmap, AFE_STOCK_4C0, 7);
	if (!ret) ret = regmap_write(afe->regmap, AFE_STOCK_4C4, 0);
	if (ret)
		return ret;
	ret = devm_request_irq(dev, afe->irq, y1_afe_irq, 0, "y1-mt6572-afe", afe);
	if (ret)
		return ret;
	ret = devm_snd_soc_register_component(dev, &y1_afe_component, y1_afe_dais,
					      ARRAY_SIZE(y1_afe_dais));
	if (ret)
		return ret;
	dev_info(dev, "DL1 -> I05/I06 -> O03/O04 -> ADDA -> MT6323; 44.1 kHz S16 stereo; FM capture 48 kHz\n");
	return 0;
}

static const struct of_device_id y1_afe_match[] = { { .compatible = "innioasis,y1-mt6572-afe" }, { } };
MODULE_DEVICE_TABLE(of, y1_afe_match);
static struct platform_driver y1_afe_driver = {
	.probe = y1_afe_probe,
	.driver = { .name = "y1-mt6572-afe", .of_match_table = y1_afe_match },
};
module_platform_driver(y1_afe_driver);
MODULE_DESCRIPTION("Innioasis Y1 MT6572 AFE playback to the MT6323 codec, FM capture");
MODULE_LICENSE("GPL");
