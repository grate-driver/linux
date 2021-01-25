// SPDX-License-Identifier: GPL-2.0-only
/*
 * tegra_rt5631.c - Tegra machine ASoC driver for boards using RT5631 codec.
 *
 * Copyright (c) 2020, Svyatoslav Ryhel and Ion Agorria
 *
 * Based on code copyright/by:
 *
 * Copyright (c) 2012, NVIDIA CORPORATION.  All rights reserved.
 * Author: Stephen Warren <swarren@nvidia.com>
 */

#include <linux/gpio/consumer.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>
#include <linux/slab.h>

#include <sound/core.h>
#include <sound/jack.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>

#include "tegra_asoc_utils.h"

#include "../codecs/rt5631.h"

struct tegra_rt5631 {
	struct tegra_asoc_utils_data util_data;
	struct gpio_desc *gpiod_hp_mute;
	struct gpio_desc *gpiod_hp_det;
};

static int tegra_rt5631_hw_params(struct snd_pcm_substream *substream,
				  struct snd_pcm_hw_params *params)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_dai *codec_dai = asoc_rtd_to_codec(rtd, 0);
	struct snd_soc_card *card = rtd->card;
	struct tegra_rt5631 *machine = snd_soc_card_get_drvdata(card);
	unsigned int srate, mclk;
	int err;

	srate = params_rate(params);
	switch (srate) {
	case 64000:
	case 88200:
	case 96000:
		mclk = 128 * srate;
		break;
	default:
		mclk = 256 * srate;
		break;
	}
	/* FIXME: Codec only requires >= 3MHz if OSR==0 */
	while (mclk < 6000000)
		mclk *= 2;

	err = tegra_asoc_utils_set_rate(&machine->util_data, srate, mclk);
	if (err < 0) {
		dev_err(card->dev, "Can't configure clocks\n");
		return err;
	}

	err = snd_soc_dai_set_sysclk(codec_dai, 0, mclk, SND_SOC_CLOCK_IN);
	if (err < 0) {
		dev_err(card->dev, "codec_dai clock not set\n");
		return err;
	}

	return 0;
}

static struct snd_soc_ops tegra_rt5631_ops = {
	.hw_params = tegra_rt5631_hw_params,
};

static struct snd_soc_jack tegra_rt5631_hp_jack;

static struct snd_soc_jack_pin tegra_rt5631_hp_jack_pins[] = {
	{
		.pin = "Headphone Jack",
		.mask = SND_JACK_HEADPHONE,
	},
};

static struct snd_soc_jack_gpio tegra_rt5631_hp_jack_gpio = {
	.name = "Headphone detection",
	.report = SND_JACK_HEADPHONE,
	.debounce_time = 150,
};

static int tegra_rt5631_event_hp(struct snd_soc_dapm_widget *w,
				 struct snd_kcontrol *k, int event)
{
	struct snd_soc_dapm_context *dapm = w->dapm;
	struct snd_soc_card *card = dapm->card;
	struct tegra_rt5631 *machine = snd_soc_card_get_drvdata(card);

	gpiod_set_value_cansleep(machine->gpiod_hp_mute,
				 !SND_SOC_DAPM_EVENT_ON(event));

	return 0;
}

static const struct snd_soc_dapm_widget tegra_rt5631_dapm_widgets[] = {
	SND_SOC_DAPM_SPK("Int Spk", NULL),
	SND_SOC_DAPM_HP("Headphone Jack", tegra_rt5631_event_hp),
	SND_SOC_DAPM_MIC("Mic Jack", NULL),
	SND_SOC_DAPM_MIC("Int Mic", NULL),
};

static const struct snd_kcontrol_new tegra_rt5631_controls[] = {
	SOC_DAPM_PIN_SWITCH("Int Spk"),
	SOC_DAPM_PIN_SWITCH("Int Mic"),
};

static int tegra_rt5631_init(struct snd_soc_pcm_runtime *rtd)
{
	struct tegra_rt5631 *machine = snd_soc_card_get_drvdata(rtd->card);
	int ret;

	ret = snd_soc_card_jack_new(rtd->card, "Headphone Jack",
				    SND_JACK_HEADPHONE,
				    &tegra_rt5631_hp_jack,
				    tegra_rt5631_hp_jack_pins,
				    ARRAY_SIZE(tegra_rt5631_hp_jack_pins));
	if (ret) {
		dev_err(rtd->dev, "Headset Jack creation failed: %d\n", ret);
		return ret;
	}

	if (machine->gpiod_hp_det) {
		tegra_rt5631_hp_jack_gpio.desc = machine->gpiod_hp_det;

		ret = snd_soc_jack_add_gpios(&tegra_rt5631_hp_jack, 1,
					     &tegra_rt5631_hp_jack_gpio);
		if (ret)
			dev_err(rtd->dev, "Jack GPIOs not added: %d\n", ret);
	}

	return 0;
}

SND_SOC_DAILINK_DEFS(hifi,
	DAILINK_COMP_ARRAY(COMP_EMPTY()),
	DAILINK_COMP_ARRAY(COMP_CODEC(NULL, "rt5631-hifi")),
	DAILINK_COMP_ARRAY(COMP_EMPTY()));

static struct snd_soc_dai_link tegra_rt5631_dai = {
	.name = "RT5631",
	.stream_name = "RT5631 PCM",
	.init = tegra_rt5631_init,
	.ops = &tegra_rt5631_ops,
	.dai_fmt = SND_SOC_DAIFMT_I2S |
		   SND_SOC_DAIFMT_NB_NF |
		   SND_SOC_DAIFMT_CBS_CFS,
	SND_SOC_DAILINK_REG(hifi),
};

static struct snd_soc_card snd_soc_tegra_rt5631 = {
	.name = "tegra-rt5631",
	.owner = THIS_MODULE,
	.dai_link = &tegra_rt5631_dai,
	.num_links = 1,
	.controls = tegra_rt5631_controls,
	.num_controls = ARRAY_SIZE(tegra_rt5631_controls),
	.dapm_widgets = tegra_rt5631_dapm_widgets,
	.num_dapm_widgets = ARRAY_SIZE(tegra_rt5631_dapm_widgets),
	.fully_routed = true,
};

static int tegra_rt5631_probe(struct platform_device *pdev)
{
	struct snd_soc_card *card = &snd_soc_tegra_rt5631;
	struct device_node *np_codec, *np_i2s;
	struct tegra_rt5631 *machine;
	struct gpio_desc *gpiod;
	int ret;

	machine = devm_kzalloc(&pdev->dev, sizeof(*machine), GFP_KERNEL);
	if (!machine)
		return -ENOMEM;

	card->dev = &pdev->dev;
	snd_soc_card_set_drvdata(card, machine);

	gpiod = devm_gpiod_get_optional(&pdev->dev, "nvidia,hp-mute",
					GPIOD_OUT_HIGH);
	if (IS_ERR(gpiod))
		return PTR_ERR(gpiod);

	machine->gpiod_hp_mute = gpiod;

	gpiod = devm_gpiod_get_optional(&pdev->dev, "nvidia,hp-det",
					GPIOD_IN);
	if (IS_ERR(gpiod))
		return PTR_ERR(gpiod);

	machine->gpiod_hp_det = gpiod;

	ret = snd_soc_of_parse_card_name(card, "nvidia,model");
	if (ret)
		return ret;

	ret = snd_soc_of_parse_audio_routing(card, "nvidia,audio-routing");
	if (ret)
		return ret;

	np_codec = of_parse_phandle(pdev->dev.of_node, "nvidia,audio-codec", 0);
	if (!np_codec) {
		dev_err(&pdev->dev,
			"Property 'nvidia,audio-codec' missing or invalid\n");
		return -EINVAL;
	}

	np_i2s = of_parse_phandle(pdev->dev.of_node, "nvidia,i2s-controller", 0);
	if (!np_i2s) {
		dev_err(&pdev->dev,
			"Property 'nvidia,i2s-controller' missing or invalid\n");
		return -EINVAL;
	}

	tegra_rt5631_dai.cpus->of_node = np_i2s;
	tegra_rt5631_dai.codecs->of_node = np_codec;
	tegra_rt5631_dai.platforms->of_node = np_i2s;

	ret = tegra_asoc_utils_init(&machine->util_data, &pdev->dev);
	if (ret)
		return ret;

	ret = devm_snd_soc_register_card(&pdev->dev, card);
	if (ret)
		return ret;

	return 0;
}

static const struct of_device_id tegra_rt5631_of_match[] = {
	{ .compatible = "nvidia,tegra-audio-rt5631", },
	{},
};
MODULE_DEVICE_TABLE(of, tegra_rt5631_of_match);

static struct platform_driver tegra_rt5631_driver = {
	.driver = {
		.name = "tegra-snd-rt5631",
		.pm = &snd_soc_pm_ops,
		.of_match_table = tegra_rt5631_of_match,
	},
	.probe = tegra_rt5631_probe,
};
module_platform_driver(tegra_rt5631_driver);

MODULE_DESCRIPTION("Tegra+RT5631 machine ASoC driver");
MODULE_AUTHOR("Stephen Warren <swarren@nvidia.com>");
MODULE_AUTHOR("Svyatoslav Ryhel <clamor95@gmail.com>");
MODULE_AUTHOR("Ion Agorria <ion@agorria.com>");
MODULE_LICENSE("GPL");
