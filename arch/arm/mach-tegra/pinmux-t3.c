/*
 * linux/arch/arm/mach-tegra/pinmux-t3-tables.c
 *
 * Common pinmux configurations for Tegra 3 SoCs
 *
 * Copyright (C) 2010-2011 NVIDIA Corporation
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 */

#include <linux/init.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/of.h>
#include <linux/spinlock.h>
#include <linux/syscore_ops.h>
#include <linux/io.h>

#undef CONFIG_ARCH_TEGRA_2x_SOC

#include "gpio-names.h"
#include "iomap.h"

#include "pinmux-t3.h"
#include "pinmux.h"

#define SET_DRIVE_PINGROUP(pg_name, r, drv_down_offset, drv_down_mask, drv_up_offset, drv_up_mask,	\
	slew_rise_offset, slew_rise_mask, slew_fall_offset, slew_fall_mask)	\
	[TEGRA_DRIVE_PINGROUP_ ## pg_name] = {			\
		.name = #pg_name,				\
		.reg = r,					\
		.drvup_offset = drv_up_offset,			\
		.drvup_mask = drv_up_mask,			\
		.drvdown_offset = drv_down_offset,		\
		.drvdown_mask = drv_down_mask,			\
		.slewrise_offset = slew_rise_offset,		\
		.slewrise_mask = slew_rise_mask,		\
		.slewfall_offset = slew_fall_offset,		\
		.slewfall_mask = slew_fall_mask,		\
	}

#define DEFAULT_DRIVE_PINGROUP(pg_name, r)		\
	[TEGRA_DRIVE_PINGROUP_ ## pg_name] = {		\
		.name = #pg_name,			\
		.reg = r,				\
		.drvup_offset = 20,			\
		.drvup_mask = 0x1f,			\
		.drvdown_offset = 12,			\
		.drvdown_mask = 0x1f,			\
		.slewrise_offset = 28,			\
		.slewrise_mask = 0x3,			\
		.slewfall_offset = 30,			\
		.slewfall_mask = 0x3,			\
	}

static const struct tegra_drive_pingroup_desc tegra_soc_drive_pingroups[TEGRA_MAX_DRIVE_PINGROUP] = {
	DEFAULT_DRIVE_PINGROUP(AO1,		0x868),
	DEFAULT_DRIVE_PINGROUP(AO2,		0x86c),
	SET_DRIVE_PINGROUP(AT1,		0x870,	14,	0x1f,	19,	0x1f,
	24,	0x3,	28,	0x3),
	SET_DRIVE_PINGROUP(AT2,		0x874,	14,	0x1f,	19,	0x1f,
	24,	0x3,	28,	0x3),
	SET_DRIVE_PINGROUP(AT3,		0x878,	14,	0x1f,	19,	0x1f,
	28,	0x3,	30,	0x3),
	SET_DRIVE_PINGROUP(AT4,		0x87c,	14,	0x1f,	19,	0x1f,
	28,	0x3,	30,	0x3),
	SET_DRIVE_PINGROUP(AT5,		0x880,	14,	0x1f,	19,	0x1f,
	28,	0x3,	30,	0x3),
	DEFAULT_DRIVE_PINGROUP(CDEV1,		0x884),
	DEFAULT_DRIVE_PINGROUP(CDEV2,		0x888),
	DEFAULT_DRIVE_PINGROUP(CSUS,		0x88c),
	DEFAULT_DRIVE_PINGROUP(DAP1,		0x890),
	DEFAULT_DRIVE_PINGROUP(DAP2,		0x894),
	DEFAULT_DRIVE_PINGROUP(DAP3,		0x898),
	DEFAULT_DRIVE_PINGROUP(DAP4,		0x89c),
	DEFAULT_DRIVE_PINGROUP(DBG,		0x8a0),
	DEFAULT_DRIVE_PINGROUP(LCD1,		0x8a4),
	DEFAULT_DRIVE_PINGROUP(LCD2,		0x8a8),
	SET_DRIVE_PINGROUP(SDIO2,		0x8ac,	12,	0x7f,	20,	0x7f,
		28,	0x3,	30,	0x3),
	SET_DRIVE_PINGROUP(SDIO3,		0x8b0,	12,	0x7f,	20,	0x7f,
		28,	0x3,	30,	0x3),
	DEFAULT_DRIVE_PINGROUP(SPI,		0x8b4),
	DEFAULT_DRIVE_PINGROUP(UAA,		0x8b8),
	DEFAULT_DRIVE_PINGROUP(UAB,		0x8bc),
	DEFAULT_DRIVE_PINGROUP(UART2,		0x8c0),
	DEFAULT_DRIVE_PINGROUP(UART3,		0x8c4),
	DEFAULT_DRIVE_PINGROUP(VI1,		0x8c8),
	SET_DRIVE_PINGROUP(SDIO1,		0x8ec,	12,	0x7f,	20,	0x7f,
		28,	0x3,	30,	0x3),
	DEFAULT_DRIVE_PINGROUP(CRT,		0x8f8),
	DEFAULT_DRIVE_PINGROUP(DDC,		0x8fc),
	SET_DRIVE_PINGROUP(GMA,			0x900,	14,	0x1f,	19,	0x1f,
		24,	0xf,	28,	0xf),
	SET_DRIVE_PINGROUP(GMB,			0x904,	14,	0x1f,	19,	0x1f,
		24,	0xf,	28,	0xf),
	SET_DRIVE_PINGROUP(GMC,			0x908,	14,	0x1f,	19,	0x1f,
		24,	0xf,	28,	0xf),
	SET_DRIVE_PINGROUP(GMD,			0x90c,	14,	0x1f,	19,	0x1f,
		24,	0xf,	28,	0xf),
	SET_DRIVE_PINGROUP(GME,		0x910,	14,	0x1f,	19,	0x1f,
	28,	0x3,	30,	0x3),
	SET_DRIVE_PINGROUP(GMF,		0x914,	14,	0x1f,	19,	0x1f,
	28,	0x3,	30,	0x3),
	SET_DRIVE_PINGROUP(GMG,		0x918,	14,	0x1f,	19,	0x1f,
	28,	0x3,	30,	0x3),
	SET_DRIVE_PINGROUP(GMH,		0x91c,	14,	0x1f,	19,	0x1f,
	28,	0x3,	30,	0x3),
	DEFAULT_DRIVE_PINGROUP(OWR,		0x920),
	DEFAULT_DRIVE_PINGROUP(UAD,		0x924),
	DEFAULT_DRIVE_PINGROUP(GPV,		0x928),
	DEFAULT_DRIVE_PINGROUP(DEV3,		0x92c),
	DEFAULT_DRIVE_PINGROUP(CEC,		0x938),
};

#define PINGROUP(pg_name, gpio_nr, vdd, f0, f1, f2, f3, fs, iod, reg)	\
	[TEGRA_PINGROUP_ ## pg_name] = {			\
		.name = #pg_name,				\
		.vddio = TEGRA_VDDIO_ ## vdd,			\
		.funcs = {					\
			TEGRA_MUX_ ## f0,			\
			TEGRA_MUX_ ## f1,			\
			TEGRA_MUX_ ## f2,			\
			TEGRA_MUX_ ## f3,			\
		},						\
		.gpionr = TEGRA_GPIO_ ## gpio_nr,		\
		.func_safe = TEGRA_MUX_ ## fs,			\
		.tri_reg = reg,					\
		.tri_bit = 4,					\
		.mux_reg = reg,					\
		.mux_bit = 0,					\
		.pupd_reg = reg,				\
		.pupd_bit = 2,					\
		.io_default = TEGRA_PIN_ ## iod,		\
		.od_bit = 6,					\
		.lock_bit = 7,					\
		.ioreset_bit = 8,				\
	}

/* !!!FIXME!!! FILL IN fSafe COLUMN IN TABLE ....... */

#define PINGROUPS	\
	/*	 NAME			GPIO		VDD		f0		f1		f2		f3		fSafe	io	reg */\
	PINGROUP(ULPI_DATA0,		PO1,		BB,		SPI3,		HSI,		UARTA,		ULPI,		RSVD,	INPUT,	0x3000),\
	PINGROUP(ULPI_DATA1,		PO2,		BB,		SPI3,		HSI,		UARTA,		ULPI,		RSVD,	INPUT,	0x3004),\
	PINGROUP(ULPI_DATA2,		PO3,		BB,		SPI3,		HSI,		UARTA,		ULPI,		RSVD,	INPUT,	0x3008),\
	PINGROUP(ULPI_DATA3,		PO4,		BB,		SPI3,		HSI,		UARTA,		ULPI,		RSVD,	INPUT,	0x300c),\
	PINGROUP(ULPI_DATA4,		PO5,		BB,		SPI2,		HSI,		UARTA,		ULPI,		RSVD,	INPUT,	0x3010),\
	PINGROUP(ULPI_DATA5,		PO6,		BB,		SPI2,		HSI,		UARTA,		ULPI,		RSVD,	INPUT,	0x3014),\
	PINGROUP(ULPI_DATA6,		PO7,		BB,		SPI2,		HSI,		UARTA,		ULPI,		RSVD,	INPUT,	0x3018),\
	PINGROUP(ULPI_DATA7,		PO0,		BB,		SPI2,		HSI,		UARTA,		ULPI,		RSVD,	INPUT,	0x301c),\
	PINGROUP(ULPI_CLK,		PY0,		BB,		SPI1,		RSVD,		UARTD,		ULPI,		RSVD,	INPUT,	0x3020),\
	PINGROUP(ULPI_DIR,		PY1,		BB,		SPI1,		RSVD,		UARTD,		ULPI,		RSVD,	INPUT,	0x3024),\
	PINGROUP(ULPI_NXT,		PY2,		BB,		SPI1,		RSVD,		UARTD,		ULPI,		RSVD,	INPUT,	0x3028),\
	PINGROUP(ULPI_STP,		PY3,		BB,		SPI1,		RSVD,		UARTD,		ULPI,		RSVD,	INPUT,	0x302c),\
	PINGROUP(DAP3_FS,		PP0,		BB,		I2S2,		RSVD1,		DISPLAYA,	DISPLAYB,	RSVD,	INPUT,	0x3030),\
	PINGROUP(DAP3_DIN,		PP1,		BB,		I2S2,		RSVD1,		DISPLAYA,	DISPLAYB,	RSVD,	INPUT,	0x3034),\
	PINGROUP(DAP3_DOUT,		PP2,		BB,		I2S2,		RSVD1,		DISPLAYA,	DISPLAYB,	RSVD,	INPUT,	0x3038),\
	PINGROUP(DAP3_SCLK,		PP3,		BB,		I2S2,		RSVD1,		DISPLAYA,	DISPLAYB,	RSVD,	INPUT,	0x303c),\
	PINGROUP(GPIO_PV0,		PV0,		BB,		RSVD,		RSVD,		RSVD,		RSVD,		RSVD,	INPUT,	0x3040),\
	PINGROUP(GPIO_PV1,		PV1,		BB,		RSVD,		RSVD,		RSVD,		RSVD,		RSVD,	INPUT,	0x3044),\
	PINGROUP(SDMMC1_CLK,		PZ0,		SDMMC1,		SDMMC1,		RSVD1,		RSVD2,		INVALID,	RSVD,	INPUT,	0x3048),\
	PINGROUP(SDMMC1_CMD,		PZ1,		SDMMC1,		SDMMC1,		RSVD1,		RSVD2,		INVALID,	RSVD,	INPUT,	0x304c),\
	PINGROUP(SDMMC1_DAT3,		PY4,		SDMMC1,		SDMMC1,		RSVD1,		UARTE,		INVALID,	RSVD,	INPUT,	0x3050),\
	PINGROUP(SDMMC1_DAT2,		PY5,		SDMMC1,		SDMMC1,		RSVD1,		UARTE,		INVALID,	RSVD,	INPUT,	0x3054),\
	PINGROUP(SDMMC1_DAT1,		PY6,		SDMMC1,		SDMMC1,		RSVD1,		UARTE,		INVALID,	RSVD,	INPUT,	0x3058),\
	PINGROUP(SDMMC1_DAT0,		PY7,		SDMMC1,		SDMMC1,		RSVD1,		UARTE,		INVALID,	RSVD,	INPUT,	0x305c),\
	PINGROUP(GPIO_PV2,		PV2,		SDMMC1,		OWR,		RSVD1,		RSVD2,		RSVD3,		RSVD,	INPUT,	0x3060),\
	PINGROUP(GPIO_PV3,		PV3,		SDMMC1,		INVALID,	RSVD1,		RSVD2,		RSVD3,		RSVD,	INPUT,	0x3064),\
	PINGROUP(CLK2_OUT,		PW5,		SDMMC1,		EXTPERIPH2,	RSVD1,		RSVD2,		RSVD3,		RSVD,	INPUT,	0x3068),\
	PINGROUP(CLK2_REQ,		PCC5,		SDMMC1,		DAP,		RSVD1,		RSVD2,		RSVD3,		RSVD,	INPUT,	0x306c),\
	PINGROUP(LCD_PWR1,		PC1,		LCD,		DISPLAYA,	DISPLAYB,	RSVD1,		RSVD2,		RSVD,	OUTPUT,	0x3070),\
	PINGROUP(LCD_PWR2,		PC6,		LCD,		DISPLAYA,	DISPLAYB,	SPI5,		INVALID,	RSVD,	OUTPUT,	0x3074),\
	PINGROUP(LCD_SDIN,		PZ2,		LCD,		DISPLAYA,	DISPLAYB,	SPI5,		RSVD,		RSVD,	OUTPUT,	0x3078),\
	PINGROUP(LCD_SDOUT,		PN5,		LCD,		DISPLAYA,	DISPLAYB,	SPI5,		INVALID,	RSVD,	OUTPUT,	0x307c),\
	PINGROUP(LCD_WR_N,		PZ3,		LCD,		DISPLAYA,	DISPLAYB,	SPI5,		INVALID,	RSVD,	OUTPUT,	0x3080),\
	PINGROUP(LCD_CS0_N,		PN4,		LCD,		DISPLAYA,	DISPLAYB,	SPI5,		RSVD,		RSVD,	OUTPUT,	0x3084),\
	PINGROUP(LCD_DC0,		PN6,		LCD,		DISPLAYA,	DISPLAYB,	RSVD1,		RSVD2,		RSVD,	OUTPUT,	0x3088),\
	PINGROUP(LCD_SCK,		PZ4,		LCD,		DISPLAYA,	DISPLAYB,	SPI5,		INVALID,	RSVD,	OUTPUT,	0x308c),\
	PINGROUP(LCD_PWR0,		PB2,		LCD,		DISPLAYA,	DISPLAYB,	SPI5,		INVALID,	RSVD,	OUTPUT,	0x3090),\
	PINGROUP(LCD_PCLK,		PB3,		LCD,		DISPLAYA,	DISPLAYB,	RSVD1,		RSVD2,		RSVD,	OUTPUT,	0x3094),\
	PINGROUP(LCD_DE,		PJ1,		LCD,		DISPLAYA,	DISPLAYB,	RSVD1,		RSVD2,		RSVD,	OUTPUT,	0x3098),\
	PINGROUP(LCD_HSYNC,		PJ3,		LCD,		DISPLAYA,	DISPLAYB,	RSVD1,		RSVD2,		RSVD,	OUTPUT,	0x309c),\
	PINGROUP(LCD_VSYNC,		PJ4,		LCD,		DISPLAYA,	DISPLAYB,	RSVD1,		RSVD2,		RSVD,	OUTPUT,	0x30a0),\
	PINGROUP(LCD_D0,		PE0,		LCD,		DISPLAYA,	DISPLAYB,	RSVD1,		RSVD2,		RSVD,	OUTPUT,	0x30a4),\
	PINGROUP(LCD_D1,		PE1,		LCD,		DISPLAYA,	DISPLAYB,	RSVD1,		RSVD2,		RSVD,	OUTPUT,	0x30a8),\
	PINGROUP(LCD_D2,		PE2,		LCD,		DISPLAYA,	DISPLAYB,	RSVD1,		RSVD2,		RSVD,	OUTPUT,	0x30ac),\
	PINGROUP(LCD_D3,		PE3,		LCD,		DISPLAYA,	DISPLAYB,	RSVD1,		RSVD2,		RSVD,	OUTPUT,	0x30b0),\
	PINGROUP(LCD_D4,		PE4,		LCD,		DISPLAYA,	DISPLAYB,	RSVD1,		RSVD2,		RSVD,	OUTPUT,	0x30b4),\
	PINGROUP(LCD_D5,		PE5,		LCD,		DISPLAYA,	DISPLAYB,	RSVD1,		RSVD2,		RSVD,	OUTPUT,	0x30b8),\
	PINGROUP(LCD_D6,		PE6,		LCD,		DISPLAYA,	DISPLAYB,	RSVD1,		RSVD2,		RSVD,	OUTPUT,	0x30bc),\
	PINGROUP(LCD_D7,		PE7,		LCD,		DISPLAYA,	DISPLAYB,	RSVD1,		RSVD2,		RSVD,	OUTPUT,	0x30c0),\
	PINGROUP(LCD_D8,		PF0,		LCD,		DISPLAYA,	DISPLAYB,	RSVD1,		RSVD2,		RSVD,	OUTPUT,	0x30c4),\
	PINGROUP(LCD_D9,		PF1,		LCD,		DISPLAYA,	DISPLAYB,	RSVD1,		RSVD2,		RSVD,	OUTPUT,	0x30c8),\
	PINGROUP(LCD_D10,		PF2,		LCD,		DISPLAYA,	DISPLAYB,	RSVD1,		RSVD2,		RSVD,	OUTPUT,	0x30cc),\
	PINGROUP(LCD_D11,		PF3,		LCD,		DISPLAYA,	DISPLAYB,	RSVD1,		RSVD2,		RSVD,	OUTPUT,	0x30d0),\
	PINGROUP(LCD_D12,		PF4,		LCD,		DISPLAYA,	DISPLAYB,	RSVD1,		RSVD2,		RSVD,	OUTPUT,	0x30d4),\
	PINGROUP(LCD_D13,		PF5,		LCD,		DISPLAYA,	DISPLAYB,	RSVD1,		RSVD2,		RSVD,	OUTPUT,	0x30d8),\
	PINGROUP(LCD_D14,		PF6,		LCD,		DISPLAYA,	DISPLAYB,	RSVD1,		RSVD2,		RSVD,	OUTPUT,	0x30dc),\
	PINGROUP(LCD_D15,		PF7,		LCD,		DISPLAYA,	DISPLAYB,	RSVD1,		RSVD2,		RSVD,	OUTPUT,	0x30e0),\
	PINGROUP(LCD_D16,		PM0,		LCD,		DISPLAYA,	DISPLAYB,	RSVD1,		RSVD2,		RSVD,	OUTPUT,	0x30e4),\
	PINGROUP(LCD_D17,		PM1,		LCD,		DISPLAYA,	DISPLAYB,	RSVD1,		RSVD2,		RSVD,	OUTPUT,	0x30e8),\
	PINGROUP(LCD_D18,		PM2,		LCD,		DISPLAYA,	DISPLAYB,	RSVD1,		RSVD2,		RSVD,	OUTPUT,	0x30ec),\
	PINGROUP(LCD_D19,		PM3,		LCD,		DISPLAYA,	DISPLAYB,	RSVD1,		RSVD2,		RSVD,	OUTPUT,	0x30f0),\
	PINGROUP(LCD_D20,		PM4,		LCD,		DISPLAYA,	DISPLAYB,	RSVD1,		RSVD2,		RSVD,	OUTPUT,	0x30f4),\
	PINGROUP(LCD_D21,		PM5,		LCD,		DISPLAYA,	DISPLAYB,	RSVD1,		RSVD2,		RSVD,	OUTPUT,	0x30f8),\
	PINGROUP(LCD_D22,		PM6,		LCD,		DISPLAYA,	DISPLAYB,	RSVD1,		RSVD2,		RSVD,	OUTPUT,	0x30fc),\
	PINGROUP(LCD_D23,		PM7,		LCD,		DISPLAYA,	DISPLAYB,	RSVD1,		RSVD2,		RSVD,	OUTPUT,	0x3100),\
	PINGROUP(LCD_CS1_N,		PW0,		LCD,		DISPLAYA,	DISPLAYB,	SPI5,		RSVD2,		RSVD,	OUTPUT,	0x3104),\
	PINGROUP(LCD_M1,		PW1,		LCD,		DISPLAYA,	DISPLAYB,	RSVD1,		RSVD2,		RSVD,	OUTPUT,	0x3108),\
	PINGROUP(LCD_DC1,		PD2,		LCD,		DISPLAYA,	DISPLAYB,	RSVD1,		RSVD2,		RSVD,	OUTPUT,	0x310c),\
	PINGROUP(HDMI_INT,		PN7,		LCD,		RSVD,		RSVD,		RSVD,		RSVD,		RSVD,	INPUT,	0x3110),\
	PINGROUP(DDC_SCL,		PV4,		LCD,		I2C4,		RSVD1,		RSVD2,		RSVD3,		RSVD,	INPUT,	0x3114),\
	PINGROUP(DDC_SDA,		PV5,		LCD,		I2C4,		RSVD1,		RSVD2,		RSVD3,		RSVD,	INPUT,	0x3118),\
	PINGROUP(CRT_HSYNC,		PV6,		LCD,		CRT,		RSVD1,		RSVD2,		RSVD3,		RSVD,	INPUT,	0x311c),\
	PINGROUP(CRT_VSYNC,		PV7,		LCD,		CRT,		RSVD1,		RSVD2,		RSVD3,		RSVD,	INPUT,	0x3120),\
	PINGROUP(VI_D0,			PT4,		VI,		INVALID,	RSVD1,		VI,		RSVD2,		RSVD,	INPUT,	0x3124),\
	PINGROUP(VI_D1,			PD5,		VI,		INVALID,	SDMMC2,		VI,		RSVD1,		RSVD,	INPUT,	0x3128),\
	PINGROUP(VI_D2,			PL0,		VI,		INVALID,	SDMMC2,		VI,		RSVD1,		RSVD,	INPUT,	0x312c),\
	PINGROUP(VI_D3,			PL1,		VI,		INVALID,	SDMMC2,		VI,		RSVD1,		RSVD,	INPUT,	0x3130),\
	PINGROUP(VI_D4,			PL2,		VI,		INVALID,	SDMMC2,		VI,		RSVD1,		RSVD,	INPUT,	0x3134),\
	PINGROUP(VI_D5,			PL3,		VI,		INVALID,	SDMMC2,		VI,		RSVD1,		RSVD,	INPUT,	0x3138),\
	PINGROUP(VI_D6,			PL4,		VI,		INVALID,	SDMMC2,		VI,		RSVD1,		RSVD,	INPUT,	0x313c),\
	PINGROUP(VI_D7,			PL5,		VI,		INVALID,	SDMMC2,		VI,		RSVD1,		RSVD,	INPUT,	0x3140),\
	PINGROUP(VI_D8,			PL6,		VI,		INVALID,	SDMMC2,		VI,		RSVD1,		RSVD,	INPUT,	0x3144),\
	PINGROUP(VI_D9,			PL7,		VI,		INVALID,	SDMMC2,		VI,		RSVD1,		RSVD,	INPUT,	0x3148),\
	PINGROUP(VI_D10,		PT2,		VI,		INVALID,	RSVD1,		VI,		RSVD2,		RSVD,	INPUT,	0x314c),\
	PINGROUP(VI_D11,		PT3,		VI,		INVALID,	RSVD1,		VI,		RSVD2,		RSVD,	INPUT,	0x3150),\
	PINGROUP(VI_PCLK,		PT0,		VI,		RSVD1,		SDMMC2,		VI,		RSVD2,		RSVD,	INPUT,	0x3154),\
	PINGROUP(VI_MCLK,		PT1,		VI,		INVALID,	INVALID,	INVALID,	VI,		RSVD,	INPUT,	0x3158),\
	PINGROUP(VI_VSYNC,		PD6,		VI,		INVALID,	RSVD1,		VI,		RSVD2,		RSVD,	INPUT,	0x315c),\
	PINGROUP(VI_HSYNC,		PD7,		VI,		INVALID,	RSVD1,		VI,		RSVD2,		RSVD,	INPUT,	0x3160),\
	PINGROUP(UART2_RXD,		PC3,		UART,		IRDA,		SPDIF,		UARTA,		SPI4,		RSVD,	INPUT,	0x3164),\
	PINGROUP(UART2_TXD,		PC2,		UART,		IRDA,		SPDIF,		UARTA,		SPI4,		RSVD,	INPUT,	0x3168),\
	PINGROUP(UART2_RTS_N,		PJ6,		UART,		UARTA,		UARTB,		GMI,		SPI4,		RSVD,	INPUT,	0x316c),\
	PINGROUP(UART2_CTS_N,		PJ5,		UART,		UARTA,		UARTB,		GMI,		SPI4,		RSVD,	INPUT,	0x3170),\
	PINGROUP(UART3_TXD,		PW6,		UART,		UARTC,		RSVD1,		GMI,		RSVD2,		RSVD,	INPUT,	0x3174),\
	PINGROUP(UART3_RXD,		PW7,		UART,		UARTC,		RSVD1,		GMI,		RSVD2,		RSVD,	INPUT,	0x3178),\
	PINGROUP(UART3_CTS_N,		PA1,		UART,		UARTC,		RSVD1,		GMI,		RSVD2,		RSVD,	INPUT,	0x317c),\
	PINGROUP(UART3_RTS_N,		PC0,		UART,		UARTC,		PWM0,		GMI,		RSVD2,		RSVD,	INPUT,	0x3180),\
	PINGROUP(GPIO_PU0,		PU0,		UART,		OWR,		UARTA,		GMI,		RSVD1,		RSVD,	INPUT,	0x3184),\
	PINGROUP(GPIO_PU1,		PU1,		UART,		RSVD1,		UARTA,		GMI,		RSVD2,		RSVD,	INPUT,	0x3188),\
	PINGROUP(GPIO_PU2,		PU2,		UART,		RSVD1,		UARTA,		GMI,		RSVD2,		RSVD,	INPUT,	0x318c),\
	PINGROUP(GPIO_PU3,		PU3,		UART,		PWM0,		UARTA,		GMI,		RSVD1,		RSVD,	INPUT,	0x3190),\
	PINGROUP(GPIO_PU4,		PU4,		UART,		PWM1,		UARTA,		GMI,		RSVD1,		RSVD,	INPUT,	0x3194),\
	PINGROUP(GPIO_PU5,		PU5,		UART,		PWM2,		UARTA,		GMI,		RSVD1,		RSVD,	INPUT,	0x3198),\
	PINGROUP(GPIO_PU6,		PU6,		UART,		PWM3,		UARTA,		GMI,		RSVD1,		RSVD,	INPUT,	0x319c),\
	PINGROUP(GEN1_I2C_SDA,		PC5,		UART,		I2C1,		RSVD1,		RSVD2,		RSVD3,		RSVD,	INPUT,	0x31a0),\
	PINGROUP(GEN1_I2C_SCL,		PC4,		UART,		I2C1,		RSVD1,		RSVD2,		RSVD3,		RSVD,	INPUT,	0x31a4),\
	PINGROUP(DAP4_FS,		PP4,		UART,		I2S3,		RSVD1,		GMI,		RSVD2,		RSVD,	INPUT,	0x31a8),\
	PINGROUP(DAP4_DIN,		PP5,		UART,		I2S3,		RSVD1,		GMI,		RSVD2,		RSVD,	INPUT,	0x31ac),\
	PINGROUP(DAP4_DOUT,		PP6,		UART,		I2S3,		RSVD1,		GMI,		RSVD2,		RSVD,	INPUT,	0x31b0),\
	PINGROUP(DAP4_SCLK,		PP7,		UART,		I2S3,		RSVD1,		GMI,		RSVD2,		RSVD,	INPUT,	0x31b4),\
	PINGROUP(CLK3_OUT,		PEE0,		UART,		EXTPERIPH3,	RSVD1,		RSVD2,		RSVD3,		RSVD,	INPUT,	0x31b8),\
	PINGROUP(CLK3_REQ,		PEE1,		UART,		DEV3,		RSVD1,		RSVD2,		RSVD3,		RSVD,	INPUT,	0x31bc),\
	PINGROUP(GMI_WP_N,		PC7,		GMI,		RSVD1,		NAND,		GMI,		GMI_ALT,	RSVD,	INPUT,	0x31c0),\
	PINGROUP(GMI_IORDY,		PI5,		GMI,		RSVD1,		NAND,		GMI,		RSVD2,		RSVD,	INPUT,	0x31c4),\
	PINGROUP(GMI_WAIT,		PI7,		GMI,		RSVD1,		NAND,		GMI,		RSVD2,		RSVD,	INPUT,	0x31c8),\
	PINGROUP(GMI_ADV_N,		PK0,		GMI,		RSVD1,		NAND,		GMI,		RSVD2,		RSVD,	INPUT,	0x31cc),\
	PINGROUP(GMI_CLK,		PK1,		GMI,		RSVD1,		NAND,		GMI,		RSVD2,		RSVD,	INPUT,	0x31d0),\
	PINGROUP(GMI_CS0_N,		PJ0,		GMI,		RSVD1,		NAND,		GMI,		DTV,		RSVD,	INPUT,	0x31d4),\
	PINGROUP(GMI_CS1_N,		PJ2,		GMI,		RSVD1,		NAND,		GMI,		DTV,		RSVD,	INPUT,	0x31d8),\
	PINGROUP(GMI_CS2_N,		PK3,		GMI,		RSVD1,		NAND,		GMI,		RSVD2,		RSVD,	INPUT,	0x31dc),\
	PINGROUP(GMI_CS3_N,		PK4,		GMI,		RSVD1,		NAND,		GMI,		GMI_ALT,	RSVD,	INPUT,	0x31e0),\
	PINGROUP(GMI_CS4_N,		PK2,		GMI,		RSVD1,		NAND,		GMI,		RSVD2,		RSVD,	INPUT,	0x31e4),\
	PINGROUP(GMI_CS6_N,		PI3,		GMI,		NAND,		NAND_ALT,	GMI,		SATA,		RSVD,	INPUT,	0x31e8),\
	PINGROUP(GMI_CS7_N,		PI6,		GMI,		NAND,		NAND_ALT,	GMI,		GMI_ALT,	RSVD,	INPUT,	0x31ec),\
	PINGROUP(GMI_AD0,		PG0,		GMI,		RSVD1,		NAND,		GMI,		RSVD2,		RSVD,	INPUT,	0x31f0),\
	PINGROUP(GMI_AD1,		PG1,		GMI,		RSVD1,		NAND,		GMI,		RSVD2,		RSVD,	INPUT,	0x31f4),\
	PINGROUP(GMI_AD2,		PG2,		GMI,		RSVD1,		NAND,		GMI,		RSVD2,		RSVD,	INPUT,	0x31f8),\
	PINGROUP(GMI_AD3,		PG3,		GMI,		RSVD1,		NAND,		GMI,		RSVD2,		RSVD,	INPUT,	0x31fc),\
	PINGROUP(GMI_AD4,		PG4,		GMI,		RSVD1,		NAND,		GMI,		RSVD2,		RSVD,	INPUT,	0x3200),\
	PINGROUP(GMI_AD5,		PG5,		GMI,		RSVD1,		NAND,		GMI,		RSVD2,		RSVD,	INPUT,	0x3204),\
	PINGROUP(GMI_AD6,		PG6,		GMI,		RSVD1,		NAND,		GMI,		RSVD2,		RSVD,	INPUT,	0x3208),\
	PINGROUP(GMI_AD7,		PG7,		GMI,		RSVD1,		NAND,		GMI,		RSVD2,		RSVD,	INPUT,	0x320c),\
	PINGROUP(GMI_AD8,		PH0,		GMI,		PWM0,		NAND,		GMI,		RSVD2,		RSVD,	INPUT,	0x3210),\
	PINGROUP(GMI_AD9,		PH1,		GMI,		PWM1,		NAND,		GMI,		RSVD2,		RSVD,	INPUT,	0x3214),\
	PINGROUP(GMI_AD10,		PH2,		GMI,		PWM2,		NAND,		GMI,		RSVD2,		RSVD,	INPUT,	0x3218),\
	PINGROUP(GMI_AD11,		PH3,		GMI,		PWM3,		NAND,		GMI,		RSVD2,		RSVD,	INPUT,	0x321c),\
	PINGROUP(GMI_AD12,		PH4,		GMI,		RSVD1,		NAND,		GMI,		RSVD2,		RSVD,	INPUT,	0x3220),\
	PINGROUP(GMI_AD13,		PH5,		GMI,		RSVD1,		NAND,		GMI,		RSVD2,		RSVD,	INPUT,	0x3224),\
	PINGROUP(GMI_AD14,		PH6,		GMI,		RSVD1,		NAND,		GMI,		RSVD2,		RSVD,	INPUT,	0x3228),\
	PINGROUP(GMI_AD15,		PH7,		GMI,		RSVD1,		NAND,		GMI,		RSVD2,		RSVD,	INPUT,	0x322c),\
	PINGROUP(GMI_A16,		PJ7,		GMI,		UARTD,		SPI4,		GMI,		GMI_ALT,	RSVD,	INPUT,	0x3230),\
	PINGROUP(GMI_A17,		PB0,		GMI,		UARTD,		SPI4,		GMI,		DTV,		RSVD,	INPUT,	0x3234),\
	PINGROUP(GMI_A18,		PB1,		GMI,		UARTD,		SPI4,		GMI,		DTV,		RSVD,	INPUT,	0x3238),\
	PINGROUP(GMI_A19,		PK7,		GMI,		UARTD,		SPI4,		GMI,		RSVD3,		RSVD,	INPUT,	0x323c),\
	PINGROUP(GMI_WR_N,		PI0,		GMI,		RSVD1,		NAND,		GMI,		RSVD3,		RSVD,	INPUT,	0x3240),\
	PINGROUP(GMI_OE_N,		PI1,		GMI,		RSVD1,		NAND,		GMI,		RSVD3,		RSVD,	INPUT,	0x3244),\
	PINGROUP(GMI_DQS,		PI2,		GMI,		RSVD1,		NAND,		GMI,		RSVD3,		RSVD,	INPUT,	0x3248),\
	PINGROUP(GMI_RST_N,		PI4,		GMI,		NAND,		NAND_ALT,	GMI,		RSVD3,		RSVD,	INPUT,	0x324c),\
	PINGROUP(GEN2_I2C_SCL,		PT5,		GMI,		I2C2,		INVALID,	GMI,		RSVD3,		RSVD,	INPUT,	0x3250),\
	PINGROUP(GEN2_I2C_SDA,		PT6,		GMI,		I2C2,		INVALID,	GMI,		RSVD3,		RSVD,	INPUT,	0x3254),\
	PINGROUP(SDMMC4_CLK,		PCC4,		SDMMC4,		INVALID,	NAND,		GMI,		SDMMC4,		RSVD,	INPUT,	0x3258),\
	PINGROUP(SDMMC4_CMD,		PT7,		SDMMC4,		I2C3,		NAND,		GMI,		SDMMC4,		RSVD,	INPUT,	0x325c),\
	PINGROUP(SDMMC4_DAT0,		PAA0,		SDMMC4,		UARTE,		SPI3,		GMI,		SDMMC4,		RSVD,	INPUT,	0x3260),\
	PINGROUP(SDMMC4_DAT1,		PAA1,		SDMMC4,		UARTE,		SPI3,		GMI,		SDMMC4,		RSVD,	INPUT,	0x3264),\
	PINGROUP(SDMMC4_DAT2,		PAA2,		SDMMC4,		UARTE,		SPI3,		GMI,		SDMMC4,		RSVD,	INPUT,	0x3268),\
	PINGROUP(SDMMC4_DAT3,		PAA3,		SDMMC4,		UARTE,		SPI3,		GMI,		SDMMC4,		RSVD,	INPUT,	0x326c),\
	PINGROUP(SDMMC4_DAT4,		PAA4,		SDMMC4,		I2C3,		I2S4,		GMI,		SDMMC4,		RSVD,	INPUT,	0x3270),\
	PINGROUP(SDMMC4_DAT5,		PAA5,		SDMMC4,		VGP3,		I2S4,		GMI,		SDMMC4,		RSVD,	INPUT,	0x3274),\
	PINGROUP(SDMMC4_DAT6,		PAA6,		SDMMC4,		VGP4,		I2S4,		GMI,		SDMMC4,		RSVD,	INPUT,	0x3278),\
	PINGROUP(SDMMC4_DAT7,		PAA7,		SDMMC4,		VGP5,		I2S4,		GMI,		SDMMC4,		RSVD,	INPUT,	0x327c),\
	PINGROUP(SDMMC4_RST_N,		PCC3,		SDMMC4,		VGP6,		RSVD1,		RSVD2,		POPSDMMC4,	RSVD,	INPUT,	0x3280),\
	PINGROUP(CAM_MCLK,		PCC0,		CAM,		VI,		INVALID,	VI_ALT2,	POPSDMMC4,	RSVD,	INPUT,	0x3284),\
	PINGROUP(GPIO_PCC1,		PCC1,		CAM,		I2S4,		RSVD1,		RSVD2,		POPSDMMC4,	RSVD,	INPUT,	0x3288),\
	PINGROUP(GPIO_PBB0,		PBB0,		CAM,		I2S4,		RSVD1,		RSVD2,		POPSDMMC4,	RSVD,	INPUT,	0x328c),\
	PINGROUP(CAM_I2C_SCL,		PBB1,		CAM,		INVALID,	I2C3,		RSVD2,		POPSDMMC4,	RSVD,	INPUT,	0x3290),\
	PINGROUP(CAM_I2C_SDA,		PBB2,		CAM,		INVALID,	I2C3,		RSVD2,		POPSDMMC4,	RSVD,	INPUT,	0x3294),\
	PINGROUP(GPIO_PBB3,		PBB3,		CAM,		VGP3,		DISPLAYA,	DISPLAYB,	POPSDMMC4,	RSVD,	INPUT,	0x3298),\
	PINGROUP(GPIO_PBB4,		PBB4,		CAM,		VGP4,		DISPLAYA,	DISPLAYB,	POPSDMMC4,	RSVD,	INPUT,	0x329c),\
	PINGROUP(GPIO_PBB5,		PBB5,		CAM,		VGP5,		DISPLAYA,	DISPLAYB,	POPSDMMC4,	RSVD,	INPUT,	0x32a0),\
	PINGROUP(GPIO_PBB6,		PBB6,		CAM,		VGP6,		DISPLAYA,	DISPLAYB,	POPSDMMC4,	RSVD,	INPUT,	0x32a4),\
	PINGROUP(GPIO_PBB7,		PBB7,		CAM,		I2S4,		RSVD1,		RSVD2,		POPSDMMC4,	RSVD,	INPUT,	0x32a8),\
	PINGROUP(GPIO_PCC2,		PCC2,		CAM,		I2S4,		RSVD1,		RSVD2,		RSVD3,		RSVD,	INPUT,	0x32ac),\
	PINGROUP(JTAG_RTCK,		PU7,		SYS,		RTCK,		RSVD1,		RSVD2,		RSVD3,		RSVD,	INPUT,	0x32b0),\
	PINGROUP(PWR_I2C_SCL,		PZ6,		SYS,		I2CPWR,		RSVD1,		RSVD2,		RSVD3,		RSVD,	INPUT,	0x32b4),\
	PINGROUP(PWR_I2C_SDA,		PZ7,		SYS,		I2CPWR,		RSVD1,		RSVD2,		RSVD3,		RSVD,	INPUT,	0x32b8),\
	PINGROUP(KB_ROW0,		PR0,		SYS,		KBC,		INVALID,	RSVD2,		RSVD3,		RSVD,	INPUT,	0x32bc),\
	PINGROUP(KB_ROW1,		PR1,		SYS,		KBC,		INVALID,	RSVD2,		RSVD3,		RSVD,	INPUT,	0x32c0),\
	PINGROUP(KB_ROW2,		PR2,		SYS,		KBC,		INVALID,	RSVD2,		RSVD3,		RSVD,	INPUT,	0x32c4),\
	PINGROUP(KB_ROW3,		PR3,		SYS,		KBC,		INVALID,	RSVD2,		INVALID,	RSVD,	INPUT,	0x32c8),\
	PINGROUP(KB_ROW4,		PR4,		SYS,		KBC,		INVALID,	TRACE,		RSVD3,		RSVD,	INPUT,	0x32cc),\
	PINGROUP(KB_ROW5,		PR5,		SYS,		KBC,		INVALID,	TRACE,		OWR,		RSVD,	INPUT,	0x32d0),\
	PINGROUP(KB_ROW6,		PR6,		SYS,		KBC,		INVALID,	SDMMC2,		INVALID,	RSVD,	INPUT,	0x32d4),\
	PINGROUP(KB_ROW7,		PR7,		SYS,		KBC,		INVALID,	SDMMC2,		INVALID,	RSVD,	INPUT,	0x32d8),\
	PINGROUP(KB_ROW8,		PS0,		SYS,		KBC,		INVALID,	SDMMC2,		INVALID,	RSVD,	INPUT,	0x32dc),\
	PINGROUP(KB_ROW9,		PS1,		SYS,		KBC,		INVALID,	SDMMC2,		INVALID,	RSVD,	INPUT,	0x32e0),\
	PINGROUP(KB_ROW10,		PS2,		SYS,		KBC,		INVALID,	SDMMC2,		INVALID,	RSVD,	INPUT,	0x32e4),\
	PINGROUP(KB_ROW11,		PS3,		SYS,		KBC,		INVALID,	SDMMC2,		INVALID,	RSVD,	INPUT,	0x32e8),\
	PINGROUP(KB_ROW12,		PS4,		SYS,		KBC,		INVALID,	SDMMC2,		INVALID,	RSVD,	INPUT,	0x32ec),\
	PINGROUP(KB_ROW13,		PS5,		SYS,		KBC,		INVALID,	SDMMC2,		INVALID,	RSVD,	INPUT,	0x32f0),\
	PINGROUP(KB_ROW14,		PS6,		SYS,		KBC,		INVALID,	SDMMC2,		INVALID,	RSVD,	INPUT,	0x32f4),\
	PINGROUP(KB_ROW15,		PS7,		SYS,		KBC,		INVALID,	SDMMC2,		INVALID,	RSVD,	INPUT,	0x32f8),\
	PINGROUP(KB_COL0,		PQ0,		SYS,		KBC,		INVALID,	TRACE,		INVALID,	RSVD,	INPUT,	0x32fc),\
	PINGROUP(KB_COL1,		PQ1,		SYS,		KBC,		INVALID,	TRACE,		INVALID,	RSVD,	INPUT,	0x3300),\
	PINGROUP(KB_COL2,		PQ2,		SYS,		KBC,		INVALID,	TRACE,		RSVD,		RSVD,	INPUT,	0x3304),\
	PINGROUP(KB_COL3,		PQ3,		SYS,		KBC,		INVALID,	TRACE,		RSVD,		RSVD,	INPUT,	0x3308),\
	PINGROUP(KB_COL4,		PQ4,		SYS,		KBC,		INVALID,	TRACE,		RSVD,		RSVD,	INPUT,	0x330c),\
	PINGROUP(KB_COL5,		PQ5,		SYS,		KBC,		INVALID,	TRACE,		RSVD,		RSVD,	INPUT,	0x3310),\
	PINGROUP(KB_COL6,		PQ6,		SYS,		KBC,		INVALID,	TRACE,		INVALID,	RSVD,	INPUT,	0x3314),\
	PINGROUP(KB_COL7,		PQ7,		SYS,		KBC,		INVALID,	TRACE,		INVALID,	RSVD,	INPUT,	0x3318),\
	PINGROUP(CLK_32K_OUT,		PA0,		SYS,		BLINK,		RSVD1,		RSVD2,		RSVD3,		RSVD,	INPUT,	0x331c),\
	PINGROUP(SYS_CLK_REQ,		PZ5,		SYS,		SYSCLK,		RSVD1,		RSVD2,		RSVD3,		RSVD,	INPUT,	0x3320),\
	PINGROUP(CORE_PWR_REQ,		INVALID,	SYS,		RSVD,		RSVD,		RSVD,		RSVD,		RSVD,	INPUT,	0x3324),\
	PINGROUP(CPU_PWR_REQ,		INVALID,	SYS,		RSVD,		RSVD,		RSVD,		RSVD,		RSVD,	INPUT,	0x3328),\
	PINGROUP(PWR_INT_N,		INVALID,	SYS,		RSVD,		RSVD,		RSVD,		RSVD,		RSVD,	INPUT,	0x332c),\
	PINGROUP(CLK_32K_IN,		INVALID,	SYS,		RSVD,		RSVD,		RSVD,		RSVD,		RSVD,	INPUT,	0x3330),\
	PINGROUP(OWR,			INVALID,	SYS,		OWR,		RSVD,		RSVD,		RSVD,		RSVD,	INPUT,	0x3334),\
	PINGROUP(DAP1_FS,		PN0,		AUDIO,		I2S0,		HDA,		GMI,		SDMMC2,		RSVD,	INPUT,	0x3338),\
	PINGROUP(DAP1_DIN,		PN1,		AUDIO,		I2S0,		HDA,		GMI,		SDMMC2,		RSVD,	INPUT,	0x333c),\
	PINGROUP(DAP1_DOUT,		PN2,		AUDIO,		I2S0,		HDA,		GMI,		SDMMC2,		RSVD,	INPUT,	0x3340),\
	PINGROUP(DAP1_SCLK,		PN3,		AUDIO,		I2S0,		HDA,		GMI,		SDMMC2,		RSVD,	INPUT,	0x3344),\
	PINGROUP(CLK1_REQ,		PEE2,		AUDIO,		DAP,		HDA,		RSVD2,		RSVD3,		RSVD,	INPUT,	0x3348),\
	PINGROUP(CLK1_OUT,		PW4,		AUDIO,		EXTPERIPH1,	RSVD1,		RSVD2,		RSVD3,		RSVD,	INPUT,	0x334c),\
	PINGROUP(SPDIF_IN,		PK6,		AUDIO,		SPDIF,		HDA,		INVALID,	DAPSDMMC2,	RSVD,	INPUT,	0x3350),\
	PINGROUP(SPDIF_OUT,		PK5,		AUDIO,		SPDIF,		RSVD1,		INVALID,	DAPSDMMC2,	RSVD,	INPUT,	0x3354),\
	PINGROUP(DAP2_FS,		PA2,		AUDIO,		I2S1,		HDA,		RSVD2,		GMI,		RSVD,	INPUT,	0x3358),\
	PINGROUP(DAP2_DIN,		PA4,		AUDIO,		I2S1,		HDA,		RSVD2,		GMI,		RSVD,	INPUT,	0x335c),\
	PINGROUP(DAP2_DOUT,		PA5,		AUDIO,		I2S1,		HDA,		RSVD2,		GMI,		RSVD,	INPUT,	0x3360),\
	PINGROUP(DAP2_SCLK,		PA3,		AUDIO,		I2S1,		HDA,		RSVD2,		GMI,		RSVD,	INPUT,	0x3364),\
	PINGROUP(SPI2_MOSI,		PX0,		AUDIO,		SPI6,		SPI2,		INVALID,	GMI,		RSVD,	INPUT,	0x3368),\
	PINGROUP(SPI2_MISO,		PX1,		AUDIO,		SPI6,		SPI2,		INVALID,	GMI,		RSVD,	INPUT,	0x336c),\
	PINGROUP(SPI2_CS0_N,		PX3,		AUDIO,		SPI6,		SPI2,		INVALID,	GMI,		RSVD,	INPUT,	0x3370),\
	PINGROUP(SPI2_SCK,		PX2,		AUDIO,		SPI6,		SPI2,		INVALID,	GMI,		RSVD,	INPUT,	0x3374),\
	PINGROUP(SPI1_MOSI,		PX4,		AUDIO,		SPI2,		SPI1,		INVALID,	GMI,		RSVD,	INPUT,	0x3378),\
	PINGROUP(SPI1_SCK,		PX5,		AUDIO,		SPI2,		SPI1,		INVALID,	GMI,		RSVD,	INPUT,	0x337c),\
	PINGROUP(SPI1_CS0_N,		PX6,		AUDIO,		SPI2,		SPI1,		INVALID,	GMI,		RSVD,	INPUT,	0x3380),\
	PINGROUP(SPI1_MISO,		PX7,		AUDIO,		INVALID,	SPI1,		INVALID,	RSVD3,		RSVD,	INPUT,	0x3384),\
	PINGROUP(SPI2_CS1_N,		PW2,		AUDIO,		INVALID,	SPI2,		INVALID,	INVALID,	RSVD,	INPUT,	0x3388),\
	PINGROUP(SPI2_CS2_N,		PW3,		AUDIO,		INVALID,	SPI2,		INVALID,	INVALID,	RSVD,	INPUT,	0x338c),\
	PINGROUP(SDMMC3_CLK,		PA6,		SDMMC3,		UARTA,		PWM2,		SDMMC3,		INVALID,	RSVD,	INPUT,	0x3390),\
	PINGROUP(SDMMC3_CMD,		PA7,		SDMMC3,		UARTA,		PWM3,		SDMMC3,		INVALID,	RSVD,	INPUT,	0x3394),\
	PINGROUP(SDMMC3_DAT0,		PB7,		SDMMC3,		RSVD0,		RSVD1,		SDMMC3,		INVALID,	RSVD,	INPUT,	0x3398),\
	PINGROUP(SDMMC3_DAT1,		PB6,		SDMMC3,		RSVD0,		RSVD1,		SDMMC3,		INVALID,	RSVD,	INPUT,	0x339c),\
	PINGROUP(SDMMC3_DAT2,		PB5,		SDMMC3,		RSVD0,		PWM1,		SDMMC3,		INVALID,	RSVD,	INPUT,	0x33a0),\
	PINGROUP(SDMMC3_DAT3,		PB4,		SDMMC3,		RSVD0,		PWM0,		SDMMC3,		INVALID,	RSVD,	INPUT,	0x33a4),\
	PINGROUP(SDMMC3_DAT4,		PD1,		SDMMC3,		PWM1,		INVALID,	SDMMC3,		INVALID,	RSVD,	INPUT,	0x33a8),\
	PINGROUP(SDMMC3_DAT5,		PD0,		SDMMC3,		PWM0,		INVALID,	SDMMC3,		INVALID,	RSVD,	INPUT,	0x33ac),\
	PINGROUP(SDMMC3_DAT6,		PD3,		SDMMC3,		SPDIF,		INVALID,	SDMMC3,		INVALID,	RSVD,	INPUT,	0x33b0),\
	PINGROUP(SDMMC3_DAT7,		PD4,		SDMMC3,		SPDIF,		INVALID,	SDMMC3,		INVALID,	RSVD,	INPUT,	0x33b4),\
	PINGROUP(PEX_L0_PRSNT_N,	PDD0,		PEXCTL,		PCIE,		HDA,		RSVD2,		RSVD3,		RSVD,	INPUT,	0x33b8),\
	PINGROUP(PEX_L0_RST_N,		PDD1,		PEXCTL,		PCIE,		HDA,		RSVD2,		RSVD3,		RSVD,	INPUT,	0x33bc),\
	PINGROUP(PEX_L0_CLKREQ_N,	PDD2,		PEXCTL,		PCIE,		HDA,		RSVD2,		RSVD3,		RSVD,	INPUT,	0x33c0),\
	PINGROUP(PEX_WAKE_N,		PDD3,		PEXCTL,		PCIE,		HDA,		RSVD2,		RSVD3,		RSVD,	INPUT,	0x33c4),\
	PINGROUP(PEX_L1_PRSNT_N,	PDD4,		PEXCTL,		PCIE,		HDA,		RSVD2,		RSVD3,		RSVD,	INPUT,	0x33c8),\
	PINGROUP(PEX_L1_RST_N,		PDD5,		PEXCTL,		PCIE,		HDA,		RSVD2,		RSVD3,		RSVD,	INPUT,	0x33cc),\
	PINGROUP(PEX_L1_CLKREQ_N,	PDD6,		PEXCTL,		PCIE,		HDA,		RSVD2,		RSVD3,		RSVD,	INPUT,	0x33d0),\
	PINGROUP(PEX_L2_PRSNT_N,	PDD7,		PEXCTL,		PCIE,		HDA,		RSVD2,		RSVD3,		RSVD,	INPUT,	0x33d4),\
	PINGROUP(PEX_L2_RST_N,		PCC6,		PEXCTL,		PCIE,		HDA,		RSVD2,		RSVD3,		RSVD,	INPUT,	0x33d8),\
	PINGROUP(PEX_L2_CLKREQ_N,	PCC7,		PEXCTL,		PCIE,		HDA,		RSVD2,		RSVD3,		RSVD,	INPUT,	0x33dc),\
	PINGROUP(HDMI_CEC,		PEE3,		SYS,		CEC,		RSVD1,		RSVD2,		RSVD3,		RSVD,	INPUT,	0x33e0),\
	/* END OF LIST */

static const struct tegra_pingroup_desc tegra_soc_pingroups[TEGRA_MAX_PINGROUP] = {
	PINGROUPS
};

#undef PINGROUP

#define PINGROUP(pg_name, gpio_nr, vdd, f0, f1, f2, f3, fs, iod, reg)	\
	[TEGRA_GPIO_##gpio_nr] =  TEGRA_PINGROUP_ ##pg_name\

static inline unsigned long pg_readl(unsigned long offset)
{
	return readl(IO_TO_VIRT(TEGRA_APB_MISC_BASE + offset));
}

#define SET_DRIVE(_name, _hsm, _schmitt, _drive, _pulldn_drive, _pullup_drive, _pulldn_slew, _pullup_slew) \
	{							\
		.pingroup = TEGRA_DRIVE_PINGROUP_##_name,	\
		.hsm = TEGRA_HSM_##_hsm,			\
		.schmitt = TEGRA_SCHMITT_##_schmitt,		\
		.drive = TEGRA_DRIVE_##_drive,			\
		.pull_down = TEGRA_PULL_##_pulldn_drive,	\
		.pull_up = TEGRA_PULL_##_pullup_drive,		\
		.slew_rising = TEGRA_SLEW_##_pulldn_slew,	\
		.slew_falling = TEGRA_SLEW_##_pullup_slew,	\
	}

// static __initdata struct tegra_drive_pingroup_config t30_def_drive_pinmux[] = {
// 	SET_DRIVE(DAP2, DISABLE, ENABLE, DIV_1, 31, 31, FASTEST, FASTEST),
// 	SET_DRIVE(DAP1, DISABLE, ENABLE, DIV_1, 31, 31, FASTEST, FASTEST),
// };

#define DEFAULT_PINMUX(_pingroup, _mux, _pupd, _tri, _io)	\
	{							\
		.pingroup	= TEGRA_PINGROUP_##_pingroup,	\
		.func		= TEGRA_MUX_##_mux,		\
		.pupd		= TEGRA_PUPD_##_pupd,		\
		.tristate	= TEGRA_TRI_##_tri,		\
		.io		= TEGRA_PIN_##_io,		\
		.lock		= TEGRA_PIN_LOCK_DEFAULT,	\
		.od		= TEGRA_PIN_OD_DEFAULT,		\
		.ioreset	= TEGRA_PIN_IO_RESET_DEFAULT,	\
	}


#define HSM_EN(reg)	(((reg) >> 2) & 0x1)
#define SCHMT_EN(reg)	(((reg) >> 3) & 0x1)
#define LPMD(reg)	(((reg) >> 4) & 0x3)
#define DRVDN(reg, offset)	(((reg) >> offset) & 0x1f)
#define DRVUP(reg, offset)	(((reg) >> offset) & 0x1f)
#define SLWR(reg, offset)	(((reg) >> offset) & 0x3)
#define SLWF(reg, offset)	(((reg) >> offset) & 0x3)

static const struct tegra_pingroup_desc *const pingroups = tegra_soc_pingroups;
static const struct tegra_drive_pingroup_desc *const drive_pingroups = tegra_soc_drive_pingroups;

static char *tegra_mux_names[TEGRA_MAX_MUX] = {
#define TEGRA_MUX(mux) [TEGRA_MUX_##mux] = #mux,
	TEGRA_MUX_LIST
#undef  TEGRA_MUX
	[TEGRA_MUX_SAFE] = "<safe>",
};

static const char *tegra_drive_names[TEGRA_MAX_DRIVE] = {
	[TEGRA_DRIVE_DIV_8] = "DIV_8",
	[TEGRA_DRIVE_DIV_4] = "DIV_4",
	[TEGRA_DRIVE_DIV_2] = "DIV_2",
	[TEGRA_DRIVE_DIV_1] = "DIV_1",
};

static const char *tegra_slew_names[TEGRA_MAX_SLEW] = {
	[TEGRA_SLEW_FASTEST] = "FASTEST",
	[TEGRA_SLEW_FAST] = "FAST",
	[TEGRA_SLEW_SLOW] = "SLOW",
	[TEGRA_SLEW_SLOWEST] = "SLOWEST",
};

static const char *tri_name(unsigned long val)
{
	return val ? "TRISTATE" : "NORMAL";
}

static const char *pupd_name(unsigned long val)
{
	switch (val) {
	case 0:
		return "NORMAL";

	case 1:
		return "PULL_DOWN";

	case 2:
		return "PULL_UP";

	default:
		return "RSVD";
	}
}

#if defined(TEGRA_PINMUX_HAS_IO_DIRECTION)
static const char *io_name(unsigned long val)
{
	switch (val) {
	case 0:
		return "OUTPUT";

	case 1:
		return "INPUT";

	default:
		return "RSVD";
	}
}
#endif

static const char *drive_name(unsigned long val)
{
	if (val >= TEGRA_MAX_DRIVE)
		return "<UNKNOWN>";

	return tegra_drive_names[val];
}

static const char *slew_name(unsigned long val)
{
	if (val >= TEGRA_MAX_SLEW)
		return "<UNKNOWN>";

	return tegra_slew_names[val];
}

#include <linux/debugfs.h>
#include <linux/seq_file.h>

static void dbg_pad_field(struct seq_file *s, int len)
{
	seq_putc(s, ',');

	while (len-- > -1)
		seq_putc(s, ' ');
}

static int dbg_pinmux_show(struct seq_file *s, void *unused)
{
	int i;
	int len;

	for (i = 0; i < TEGRA_MAX_PINGROUP; i++) {
		unsigned long tri;
		unsigned long mux;
		unsigned long pupd;

		seq_printf(s, "\t{TEGRA_PINGROUP_%s", pingroups[i].name);
		len = strlen(pingroups[i].name);
		dbg_pad_field(s, 15 - len);

		if (pingroups[i].mux_reg <= 0) {
			seq_printf(s, "TEGRA_MUX_NONE");
			len = strlen("NONE");
		} else {
			mux = (pg_readl(pingroups[i].mux_reg) >>
			       pingroups[i].mux_bit) & 0x3;
			BUG_ON(pingroups[i].funcs[mux] == 0);
			if (pingroups[i].funcs[mux] ==  TEGRA_MUX_INVALID) {
				seq_printf(s, "TEGRA_MUX_INVALID");
				len = 7;
			} else if (pingroups[i].funcs[mux] & TEGRA_MUX_RSVD) {
				seq_printf(s, "TEGRA_MUX_RSVD%1lu", mux+1);
				len = 5;
			} else {
				BUG_ON(!tegra_mux_names[pingroups[i].funcs[mux]]);
				seq_printf(s, "TEGRA_MUX_%s",
					   tegra_mux_names[pingroups[i].funcs[mux]]);
				len = strlen(tegra_mux_names[pingroups[i].funcs[mux]]);
			}
		}
		dbg_pad_field(s, 13-len);

#if defined(TEGRA_PINMUX_HAS_IO_DIRECTION)
		{
			unsigned long io;
			io = (pg_readl(pingroups[i].mux_reg) >> 5) & 0x1;
			seq_printf(s, "TEGRA_PIN_%s", io_name(io));
			len = strlen(io_name(io));
			dbg_pad_field(s, 6 - len);
		}
#endif
		if (pingroups[i].pupd_reg <= 0) {
			seq_printf(s, "TEGRA_PUPD_NORMAL");
			len = strlen("NORMAL");
		} else {
			pupd = (pg_readl(pingroups[i].pupd_reg) >>
				pingroups[i].pupd_bit) & 0x3;
			seq_printf(s, "TEGRA_PUPD_%s", pupd_name(pupd));
			len = strlen(pupd_name(pupd));
		}
		dbg_pad_field(s, 9 - len);

		if (pingroups[i].tri_reg <= 0) {
			seq_printf(s, "TEGRA_TRI_NORMAL");
		} else {
			tri = (pg_readl(pingroups[i].tri_reg) >>
			       pingroups[i].tri_bit) & 0x1;

			seq_printf(s, "TEGRA_TRI_%s", tri_name(tri));
		}
		seq_printf(s, "},\n");
	}
	return 0;
}

static int dbg_pinmux_open(struct inode *inode, struct file *file)
{
	return single_open(file, dbg_pinmux_show, &inode->i_private);
}

static const struct file_operations debug_fops = {
	.open		= dbg_pinmux_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= single_release,
};

static int dbg_drive_pinmux_show(struct seq_file *s, void *unused)
{
	int i;
	int len;
	u8 offset;

	for (i = 0; i < TEGRA_MAX_DRIVE_PINGROUP; i++) {
		u32 reg;

		seq_printf(s, "\t{TEGRA_DRIVE_PINGROUP_%s",
			drive_pingroups[i].name);
		len = strlen(drive_pingroups[i].name);
		dbg_pad_field(s, 7 - len);


		reg = pg_readl(drive_pingroups[i].reg);
		if (HSM_EN(reg)) {
			seq_printf(s, "TEGRA_HSM_ENABLE");
			len = 16;
		} else {
			seq_printf(s, "TEGRA_HSM_DISABLE");
			len = 17;
		}
		dbg_pad_field(s, 17 - len);

		if (SCHMT_EN(reg)) {
			seq_printf(s, "TEGRA_SCHMITT_ENABLE");
			len = 21;
		} else {
			seq_printf(s, "TEGRA_SCHMITT_DISABLE");
			len = 22;
		}
		dbg_pad_field(s, 22 - len);

		seq_printf(s, "TEGRA_DRIVE_%s", drive_name(LPMD(reg)));
		len = strlen(drive_name(LPMD(reg)));
		dbg_pad_field(s, 5 - len);

		offset = drive_pingroups[i].drvdown_offset;
		seq_printf(s, "TEGRA_PULL_%d", DRVDN(reg, offset));
		len = DRVDN(reg, offset) < 10 ? 1 : 2;
		dbg_pad_field(s, 2 - len);

		offset = drive_pingroups[i].drvup_offset;
		seq_printf(s, "TEGRA_PULL_%d", DRVUP(reg, offset));
		len = DRVUP(reg, offset) < 10 ? 1 : 2;
		dbg_pad_field(s, 2 - len);

		offset = drive_pingroups[i].slewrise_offset;
		seq_printf(s, "TEGRA_SLEW_%s", slew_name(SLWR(reg, offset)));
		len = strlen(slew_name(SLWR(reg, offset)));
		dbg_pad_field(s, 7 - len);

		offset= drive_pingroups[i].slewfall_offset;
		seq_printf(s, "TEGRA_SLEW_%s", slew_name(SLWF(reg, offset)));

		seq_printf(s, "},\n");
	}
	return 0;
}

static int dbg_drive_pinmux_open(struct inode *inode, struct file *file)
{
	return single_open(file, dbg_drive_pinmux_show, &inode->i_private);
}

static const struct file_operations debug_drive_fops = {
	.open		= dbg_drive_pinmux_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= single_release,
};

static int __init tegra_pinmux_debuginit(void)
{
	if (!of_machine_is_compatible("nvidia,tegra30"))
		return 0;

	(void) debugfs_create_file("tegra_pinmux", S_IRUGO,
					NULL, NULL, &debug_fops);
	(void) debugfs_create_file("tegra_pinmux_drive", S_IRUGO,
					NULL, NULL, &debug_drive_fops);
	return 0;
}
late_initcall(tegra_pinmux_debuginit);
