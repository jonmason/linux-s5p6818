/*
 * es8316.c -- es8316 ALSA SoC audio driver
 * Copyright Everest Semiconductor Co.,Ltd
 *
 * Author: David Yang <yangxiaohua@everest-semi.com>
 *
 * Based on es8316.c
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/init.h>
#include <linux/delay.h>
#include <linux/pm.h>
#include <linux/i2c.h>
#include <linux/spi/spi.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/of_gpio.h>
#include <sound/core.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <sound/tlv.h>
#include <sound/soc.h>
#include <sound/soc-dapm.h>
#include <sound/initval.h>
#include <linux/proc_fs.h>
#include <linux/gpio.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/of.h>
#include "es8316.h"

#if	0
#define DBG(x...) pr_info(x)
#else
#define DBG(x...) do { } while (0)
#endif
#define alsa_dbg DBG

#define INVALID_GPIO -1
int es8316_spk_con_gpio = INVALID_GPIO;
int es8316_hp_con_gpio = INVALID_GPIO;
int es8316_hp_det_gpio = INVALID_GPIO;

#define GPIO_LOW  0
#define GPIO_HIGH 1
#ifndef es8316_DEF_VOL
#define es8316_DEF_VOL			0x1e
#endif

int es8316_jack_insert = 0;
#ifdef CONFIG_SND_CODEC_ES8316_MODULE
EXPORT_SYMBOL(es8316_jack_insert);
#endif

static struct snd_soc_codec *es8316_codec;

void es8316_mono_en(int enable)
{
	struct snd_soc_codec *codec;

	codec = es8316_codec;

	DBG("%s: enable %d\n", __func__, enable);

	if (enable) {
		/* R OUTEN */
		snd_soc_write(codec, ES8316_CPHP_OUTEN_REG17, 0x06);
		/* VPP SET AND ZERO L/R,MONO SET */
		snd_soc_write(codec, ES8316_DAC_SET3_REG32, 0x08);
	} else {
		/* VPP SET AND ZERO L/R,STEREO SET */
		snd_soc_write(codec, ES8316_DAC_SET3_REG32, 0x00);
		/* L/R OUTEN */
		snd_soc_write(codec, ES8316_CPHP_OUTEN_REG17, 0x66);
	}
}
EXPORT_SYMBOL(es8316_mono_en);

static u16 es8316_reg[] = {
	0x03, 0x03, 0x00, 0x20, 0x11, 0x00, 0x11, 0x00,  /* 0 */
	0x00, 0x01, 0x00, 0x00, 0xf8, 0x3f, 0x00, 0x00,  /* 8 */
	0x01, 0xfc, 0x28, 0x00, 0x00, 0x33, 0x00, 0x00,  /* 16 */
	0x88, 0x06, 0x22, 0x03, 0x0f, 0x00, 0x80, 0x80,  /* 24 */
	0x00, 0x00, 0xc0, 0x00, 0x01, 0x08, 0x10, 0xc0,  /* 32 */
	0x00, 0x1c, 0x00, 0xb0, 0x32, 0x03, 0x00, 0x11,  /* 40 */
	0x10, 0x00, 0x00, 0xc0, 0xc0, 0x1f, 0xf7, 0xfd,  /* 48 */
	0xff, 0x1f, 0xf7, 0xfd, 0xff, 0x1f, 0xf7, 0xfd,  /* 56 */
	0xff, 0x1f, 0xf7, 0xfd, 0xff, 0x1f, 0xf7, 0xfd,  /* 64 */
	0xff, 0x1f, 0xf7, 0xfd, 0xff, 0x00, 0x00, 0x00,  /* 72 */
	0x00, 0x00, 0x00, 0x00,                          /* 80 */
};

static unsigned int es8316_read_reg_cache(struct snd_soc_codec *codec,
					  unsigned int reg)
{
	if (reg >= ES8316_REGNUM)
		return -1;
	return es8316_reg[reg];
}
static int es8316_write(struct snd_soc_codec *codec, unsigned int reg,
			unsigned int value)
{
	u8 data[2];
	int ret;

	data[0] = reg;
	data[1] = value & 0x00ff;

	if (reg < ES8316_REGNUM)
		es8316_reg[reg] = value;

	ret = codec->hw_write(codec->control_data, data, 2);
	if (ret == 2)
		return 0;
	if (ret < 0)
		return ret;
	else
		return -EIO;
}

/* codec private data */
struct es8316_priv {
	struct regmap *regmap;
	unsigned int sysclk;
	struct snd_pcm_hw_constraint_list *sysclk_constraints;
};

/*
* es8316_reset
* write value 0xff to reg0x00, the chip will be in reset mode
* then, writer 0x00 to reg0x00, unreset the chip
*/
static int es8316_reset(struct snd_soc_codec *codec)
{
	snd_soc_write(codec, ES8316_RESET_REG00, 0xff);
	return snd_soc_write(codec, ES8316_RESET_REG00, 0x00);
}

/*
 * es8316S Controls
 */
static const DECLARE_TLV_DB_SCALE(dac_vol_tlv, -9600, 50, 1);
static const DECLARE_TLV_DB_SCALE(adc_vol_tlv, -9600, 50, 1);
static const DECLARE_TLV_DB_SCALE(hpmixer_gain_tlv, -1200, 150, 0);
static const DECLARE_TLV_DB_SCALE(mic_bst_tlv, 0, 1200, 0);

/* {0, +3, +6, +9, +12, +15, +18, +21, +24,+27,+30,+33} dB */
static unsigned int linin_pga_tlv[] = {
	TLV_DB_RANGE_HEAD(12),
	0, 0, TLV_DB_SCALE_ITEM(0, 0, 0),
	1, 1, TLV_DB_SCALE_ITEM(300, 0, 0),
	2, 2, TLV_DB_SCALE_ITEM(600, 0, 0),
	3, 3, TLV_DB_SCALE_ITEM(900, 0, 0),
	4, 4, TLV_DB_SCALE_ITEM(1200, 0, 0),
	5, 5, TLV_DB_SCALE_ITEM(1500, 0, 0),
	6, 6, TLV_DB_SCALE_ITEM(1800, 0, 0),
	7, 7, TLV_DB_SCALE_ITEM(2100, 0, 0),
	8, 8, TLV_DB_SCALE_ITEM(2400, 0, 0),
};
static unsigned int hpout_vol_tlv[] = {
	TLV_DB_RANGE_HEAD(1),
	0, 3, TLV_DB_SCALE_ITEM(-4800, 1200, 0),

};
static const char *const alc_func_txt[] = {"Off", "On"};
static const struct soc_enum alc_func =
	SOC_ENUM_SINGLE(ES8316_ADC_ALC1_REG29, 6, 2, alc_func_txt);

static const char *const ng_type_txt[] = {"Constant PGA Gain",
				    "Mute ADC Output"};
static const struct soc_enum ng_type =
	SOC_ENUM_SINGLE(ES8316_ADC_ALC6_REG2E, 6, 2, ng_type_txt);

static const char *const adcpol_txt[] = {"Normal", "Invert"};
static const struct soc_enum adcpol =
	SOC_ENUM_SINGLE(ES8316_ADC_MUTE_REG26, 1, 2, adcpol_txt);
static const char *const dacpol_txt[] = {"Normal", "R Invert", "L Invert",
				   "L + R Invert"};
static const struct soc_enum dacpol =
	SOC_ENUM_SINGLE(ES8316_DAC_SET1_REG30, 0, 4, dacpol_txt);

static const struct snd_kcontrol_new es8316_snd_controls[] = {
	/* HP OUT VOLUME */
	SOC_DOUBLE_TLV("HP Playback Volume", ES8316_CPHP_ICAL_VOL_REG18,
		4, 0, 0, 1, hpout_vol_tlv),
	/* HPMIXER VOLUME Control */
	SOC_DOUBLE_TLV("HPMixer Gain", ES8316_HPMIX_VOL_REG16,
	  0, 4, 7, 0, hpmixer_gain_tlv),

	/* DAC Digital controls */
	SOC_DOUBLE_R_TLV("DAC Playback Volume", ES8316_DAC_VOLL_REG33,
			ES8316_DAC_VOLR_REG34, 0, 0xC0, 1, dac_vol_tlv),

	SOC_SINGLE("Enable DAC Soft Ramp", ES8316_DAC_SET1_REG30, 4, 1, 1),
	SOC_SINGLE("DAC Soft Ramp Rate", ES8316_DAC_SET1_REG30, 2, 4, 0),

	SOC_ENUM("Playback Polarity", dacpol),
	SOC_SINGLE("DAC Notch Filter", ES8316_DAC_SET2_REG31, 6, 1, 0),
	SOC_SINGLE("DAC Double Fs Mode", ES8316_DAC_SET2_REG31, 7, 1, 0),
	SOC_SINGLE("DAC Volume Control-LeR", ES8316_DAC_SET2_REG31, 2, 1, 0),
	SOC_SINGLE("DAC Stereo Enhancement", ES8316_DAC_SET3_REG32, 0, 7, 0),

	/* +20dB D2SE PGA Control */
	SOC_SINGLE_TLV("MIC Boost", ES8316_ADC_D2SEPGA_REG24,
		0, 1, 0, mic_bst_tlv),
	/* 0-+24dB Lineinput PGA Control */
	SOC_SINGLE_TLV("Input PGA", ES8316_ADC_PGAGAIN_REG23,
		4, 8, 0, linin_pga_tlv),

	/* ADC Digital  Control */
	SOC_SINGLE_TLV("ADC Capture Volume", ES8316_ADC_VOLUME_REG27,
	  0, 0xC0, 1, adc_vol_tlv),
	SOC_SINGLE("ADC Soft Ramp", ES8316_ADC_MUTE_REG26, 4, 1, 0),
	SOC_ENUM("Capture Polarity", adcpol),
	SOC_SINGLE("ADC Double FS Mode", ES8316_ADC_DMIC_REG25, 4, 1, 0),
		/* ADC ALC  Control */
	SOC_SINGLE("ALC Capture Target Volume", ES8316_ADC_ALC3_REG2B,
		   4, 10, 0),
	SOC_SINGLE("ALC Capture Max PGA", ES8316_ADC_ALC1_REG29, 0, 28, 0),
	SOC_SINGLE("ALC Capture Min PGA", ES8316_ADC_ALC2_REG2A, 0, 28, 0),
	SOC_ENUM("ALC Capture Function", alc_func),
	SOC_SINGLE("ALC Capture Hold Time", ES8316_ADC_ALC3_REG2B, 0, 10, 0),
	SOC_SINGLE("ALC Capture Decay Time", ES8316_ADC_ALC4_REG2C, 4, 10, 0),
	SOC_SINGLE("ALC Capture Attack Time", ES8316_ADC_ALC4_REG2C, 0, 10, 0),
	SOC_SINGLE("ALC Capture NG Threshold", ES8316_ADC_ALC6_REG2E, 0, 31, 0),
	SOC_ENUM("ALC Capture NG Type", ng_type),
	SOC_SINGLE("ALC Capture NG Switch", ES8316_ADC_ALC6_REG2E, 5, 1, 0),
};

/* Analog Input MUX */
static const char * const es8316_analog_in_txt[] = {
	"lin1-rin1",
	"lin2-rin2",
	"lin1-rin1 with 20db Boost",
	"lin2-rin2 with 20db Boost"
};
static const unsigned int es8316_analog_in_values[] = {
	0,/*1,*/
	1,
	2,
	3
};
static const struct soc_enum es8316_analog_input_enum =
	SOC_VALUE_ENUM_SINGLE(ES8316_ADC_PDN_LINSEL_REG22, 4, 4,
			      ARRAY_SIZE(es8316_analog_in_txt),
			      es8316_analog_in_txt,
			      es8316_analog_in_values);
static const struct snd_kcontrol_new es8316_analog_in_mux_controls =
	SOC_DAPM_ENUM("Route", es8316_analog_input_enum);

/* Dmic MUX */
static const char * const es8316_dmic_txt[] = {
	"dmic disable",
	"dmic data at high level",
	"dmic data at low level",
};
static const unsigned int es8316_dmic_values[] = {
	0,/*1,*/
	1,
	2
};
static const struct soc_enum es8316_dmic_src_enum =
	SOC_VALUE_ENUM_SINGLE(ES8316_ADC_DMIC_REG25, 0, 3,
			      ARRAY_SIZE(es8316_dmic_txt),
			      es8316_dmic_txt,
			      es8316_dmic_values);
static const struct snd_kcontrol_new es8316_dmic_src_controls =
	SOC_DAPM_ENUM("Route", es8316_dmic_src_enum);

/* hp mixer mux */
static const char *const es8316_hpmux_texts[] = {
	"lin1-rin1",
	"lin2-rin2",
	"lin-rin with Boost",
	"lin-rin with Boost and PGA"
};

static const unsigned int es8316_hpmux_values[] = {
	0, 1, 2, 3
};

static const struct soc_enum es8316_left_hpmux_enum =
	SOC_VALUE_ENUM_SINGLE(ES8316_HPMIX_SEL_REG13, 4, 7,
			      ARRAY_SIZE(es8316_hpmux_texts),
			      es8316_hpmux_texts,
			      es8316_hpmux_values);
static const struct snd_kcontrol_new es8316_left_hpmux_controls =
	SOC_DAPM_ENUM("Route", es8316_left_hpmux_enum);

static const struct soc_enum es8316_right_hpmux_enum =
	SOC_VALUE_ENUM_SINGLE(ES8316_HPMIX_SEL_REG13, 0, 7,
			      ARRAY_SIZE(es8316_hpmux_texts),
			      es8316_hpmux_texts,
			      es8316_hpmux_values);
static const struct snd_kcontrol_new es8316_right_hpmux_controls =
	SOC_DAPM_ENUM("Route", es8316_right_hpmux_enum);

/* headphone Output Mixer */
static const struct snd_kcontrol_new es8316_out_left_mix[] = {
	SOC_DAPM_SINGLE("LLIN Switch", ES8316_HPMIX_SWITCH_REG14,
			6, 1, 0),
	SOC_DAPM_SINGLE("Left DAC Switch", ES8316_HPMIX_SWITCH_REG14,
			7, 1, 0),
};
static const struct snd_kcontrol_new es8316_out_right_mix[] = {
	SOC_DAPM_SINGLE("RLIN Switch", ES8316_HPMIX_SWITCH_REG14,
			2, 1, 0),
	SOC_DAPM_SINGLE("Right DAC Switch", ES8316_HPMIX_SWITCH_REG14,
			3, 1, 0),
};

/* DAC data source mux */
static const char *const es8316_dacsrc_texts[] = {
	"LDATA TO LDAC, RDATA TO RDAC",
	"LDATA TO LDAC, LDATA TO RDAC",
	"RDATA TO LDAC, RDATA TO RDAC",
	"RDATA TO LDAC, LDATA TO RDAC",
};

static const unsigned int es8316_dacsrc_values[] = {
	0, 1, 2, 3};

static const struct soc_enum es8316_dacsrc_mux_enum =
	SOC_VALUE_ENUM_SINGLE(ES8316_DAC_SET1_REG30, 6, 4,
			      ARRAY_SIZE(es8316_dacsrc_texts),
			      es8316_dacsrc_texts,
			      es8316_dacsrc_values);
static const struct snd_kcontrol_new es8316_dacsrc_mux_controls =
	SOC_DAPM_ENUM("Route", es8316_dacsrc_mux_enum);

/* dapm widgets */
static const struct snd_soc_dapm_widget es8316_dapm_widgets[] = {
	/* Input Lines */
	SND_SOC_DAPM_INPUT("DMIC"),
	SND_SOC_DAPM_INPUT("MIC1"),
	SND_SOC_DAPM_INPUT("MIC2"),

	SND_SOC_DAPM_MICBIAS("micbias", ES8316_SYS_PDN_REG0D,
			5, 0),

	/* Input MUX */
	SND_SOC_DAPM_MUX("Differential Mux", SND_SOC_NOPM, 0, 0,
		&es8316_analog_in_mux_controls),
	/* D2SE PGA */
	SND_SOC_DAPM_PGA("D2SE PGA", ES8316_ADC_D2SEPGA_REG24,
		0, 0, NULL, 0),
	/* Line input PGA */
	SND_SOC_DAPM_PGA("Line input PGA", ES8316_ADC_PDN_LINSEL_REG22,
		7, 0, NULL, 0),

	/* ADCs */
	SND_SOC_DAPM_ADC("Mono ADC", NULL, ES8316_ADC_PDN_LINSEL_REG22, 6, 0),

	/* Dmic MUX */
	SND_SOC_DAPM_MUX("Digital Mic Mux", SND_SOC_NOPM, 0, 0,
		&es8316_dmic_src_controls),

	/* Digital Interface */
	SND_SOC_DAPM_AIF_OUT("I2S OUT", NULL,  0,
			ES8316_SDP_ADCFMT_REG0A, 6, 0),
	SND_SOC_DAPM_AIF_IN("I2S IN", NULL, 0,
			ES8316_SDP_DACFMT_REG0B, 6, 0),

	/*  DACs DATA SRC MUX */
	SND_SOC_DAPM_MUX("DAC SRC Mux", SND_SOC_NOPM, 0, 0,
		&es8316_dacsrc_mux_controls),
	/*  DACs  */
	SND_SOC_DAPM_DAC("Right DAC", NULL, ES8316_DAC_PDN_REG2F, 0, 0),
	SND_SOC_DAPM_DAC("Left DAC", NULL, ES8316_DAC_PDN_REG2F, 4, 0),

	/* Headphone Output Side */
	/* hpmux for hp mixer */
	SND_SOC_DAPM_MUX("Left Hp mux", SND_SOC_NOPM, 0, 0,
	   &es8316_left_hpmux_controls),
	SND_SOC_DAPM_MUX("Right Hp mux", SND_SOC_NOPM, 0, 0,
	   &es8316_right_hpmux_controls),
	/* Output mixer  */
	SND_SOC_DAPM_MIXER("Left Hp mixer", ES8316_HPMIX_PDN_REG15,
	   4, 0, &es8316_out_left_mix[0], ARRAY_SIZE(es8316_out_left_mix)),
	SND_SOC_DAPM_MIXER("Right Hp mixer", ES8316_HPMIX_PDN_REG15,
	   0, 0, &es8316_out_right_mix[0], ARRAY_SIZE(es8316_out_right_mix)),

	/* Output charge pump */
	SND_SOC_DAPM_PGA("HPCP L", ES8316_CPHP_OUTEN_REG17,
		6, 1, NULL, 0),
	SND_SOC_DAPM_PGA("HPCP R", ES8316_CPHP_OUTEN_REG17,
		2, 1, NULL, 0),
	/* Output Driver */
	SND_SOC_DAPM_PGA("HPVOL L", ES8316_CPHP_OUTEN_REG17,
		5, 1, NULL, 0),
	SND_SOC_DAPM_PGA("HPVOL R", ES8316_CPHP_OUTEN_REG17,
		1, 1, NULL, 0),

	/* Output Lines */
	SND_SOC_DAPM_OUTPUT("HPOL"),
	SND_SOC_DAPM_OUTPUT("HPOR"),
};

static const struct snd_soc_dapm_route es8316_dapm_routes[] = {
	/*
	 * record route map
	 */
	{"Differential Mux", "lin1-rin1", "MIC1"},
	{"Differential Mux", "lin2-rin2", "MIC2"},
	{"Differential Mux", "lin1-rin1 with 20db Boost", "MIC1"},
	{"Differential Mux", "lin2-rin2 with 20db Boost", "MIC2"},

	{"D2SE PGA",  NULL, "Differential Mux"},
	{"Line input PGA", NULL, "D2SE PGA"},

	{"Mono ADC", NULL, "Line input PGA"},

	{"Digital Mic Mux", "dmic disable", "Mono ADC"},
	{"Digital Mic Mux", "dmic data at high level", "DMIC"},
	{"Digital Mic Mux", "dmic data at low level", "DMIC"},

	{"I2S OUT", NULL, "Digital Mic Mux"},
	/*
	 * playback route map
	 */
	{"DAC SRC Mux", "LDATA TO LDAC, RDATA TO RDAC", "I2S IN"},
	{"DAC SRC Mux", "LDATA TO LDAC, LDATA TO RDAC", "I2S IN"},
	{"DAC SRC Mux", "RDATA TO LDAC, RDATA TO RDAC", "I2S IN"},
	{"DAC SRC Mux", "RDATA TO LDAC, LDATA TO RDAC", "I2S IN"},

	{"Left Hp mux", "lin1-rin1", "MIC1"},
	{"Left Hp mux", "lin2-rin2", "MIC2"},
	{"Left Hp mux", "lin-rin with Boost", "D2SE PGA"},
	{"Left Hp mux", "lin-rin with Boost and PGA", "Line input PGA"},

	{"Right Hp mux", "lin1-rin1", "MIC1"},
	{"Right Hp mux", "lin2-rin2", "MIC2"},
	{"Right Hp mux", "lin-rin with Boost", "D2SE PGA"},
	{"Right Hp mux", "lin-rin with Boost and PGA", "Line input PGA"},

	{"Left Hp mixer", "LLIN Switch", "Left Hp mux"},
	{"Left Hp mixer", "Left DAC Switch", "DAC SRC Mux"},

	{"Right Hp mixer", "RLIN Switch", "Right Hp mux"},
	{"Right Hp mixer", "Right DAC Switch", "DAC SRC Mux"},

	{"HPCP L", NULL, "Left Hp mixer"},
	{"HPCP R", NULL, "Right Hp mixer"},

	{"HPVOL L", NULL, "HPCP L"},
	{"HPVOL R", NULL, "HPCP R"},

	{"HPOL", NULL, "HPVOL L"},
	{"HPOR", NULL, "HPVOL R"},
};

struct _coeff_div {
	u32 mclk;
	u32 rate;
	u16 fs;
	u8 sr:4;
	u8 usb:1;
};


/* codec hifi mclk clock divider coefficients */
static const struct _coeff_div coeff_div[] = {
	/* 8k */
	{12288000, 8000, 1536, 0xa, 0x0},
	{11289600, 8000, 1408, 0x9, 0x0},
	{18432000, 8000, 2304, 0xc, 0x0},
	{16934400, 8000, 2112, 0xb, 0x0},
	{12000000, 8000, 1500, 0xb, 0x1},

	/* 11.025k */
	{11289600, 11025, 1024, 0x7, 0x0},
	{16934400, 11025, 1536, 0xa, 0x0},
	{12000000, 11025, 1088, 0x9, 0x1},

	/* 16k */
	{12288000, 16000, 768, 0x6, 0x0},
	{18432000, 16000, 1152, 0x8, 0x0},
	{12000000, 16000, 750, 0x7, 0x1},

	/* 22.05k */
	{11289600, 22050, 512, 0x4, 0x0},
	{16934400, 22050, 768, 0x6, 0x0},
	{12000000, 22050, 544, 0x6, 0x1},

	/* 32k */
	{12288000, 32000, 384, 0x3, 0x0},
	{18432000, 32000, 576, 0x5, 0x0},
	{12000000, 32000, 375, 0x4, 0x1},

	/* 44.1k */
	{11289600, 44100, 256, 0x2, 0x0},
	{16934400, 44100, 384, 0x3, 0x0},
	{12000000, 44100, 272, 0x3, 0x1},

	/* 48k */
	{12288000, 48000, 256, 0x2, 0x0},
	{18432000, 48000, 384, 0x3, 0x0},
	{12000000, 48000, 250, 0x2, 0x1},

	/* 88.2k */
	{11289600, 88200, 128, 0x0, 0x0},
	{16934400, 88200, 192, 0x1, 0x0},
	{12000000, 88200, 136, 0x1, 0x1},

	/* 96k */
	{12288000, 96000, 128, 0x0, 0x0},
	{18432000, 96000, 192, 0x1, 0x0},
	{12000000, 96000, 125, 0x0, 0x1},
};

static inline int get_coeff(int mclk, int rate)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(coeff_div); i++) {
		if (coeff_div[i].rate == rate && coeff_div[i].mclk == mclk)
			return i;
	}

	return -EINVAL;
}

/* The set of rates we can generate from the above for each SYSCLK */

static unsigned int rates_12288[] = {
	8000, 12000, 16000, 24000, 24000, 32000, 48000, 96000,
};

static struct snd_pcm_hw_constraint_list constraints_12288 = {
	.count	= ARRAY_SIZE(rates_12288),
	.list	= rates_12288,
};

static unsigned int rates_112896[] = {
	8000, 11025, 22050, 44100,
};

static struct snd_pcm_hw_constraint_list constraints_112896 = {
	.count	= ARRAY_SIZE(rates_112896),
	.list	= rates_112896,
};

static unsigned int rates_12[] = {
	8000, 11025, 12000, 16000, 22050, 24000, 32000, 44100, 48000,
	48000, 88235, 96000,
};

static struct snd_pcm_hw_constraint_list constraints_12 = {
	.count	= ARRAY_SIZE(rates_12),
	.list	= rates_12,
};

/*
 * Note that this should be called from init rather than from hw_params.
 */
static int es8316_set_dai_sysclk(struct snd_soc_dai *codec_dai,
		int clk_id, unsigned int freq, int dir)
{
	struct snd_soc_codec *codec = codec_dai->codec;
	struct es8316_priv *es8316 = snd_soc_codec_get_drvdata(codec);

	DBG("Enter::%s----%d\n", __func__, __LINE__);

	switch (freq) {
	case 11289600:
	case 18432000:
	case 22579200:
	case 36864000:
		es8316->sysclk_constraints = &constraints_112896;
		es8316->sysclk = freq;
		return 0;

	case 12288000:
	case 16934400:
	case 24576000:
	case 33868800:
		es8316->sysclk_constraints = &constraints_12288;
		es8316->sysclk = freq;
		return 0;

	case 12000000:
	case 24000000:
		es8316->sysclk_constraints = &constraints_12;
		es8316->sysclk = freq;
		return 0;
	}
	return -EINVAL;
}

static int es8316_set_dai_fmt(struct snd_soc_dai *codec_dai,
		unsigned int fmt)
{
	struct snd_soc_codec *codec = codec_dai->codec;
	u8 iface = 0;
	u8 adciface = 0;
	u8 daciface = 0;

	alsa_dbg("%s----%d, fmt[%02x]\n", __func__, __LINE__, fmt);

	iface    = snd_soc_read(codec, ES8316_IFACE);
	adciface = snd_soc_read(codec, ES8316_ADC_IFACE);
	daciface = snd_soc_read(codec, ES8316_DAC_IFACE);

	/* set master/slave audio interface */
	switch (fmt & SND_SOC_DAIFMT_MASTER_MASK) {
	case SND_SOC_DAIFMT_CBM_CFM:    /* MASTER MODE */
		alsa_dbg("es8316 in master mode");
		iface |= 0x80;
		break;
	case SND_SOC_DAIFMT_CBS_CFS:	/* SLAVE MODE */
		alsa_dbg("es8316 in slave mode");
		iface &= 0x7F;
		break;
	default:
		return -EINVAL;
	}

	/* interface format */
	switch (fmt & SND_SOC_DAIFMT_FORMAT_MASK) {
	case SND_SOC_DAIFMT_I2S:
		adciface &= 0xFC;
		daciface &= 0xFC;
		break;
	case SND_SOC_DAIFMT_RIGHT_J:
		break;
	case SND_SOC_DAIFMT_LEFT_J:
		break;
	case SND_SOC_DAIFMT_DSP_A:
	break;
	case SND_SOC_DAIFMT_DSP_B:
		break;
	default:
		return -EINVAL;
	}

	/* clock inversion */
	switch (fmt & SND_SOC_DAIFMT_INV_MASK) {
	case SND_SOC_DAIFMT_NB_NF:
		break;
	case SND_SOC_DAIFMT_IB_IF:
		iface    |= 0x20;
		adciface |= 0x20;
		daciface |= 0x20;
		break;
	case SND_SOC_DAIFMT_IB_NF:
		iface    |= 0x20;
		adciface &= 0xDF;
		daciface &= 0xDF;
		break;
	case SND_SOC_DAIFMT_NB_IF:
		iface    &= 0xDF;
		adciface |= 0x20;
		daciface |= 0x20;
		break;
	default:
		return -EINVAL;
	}

	snd_soc_write(codec, ES8316_IFACE, iface);
	snd_soc_write(codec, ES8316_ADC_IFACE, adciface);
	snd_soc_write(codec, ES8316_DAC_IFACE, daciface);

	return 0;
}

static int es8316_pcm_hw_params(struct snd_pcm_substream *substream,
				struct snd_pcm_hw_params *params,
				struct snd_soc_dai *dai)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_codec *codec = rtd->codec;
	struct es8316_priv *es8316 = snd_soc_codec_get_drvdata(codec);
	u16 srate = snd_soc_read(codec, ES8316_IFACE) & 0xff;
	u16 adciface = snd_soc_read(codec, ES8316_ADC_IFACE) & 0x7f;
	u16 daciface = snd_soc_read(codec, ES8316_DAC_IFACE) & 0x7f;
	int coeff;

	coeff = get_coeff(es8316->sysclk, params_rate(params));
	if (coeff < 0) {
		coeff = get_coeff(es8316->sysclk / 2, params_rate(params));
		srate |= 0x40;
	}
	if (coeff < 0) {
		dev_err(codec->dev,
			"Unable to configure sample rate %dHz with %dHz MCLK\n",
			params_rate(params), es8316->sysclk);
		return coeff;
	}

	/* bit size */
	switch (params_format(params)) {
	case SNDRV_PCM_FORMAT_S16_LE:
		adciface |= 0x000C;
		daciface |= 0x000C;
		break;
	case SNDRV_PCM_FORMAT_S20_3LE:
		adciface |= 0x0004;
		daciface |= 0x0004;
		break;
	case SNDRV_PCM_FORMAT_S24_LE:
		break;
	case SNDRV_PCM_FORMAT_S32_LE:
		adciface |= 0x0010;
		daciface |= 0x0010;
		break;
	}

	/* set iface & srate*/
	snd_soc_write(codec, ES8316_DAC_IFACE, daciface);
	snd_soc_write(codec, ES8316_ADC_IFACE, adciface);

	if (coeff >= 0) {
		snd_soc_write(codec, ES8316_IFACE, srate);
		snd_soc_write(codec, ES8316_CLKMGR_ADCDIV2_REG05,
			      coeff_div[coeff].sr |
			      (coeff_div[coeff].usb) << 4);
		snd_soc_write(codec, ES8316_CLKMGR_DACDIV2_REG07,
			      coeff_div[coeff].sr |
			      (coeff_div[coeff].usb) << 4);
	}

	return 0;
}

static int es8316_set_bias_level(struct snd_soc_codec *codec,
				 enum snd_soc_bias_level level)
{
	struct snd_soc_dapm_context *dapm = snd_soc_codec_get_dapm(codec);

	DBG("%s %d\n", __func__, level);

	switch (level) {
	case SND_SOC_BIAS_ON:
		dev_dbg(codec->dev, "%s on\n", __func__);
		break;
	case SND_SOC_BIAS_PREPARE:
		dev_dbg(codec->dev, "%s prepare\n", __func__);
		snd_soc_write(codec, ES8316_CLKMGR_CLKSW_REG01, 0x7F);
		snd_soc_write(codec, ES8316_SYS_PDN_REG0D, 0x00);
		snd_soc_write(codec, ES8316_ADC_PDN_LINSEL_REG22, 0x20);
		if (es8316_jack_insert) {
			/* VPP SET AND ZERO L/R,STEREO SET */
			snd_soc_write(codec, ES8316_DAC_SET3_REG32, 0x00);
		} else {
			/* VPP SET AND ZERO L/R,MONO SET */
			snd_soc_write(codec, ES8316_DAC_SET3_REG32, 0x08);
		}
		snd_soc_write(codec, ES8316_DAC_PDN_REG2F, 0x00);
		snd_soc_write(codec, ES8316_HPMIX_SWITCH_REG14, 0x88);
		snd_soc_write(codec, ES8316_HPMIX_PDN_REG15, 0x88);
		snd_soc_write(codec, ES8316_HPMIX_VOL_REG16, 0xBB);
		snd_soc_write(codec, ES8316_CPHP_PDN2_REG1A, 0x10);
		snd_soc_write(codec, ES8316_CPHP_LDOCTL_REG1B, 0x30);
		snd_soc_write(codec, ES8316_CPHP_PDN1_REG19, 0x03);
		snd_soc_write(codec, ES8316_CPHP_ICAL_VOL_REG18, 0x00);
		snd_soc_write(codec, ES8316_RESET_REG00, 0xC0);
		if (es8316_jack_insert) {
			/* L/R OUTEN */
			snd_soc_write(codec, ES8316_CPHP_OUTEN_REG17, 0x66);
		} else {
			/* R OUTEN */
			snd_soc_write(codec, ES8316_CPHP_OUTEN_REG17, 0x06);
			es8316_spk_on(1);
		}
		DBG("%s: jack_insert %d\n", __func__, es8316_jack_insert);
		break;
	case SND_SOC_BIAS_STANDBY:
		dev_dbg(codec->dev, "%s standby\n", __func__);
		snd_soc_write(codec, ES8316_CLKMGR_CLKSW_REG01, 0x7F);
		snd_soc_write(codec, ES8316_SYS_PDN_REG0D, 0x00);
		snd_soc_write(codec, ES8316_ADC_PDN_LINSEL_REG22, 0x20);
		if (es8316_jack_insert) {
			/* VPP SET AND ZERO L/R,STEREO SET */
			snd_soc_write(codec, ES8316_DAC_SET3_REG32, 0x00);
		} else {
			/* VPP SET AND ZERO L/R,MONO SET */
			snd_soc_write(codec, ES8316_DAC_SET3_REG32, 0x08);
		}
		snd_soc_write(codec, ES8316_DAC_PDN_REG2F, 0x00);
		snd_soc_write(codec, ES8316_HPMIX_SWITCH_REG14, 0x88);
		snd_soc_write(codec, ES8316_HPMIX_PDN_REG15, 0x88);
		snd_soc_write(codec, ES8316_HPMIX_VOL_REG16, 0xBB);
		snd_soc_write(codec, ES8316_CPHP_PDN2_REG1A, 0x10);
		snd_soc_write(codec, ES8316_CPHP_LDOCTL_REG1B, 0x30);
		snd_soc_write(codec, ES8316_CPHP_PDN1_REG19, 0x03);
		snd_soc_write(codec, ES8316_CPHP_ICAL_VOL_REG18, 0x00);
		snd_soc_write(codec, ES8316_RESET_REG00, 0xC0);
		if (es8316_jack_insert) {
			/* L/R OUTEN */
			snd_soc_write(codec, ES8316_CPHP_OUTEN_REG17, 0x66);
		} else {
			es8316_spk_on(0);
			/* R OUTEN */
			snd_soc_write(codec, ES8316_CPHP_OUTEN_REG17, 0x06);
		}
		DBG("%s: jack_insert %d\n", __func__, es8316_jack_insert);
		break;
	case SND_SOC_BIAS_OFF:
		dev_dbg(codec->dev, "%s off\n", __func__);
		snd_soc_write(codec, ES8316_CPHP_ICAL_VOL_REG18, 0x33);
		snd_soc_write(codec, ES8316_CPHP_OUTEN_REG17, 0x00);
		snd_soc_write(codec, ES8316_CPHP_LDOCTL_REG1B, 0x03);
		snd_soc_write(codec, ES8316_CPHP_PDN2_REG1A, 0x22);
		snd_soc_write(codec, ES8316_CPHP_PDN1_REG19, 0x06);
		snd_soc_write(codec, ES8316_HPMIX_SWITCH_REG14, 0x00);
		snd_soc_write(codec, ES8316_HPMIX_PDN_REG15, 0x33);
		snd_soc_write(codec, ES8316_HPMIX_VOL_REG16, 0x00);
		snd_soc_write(codec, ES8316_ADC_PDN_LINSEL_REG22, 0xC0);
		snd_soc_write(codec, ES8316_DAC_PDN_REG2F, 0x11);
		snd_soc_write(codec, ES8316_SYS_PDN_REG0D, 0x3F);
		snd_soc_write(codec, ES8316_CLKMGR_CLKSW_REG01, 0x03);
		snd_soc_write(codec, ES8316_RESET_REG00, 0x7F);
		break;
	}
	dapm->bias_level = level;

	return 0;
}

#define es8316_RATES SNDRV_PCM_RATE_8000_96000

#define es8316_FORMATS (SNDRV_PCM_FMTBIT_S16_LE | SNDRV_PCM_FMTBIT_S20_3LE |\
	SNDRV_PCM_FMTBIT_S24_LE)

static struct snd_soc_dai_ops es8316_ops = {
	.hw_params = es8316_pcm_hw_params,
	.set_fmt = es8316_set_dai_fmt,
	.set_sysclk = es8316_set_dai_sysclk,
};

static struct snd_soc_dai_driver es8316_dai = {
	.name = "ES8316 HiFi",
	.playback = {
		.stream_name = "Playback",
		.channels_min = 1,
		.channels_max = 2,
		.rates = es8316_RATES,
		.formats = es8316_FORMATS,
	},
	.capture = {
		.stream_name = "Capture",
		.channels_min = 1,
		.channels_max = 2,
		.rates = es8316_RATES,
		.formats = es8316_FORMATS,
	 },
	.ops = &es8316_ops,
	.symmetric_rates = 1,
};


static int es8316_init_regs(struct snd_soc_codec *codec)
{
	dev_dbg(codec->dev, "%s\n", __func__);


	snd_soc_write(codec, ES8316_RESET_REG00, 0x3F);  /* RESET */
	msleep(50);
	snd_soc_write(codec, ES8316_RESET_REG00, 0x00);
	/* INPUT SELECT INPUT1:0X20,INPUT2:0X30 */
	snd_soc_write(codec, ES8316_ADC_PDN_LINSEL_REG22, 0x20);
	/* DC MEASURE DISABLE, 10db disable */
	snd_soc_write(codec, ES8316_ADC_D2SEPGA_REG24, 0x00);
	/* ADC VOL=0DB, MIN=0XC0(-96DB),STEP=0.5DB */
	snd_soc_write(codec, ES8316_ADC_VOLUME_REG27, 0x00);
	/* DAC double speed,auto mute disable */
	snd_soc_write(codec, ES8316_DAC_SET2_REG31, 0x00);
	/* VPP SET AND ZERO L/R, STEREO SET */
	snd_soc_write(codec, ES8316_DAC_SET3_REG32, 0x00);
	/* LDAC VOL=0DB, MIN=0XC0(-96DB),STEP=0.5DB */
	snd_soc_write(codec, ES8316_DAC_VOLL_REG33, 0x09);
	/* RDAC VOL=0DB, MIN=0XC0(-96DB),STEP=0.5DB */
	snd_soc_write(codec, ES8316_DAC_VOLR_REG34, 0x09);
	/* ADC FORMAT */
	snd_soc_write(codec, ES8316_SDP_ADCFMT_REG0A, 0x0C);
	/* DAC FORMAT */
	snd_soc_write(codec, ES8316_SDP_DACFMT_REG0B, 0x0C);
	/* CODEC IN SLAVE, */
	snd_soc_write(codec, ES8316_SDP_MS_BCKDIV_REG09, 0x04);
	/* CLK CASE */
	snd_soc_write(codec, ES8316_CLKMGR_CLKSEL_REG02, 0x09);
	/* ADC SCLK/LRCLK=64FS */
	snd_soc_write(codec, ES8316_CLKMGR_ADCOSR_REG03, 0x20);
	/* ADC MCLK RATIO/1=256 */
	snd_soc_write(codec, ES8316_CLKMGR_ADCDIV1_REG04, 0x11);
	/* ADC LRCLK=MCLK/256 */
	snd_soc_write(codec, ES8316_CLKMGR_ADCDIV2_REG05, 0x02);
	/* DAC MCLK RATIO/1=256 */
	snd_soc_write(codec, ES8316_CLKMGR_DACDIV1_REG06, 0x11);
	/* DAC LRCLK=MCLK/256 */
	snd_soc_write(codec, ES8316_CLKMGR_DACDIV2_REG07, 0x02);
	/* CHARGE PUMP DIVIDER */
	snd_soc_write(codec, ES8316_CLKMGR_CPDIV_REG08, 0x00);
	/* CLK ON */
	snd_soc_write(codec, ES8316_CLKMGR_CLKSW_REG01, 0x7F);
	snd_soc_write(codec, ES8316_SYS_VMIDLOW_REG10, 0x11);
	snd_soc_write(codec, ES8316_SYS_REF_REG12, 0x28);
	snd_soc_write(codec, ES8316_SYS_LP1_REG0E, 0x04);
	snd_soc_write(codec, ES8316_SYS_LP2_REG0F, 0x00);
	/* VMID SEQUENCE */
	snd_soc_write(codec, ES8316_SYS_VMIDSEL_REG0C, 0xFF);
	/* POWER UP DACPOWER */
	snd_soc_write(codec, ES8316_DAC_PDN_REG2F, 0x00);
	/* L,R MIXER SET */
	snd_soc_write(codec, ES8316_HPMIX_SEL_REG13, 0x00);
	/* MIXER */
	snd_soc_write(codec, ES8316_HPMIX_SWITCH_REG14, 0x88);
	/* MIXER */
	snd_soc_write(codec, ES8316_HPMIX_PDN_REG15, 0x88);
	/* MIXER */
	snd_soc_write(codec, ES8316_HPMIX_VOL_REG16, 0xBB);
	/* CHARGE PUMP */
	snd_soc_write(codec, ES8316_CPHP_PDN2_REG1A, 0x10);
	/* CHARGE PUMP */
	snd_soc_write(codec, ES8316_CPHP_LDOCTL_REG1B, 0x00);
	/* CHARGE PUMP */
	snd_soc_write(codec, ES8316_CPHP_PDN1_REG19, 0x03);
	/* CHARGE PUMP */
	snd_soc_write(codec, ES8316_CPHP_ICAL_VOL_REG18, 0x00);
	/* CHARGE PUMP */
	snd_soc_write(codec, ES8316_CPHP_OUTEN_REG17, 0x33);
	/* CSM POWER UP */
	snd_soc_write(codec, ES8316_RESET_REG00, 0xC0);
	msleep(50);
	/* HPOUT L/R OUTEN */
	snd_soc_write(codec, ES8316_CPHP_OUTEN_REG17, 0x66);
	snd_soc_write(codec, ES8316_GPIO_SEL_REG4D, 0x00);
	snd_soc_write(codec, ES8316_GPIO_DEBUNCE_INT_REG4E, 0x02);
	snd_soc_write(codec, ES8316_TESTMODE_REG50, 0xA0);
	snd_soc_write(codec, ES8316_TEST2_REG52, 0x03);
	/* ADC,DAC VREF SET */
	snd_soc_write(codec, ES8316_SYS_PDN_REG0D, 0x00);
	/*alc set*/
	snd_soc_write(codec, ES8316_ADC_PGAGAIN_REG23, 0xa0);
	snd_soc_write(codec, ES8316_ADC_D2SEPGA_REG24, 0x00);
	/* ALC ON */
	snd_soc_write(codec, ES8316_ADC_ALC1_REG29, 0xD4);
	snd_soc_write(codec, ES8316_ADC_ALC2_REG2A, 0x08);
	snd_soc_write(codec, ES8316_ADC_ALC3_REG2B, 0x90);
	snd_soc_write(codec, ES8316_ADC_ALC4_REG2C, 0x05);
	snd_soc_write(codec, ES8316_ADC_ALC5_REG2D, 0x06);
	/* NOISE GATE ENABLE, GATE=-75db */
	snd_soc_write(codec, ES8316_ADC_ALC6_REG2E, 0x61);
	return 0;
}

#ifdef CONFIG_PM
static int es8316_suspend(struct snd_soc_codec *codec)
{
	DBG("###########%s %d ", __func__, __LINE__);
	snd_soc_write(codec, ES8316_CPHP_ICAL_VOL_REG18, 0x33);
	snd_soc_write(codec, ES8316_CPHP_OUTEN_REG17, 0x00);
	snd_soc_write(codec, ES8316_CPHP_LDOCTL_REG1B, 0x03);
	snd_soc_write(codec, ES8316_CPHP_PDN2_REG1A, 0x22);
	snd_soc_write(codec, ES8316_CPHP_PDN1_REG19, 0x06);
	snd_soc_write(codec, ES8316_HPMIX_SWITCH_REG14, 0x00);
	snd_soc_write(codec, ES8316_HPMIX_PDN_REG15, 0x33);
	snd_soc_write(codec, ES8316_HPMIX_VOL_REG16, 0x00);
	snd_soc_write(codec, ES8316_ADC_PDN_LINSEL_REG22, 0xC0);
	snd_soc_write(codec, ES8316_CLKMGR_CLKSW_REG01, 0x03);
	es8316_set_bias_level(codec, SND_SOC_BIAS_OFF);
	return 0;
}

static int es8316_resume(struct snd_soc_codec *codec)
{
	DBG("###########%s %d ", __func__, __LINE__);

	es8316_init_regs(codec);
	es8316_set_bias_level(codec, SND_SOC_BIAS_STANDBY);
	return 0;
}
#else
#define es8316_suspend NULL
#define es8316_resume NULL
#endif

static int es8316_probe(struct snd_soc_codec *codec)
{
	int ret = 0;
	struct snd_soc_dapm_context *dapm = snd_soc_codec_get_dapm(codec);

#if defined(CONFIG_SND_NX_SIMPLE_CARD) && \
	(defined(CONFIG_MACH_NANOPI3) || defined(CONFIG_MACH_NANOPI2))
	es8316_jack_insert = 1;	/* Stereo out */
#endif

	DBG("---%s--start--\n", __func__);
	codec->hw_write = (hw_write_t)i2c_master_send;
	codec->control_data = container_of(codec->dev, struct i2c_client, dev);
	es8316_codec = codec;

	ret = es8316_reset(codec);
	if (ret < 0) {
		dev_err(codec->dev, "fail to reset audio (%d)\n", ret);
		goto err;
	}

	es8316_init_regs(codec);
	es8316_set_bias_level(codec, SND_SOC_BIAS_STANDBY);

	snd_soc_add_codec_controls(codec, es8316_snd_controls,
				ARRAY_SIZE(es8316_snd_controls));
	snd_soc_dapm_new_controls(dapm, es8316_dapm_widgets,
				  ARRAY_SIZE(es8316_dapm_widgets));
	snd_soc_dapm_add_routes(dapm, es8316_dapm_routes,
			ARRAY_SIZE(es8316_dapm_routes));
#if defined(HS_IRQ)
	det_initialize();
#elif defined(HS_TIMER)
	hsdet_init();
#endif
	DBG("---%s--ok--\n", __func__);
err:
	DBG("###########%s %d ", __func__, __LINE__);
	return ret;
}

static int es8316_remove(struct snd_soc_codec *codec)
{
	es8316_set_bias_level(codec, SND_SOC_BIAS_OFF);
	return 0;
}

static struct snd_soc_codec_driver soc_codec_dev_es8316 = {
	.read  = es8316_read_reg_cache,
	.write = es8316_write,
	.probe = es8316_probe,
	.remove = es8316_remove,
	.suspend = es8316_suspend,
	.resume = es8316_resume,
	.set_bias_level = es8316_set_bias_level,
	.reg_cache_size = ARRAY_SIZE(es8316_reg),
	.reg_word_size = sizeof(u16),
};

#if defined(CONFIG_SPI_MASTER)
static int es8316_spi_probe(struct spi_device *spi)
{
	struct es8316_priv *es8316;
	int ret;

	es8316 = kzalloc(sizeof(struct es8316_priv), GFP_KERNEL);
	if (es8316 == NULL)
		return -ENOMEM;

	spi_set_drvdata(spi, es8316);

	ret = snd_soc_register_codec(&spi->dev,
			&soc_codec_dev_es8316, &es8316_dai, 1);
	if (ret < 0)
		kfree(es8316);
	return ret;
}

static int es8316_spi_remove(struct spi_device *spi)
{
	snd_soc_unregister_codec(&spi->dev);
	kfree(spi_get_drvdata(spi));
	return 0;
}

static struct spi_driver es8316_spi_driver = {
	.driver = {
		.name	= "es8316",
		.owner	= THIS_MODULE,
	},
	.probe		= es8316_spi_probe,
	.remove		= es8316_spi_remove,
};
#endif /* CONFIG_SPI_MASTER */

#if defined(CONFIG_I2C) || defined(CONFIG_I2C_MODULE)

static void es8316_i2c_shutdown(struct i2c_client *i2c)
{
	struct snd_soc_codec *codec;

	if (!es8316_codec)
		goto err;
	es8316_spk_on(0);
	codec = es8316_codec;
	snd_soc_write(codec, ES8316_CPHP_ICAL_VOL_REG18, 0x33);
	snd_soc_write(codec, ES8316_CPHP_OUTEN_REG17, 0x00);
	snd_soc_write(codec, ES8316_CPHP_LDOCTL_REG1B, 0x03);
	snd_soc_write(codec, ES8316_CPHP_PDN2_REG1A, 0x22);
	snd_soc_write(codec, ES8316_CPHP_PDN1_REG19, 0x06);
	snd_soc_write(codec, ES8316_HPMIX_SWITCH_REG14, 0x00);
	snd_soc_write(codec, ES8316_HPMIX_PDN_REG15, 0x33);
	snd_soc_write(codec, ES8316_HPMIX_VOL_REG16, 0x00);
	snd_soc_write(codec, ES8316_ADC_PDN_LINSEL_REG22, 0xC0);
	snd_soc_write(codec, ES8316_DAC_PDN_REG2F, 0x11);
	snd_soc_write(codec, ES8316_SYS_PDN_REG0D, 0x3F);
	snd_soc_write(codec, ES8316_CLKMGR_CLKSW_REG01, 0x03);
	snd_soc_write(codec, ES8316_RESET_REG00, 0x7F);
err:
	return;
}
static u32 cur_reg;

static ssize_t es8316_show(struct device *dev, struct device_attribute *attr,
			   char *_buf)
{
	int ret;

	ret = sprintf(_buf, "%s(): get 0x%04x=0x%04x\n", __func__,
		      cur_reg, snd_soc_read(es8316_codec, cur_reg));
	return ret;
}

static u32 strtol(const char *nptr, int base)
{
	u32 ret;

	if (!nptr || (base != 16 && base != 10 && base != 8)) {
		pr_err("%s(): NULL pointer input\n", __func__);
		return -1;
	}
	for (ret = 0; *nptr; nptr++) {
		if ((base == 16 && *nptr >= 'A' && *nptr <= 'F') ||
			(base == 16 && *nptr >= 'a' && *nptr <= 'f') ||
			(base >= 10 && *nptr >= '0' && *nptr <= '9') ||
			(base >= 8 && *nptr >= '0' && *nptr <= '7')) {
			ret *= base;
			if (base == 16 && *nptr >= 'A' && *nptr <= 'F')
				ret += *nptr-'A'+10;
			else if (base == 16 && *nptr >= 'a' && *nptr <= 'f')
				ret += *nptr-'a'+10;
			else if (base >= 10 && *nptr >= '0' && *nptr <= '9')
				ret += *nptr-'0';
			else if (base >= 8 && *nptr >= '0' && *nptr <= '7')
				ret += *nptr-'0';
		} else
			return ret;
	}
	return ret;
}

static ssize_t es8316_store(struct device *dev,
					struct device_attribute *attr,
					const char *_buf, size_t _count)
{
	const char *p = _buf;
	u32 reg, val;

	if (!strncmp(_buf, "get", strlen("get"))) {
		p += strlen("get");
		cur_reg = (u32)strtol(p, 16);
		val = snd_soc_read(es8316_codec, cur_reg);
		pr_info("%s(): get 0x%04x=0x%04x\n", __func__, cur_reg, val);
	} else if (!strncmp(_buf, "put", strlen("put"))) {
		p += strlen("put");
		reg = strtol(p, 16);
		p = strchr(_buf, '=');
		if (p) {
			++p;
			val = strtol(p, 16);
			snd_soc_write(es8316_codec, reg, val);
			pr_info("%s(): set 0x%04x=0x%04x\n", __func__, reg,
			       val);
		} else
			pr_err("%s(): Bad string format input!\n", __func__);
	} else
		pr_err("%s(): Bad string format input!\n", __func__);

	return _count;
}

static struct device *es8316_dev;
static struct class *es8316_class;
static DEVICE_ATTR(es8316, 0664, es8316_show, es8316_store);
static int es8316_i2c_probe(struct i2c_client *i2c,
				      const struct i2c_device_id *id)
{
	struct es8316_priv *es8316;
	struct i2c_adapter *adapter = to_i2c_adapter(i2c->dev.parent);
	int ret = -1;

	DBG("---%s---probe start\n", __func__);

	if (!i2c_check_functionality(adapter, I2C_FUNC_I2C)) {
		dev_warn(&adapter->dev,
		"I2C-Adapter doesn't support\n");
		return -EIO;
	}

	es8316 = kzalloc(sizeof(struct es8316_priv), GFP_KERNEL);
	if (es8316 == NULL)
		return -ENOMEM;

	i2c_set_clientdata(i2c, es8316);
	ret =  snd_soc_register_codec(&i2c->dev, &soc_codec_dev_es8316,
	     &es8316_dai, 1);
	if (ret < 0) {
		kfree(es8316);
		return ret;
	}

	es8316_class = class_create(THIS_MODULE, "es8316");
	if (IS_ERR(es8316_class)) {
		pr_err("Failed, Cannot create class audio_es8316.\n");
		return -ENOMEM;
	}
	es8316_dev = device_create(es8316_class, NULL, MKDEV(0, 1), NULL,
				   "dev");
	ret = device_create_file(es8316_dev, &dev_attr_es8316);
	if (ret < 0)
		pr_err("failed to add dev_attr_es8316 file\n");
	DBG("---%s---probe ok\n", __func__);
	return ret;
}

static  int es8316_i2c_remove(struct i2c_client *client)
{
	snd_soc_unregister_codec(&client->dev);
	kfree(i2c_get_clientdata(client));
	return 0;
}

#ifdef CONFIG_OF
static const struct of_device_id es8316_of_match[] = {
	{ .compatible = "everest,es8316" },
	{},
};
MODULE_DEVICE_TABLE(of, es8316_of_match);
#else
#define es8316_of_match NULL
#endif

static const struct i2c_device_id es8316_i2c_id[] = {
	{ "es8316", 0 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, es8316_i2c_id);

static struct i2c_driver es8316_i2c_driver = {
	.driver = {
		.name = "ES8316",
		.owner = THIS_MODULE,
		.of_match_table = es8316_of_match,
	},
	.shutdown = es8316_i2c_shutdown,
	.probe = es8316_i2c_probe,
	.remove = es8316_i2c_remove,
	.id_table = es8316_i2c_id,
};
#endif

static int __init es8316_init(void)
{
	DBG("--%s--start--\n", __func__);
#if defined(CONFIG_I2C) || defined(CONFIG_I2C_MODULE)
	return i2c_add_driver(&es8316_i2c_driver);
#endif
#if defined(CONFIG_SPI_MASTER)
	return spi_register_driver(&es8316_spi_driver);
#endif
}

static void __exit es8316_exit(void)
{
#if defined(CONFIG_I2C) || defined(CONFIG_I2C_MODULE)
	return i2c_del_driver(&es8316_i2c_driver);
#endif
#if defined(CONFIG_SPI_MASTER)
	return spi_unregister_driver(&es8316_spi_driver);
#endif
}

module_init(es8316_init);
module_exit(es8316_exit);

MODULE_DESCRIPTION("ASoC es8316 driver");
MODULE_AUTHOR("Mark Brown <will@everset-semi.com>");
MODULE_LICENSE("GPL");
