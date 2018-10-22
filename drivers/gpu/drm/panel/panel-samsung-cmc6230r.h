#ifndef _SAMSUNG_CMC6230R_H_
#define _SAMSUNG_CMC6230R_H_

#define CMC623_REG_SELBANK   0x00

/* A stage configuration */
#define CMC623_REG_DNRHDTROVE 0x01
#define CMC623_REG_DITHEROFF 0x06
#define CMC623_REG_CLKCONT 0x10
#define CMC623_REG_CLKGATINGOFF 0x0a
#define CMC623_REG_INPUTIFCON 0x24
#define CMC623_REG_CLKMONCONT   0x11
#define CMC623_REG_HDRTCEOFF 0x3a
#define CMC623_REG_I2C 0x0d
#define CMC623_REG_BSTAGE 0x0e
#define CMC623_REG_CABCCTRL 0x7c
#define CMC623_REG_PWMCTRL 0xb4
#define CMC623_REG_OVEMAX 0x54

/* A stage image size */
#define CMC623_REG_1280 0x22
#define CMC623_REG_800 0x23

/* B stage image size */
#define CMC623_REG_SCALERINPH 0x09
#define CMC623_REG_SCALERINPV 0x0a
#define CMC623_REG_SCALEROUTH 0x0b
#define CMC623_REG_SCALEROUTV 0x0c

/* EDRAM configuration */
#define CMC623_REG_EDRBFOUT40 0x01
#define CMC623_REG_EDRAUTOREF 0x06
#define CMC623_REG_EDRACPARAMTIM 0x07

/* Vsync Calibartion */
#define CMC623_REG_CALVAL10 0x65

/* tcon output polarity */
#define CMC623_REG_TCONOUTPOL 0x68

/* tcon RGB configuration */
#define CMC623_REG_TCONRGB1 0x6c
#define CMC623_REG_TCONRGB2 0x6d
#define CMC623_REG_TCONRGB3 0x6e

/* Reg update */
#define CMC623_REG_REGMASK 0x28
#define CMC623_REG_SWRESET 0x09
#define CMC623_REG_RGBIFEN 0x26

typedef enum {
	CMC623_TYPE_LSI,
	CMC623_TYPE_FUJITSU,
	CMC623_TYPE_MAX,
} cmc623_type;

struct cmc623_register_set {
	uint16_t addr;
	uint16_t data;
};

static const unsigned char cmc623_default_plut[] = {
	 0x42, 0x47, 0x3E, 0x52, 0x42, 0x3F, 0x3A, 0x37, 0x3F
};

static const struct cmc623_register_set standard_ui_cabcon[] = {
	{0x0000,0x0000},  //BANK 0
	{0x0001,0x0070},  //SCR LABC CABC
	{0x002c,0x0fff},  //DNR bypass 0x003C
	{0x002d,0x1900},  //DNR bypass 0x0a08
	{0x002e,0x0000},  //DNR bypass 0x1010
	{0x002f,0x0fff},  //DNR bypass 0x0400
	{0x003A,0x0009},  //HDTR CS
	{0x003B,0x03ff},  //DE SHARPNESS
	{0x003C,0x0000},  //NOISE LEVEL
	{0x003F,0x0100},  //CS GAIN
	{0x0042,0x0000},  //DE TH (MAX DIFF)
	{0x0072,0x0000},  //CABC Dgain
	{0x0073,0x0000},
	{0x0074,0x0000},
	{0x0075,0x0000},
	{0x007C,0x0002},  //Dynamic LCD
	{0x00b4,0x5640},  //CABC PWM
	{0x00c8,0x0000},  //kb R  SCR
	{0x00c9,0x0000},  //gc R
	{0x00ca,0xffff},  //rm R
	{0x00cb,0xffff},  //yw R
	{0x00cc,0x0000},  //kb G
	{0x00cd,0xffff},  //gc G
	{0x00ce,0x0000},  //rm G
	{0x00cf,0xfff5},  //yw G
	{0x00d0,0x00ff},  //kb B
	{0x00d1,0x00ff},  //gc B
	{0x00d2,0x00ff},  //rm B
	{0x00d3,0x00ff},  //yw B
	{0x0000,0x0001},  //BANK 1
	{0x0021,0x3f00},  //GAMMA n1sc, 2217
	{0x0022,0x2003},
	{0x0023,0x2003},
	{0x0024,0x2003},
	{0x0025,0x2003},
	{0x0026,0x2003},
	{0x0027,0x2003},
	{0x0028,0x2003},
	{0x0029,0x2003},
	{0x002A,0x2003},
	{0x002B,0x2003},
	{0x002C,0x2003},
	{0x002D,0x2003},
	{0x002E,0x2003},
	{0x002F,0x2003},
	{0x0030,0x2003},
	{0x0031,0x2003},
	{0x0032,0x2003},
	{0x0033,0x2100},
	{0x0034,0xa40c},
	{0x0035,0xa40c},
	{0x0036,0x1c26},
	{0x0037,0x1652},
	{0x0038,0xFF00},
	{0x0020,0x0001},
	{0x0000,0x0000},  //BANK 0
	{0x0028,0x0000},  //Register Mask
	{0xffff,0xffff},
};

static const struct cmc623_register_set cmc623_regs_lsi[] = {
	/* select SFR Bank0 */
	{CMC623_REG_SELBANK, 0x0000},

	/* A stage configuration */
	{0x08, 0x0068},

	{CMC623_REG_DNRHDTROVE, 0x0020},
	{CMC623_REG_DITHEROFF, 0x0000},

	{0x0f, 0x0078},
	{0x0b, 0x0184},

	{CMC623_REG_INPUTIFCON, 0x0001},
	{CMC623_REG_HDRTCEOFF, 0x0000},
	{CMC623_REG_I2C, 0x1a07},
	{CMC623_REG_BSTAGE, 0x0708},
	{CMC623_REG_CABCCTRL, 0x0002},

	{0xB3, 0xFFFF},

	{CMC623_REG_PWMCTRL, 0xC000},

	{CMC623_REG_1280, 0x0500},
	{CMC623_REG_800, 0x0320},

	/* select SFR Bank1 */
	{CMC623_REG_SELBANK, 0x0001},

	/* B stage image size */
	{CMC623_REG_SCALERINPH, 0x0500},
	{CMC623_REG_SCALERINPV, 0x0320},
	{CMC623_REG_SCALEROUTH, 0x0500},
	{CMC623_REG_SCALEROUTV, 0x0320},

	/* EDRAM configuration */
	{CMC623_REG_EDRBFOUT40, 0x0280},

	{CMC623_REG_EDRAUTOREF, 0x008B},
	{CMC623_REG_EDRACPARAMTIM, 0x3226},

	/* tcon output polarity */
	{CMC623_REG_TCONOUTPOL, 0x0080},

	/* tcon RGB configuration */
	{CMC623_REG_TCONRGB1, 0x0330}, /* VLW, HLW*/
	{CMC623_REG_TCONRGB2, 0x0b02}, /* VBP, VFP*/
	{CMC623_REG_TCONRGB3, 0x4010}, /* HBP, HFP*/

	/* Reg update */
	{CMC623_REG_SELBANK, 0x0000},  /* select BANK0*/
	{CMC623_REG_REGMASK, 0x0000},

	{CMC623_REG_SWRESET, 0x0000},  /* SW reset*/
	{CMC623_REG_SWRESET, 0xffff},

	{CMC623_REG_RGBIFEN, 0x0001},  /* enable RGB IF*/
	{0xffff, 0xffff},
};

static const struct cmc623_register_set cmc623_regs_fujitsu[] = {
	/* select SFR Bank0 */
	{CMC623_REG_SELBANK, 0x0000},

	/* A stage configuration */
	{0x0C, 0x001F},
	{0x12, 0x0000},
	{0x16, 0x0000},
	{0x17, 0x0000},
	{0x18, 0x0000},
	{0x19, 0x0000},

	{CMC623_REG_DNRHDTROVE, 0x0020},
	{CMC623_REG_DITHEROFF, 0x0000},
	{CMC623_REG_CLKCONT, 0x221A},

	{0x0f, 0x0078},
	{0x0b, 0x0184},

	{CMC623_REG_INPUTIFCON, 0x0001},
	{CMC623_REG_HDRTCEOFF, 0x0000},
	{CMC623_REG_I2C, 0x1806},
	{CMC623_REG_BSTAGE, 0x0607},
	{CMC623_REG_CABCCTRL, 0x0002},

	{0xB3, 0xFFFF},

	{CMC623_REG_PWMCTRL, 0xC000},

	{CMC623_REG_1280, 0x0500},
	{CMC623_REG_800, 0x0320},

	/* select SFR Bank1 */
	{CMC623_REG_SELBANK, 0x0001},

	/* B stage image size */
	{CMC623_REG_SCALERINPH, 0x0500},
	{CMC623_REG_SCALERINPV, 0x0320},
	{CMC623_REG_SCALEROUTH, 0x0500},
	{CMC623_REG_SCALEROUTV, 0x0320},

	/* tcon output polarity */
	{CMC623_REG_TCONOUTPOL, 0x0080},

	/* for 76Mhz pclk */
	{CMC623_REG_TCONRGB1, 0x1230}, /* VLW, HLW*/
	{CMC623_REG_TCONRGB2, 0x4C06}, /* VBP, VFP*/
	{CMC623_REG_TCONRGB3, 0x4010}, /* HBP, HFP*/

	/* Reg update */
	{CMC623_REG_SELBANK, 0x0000},  /* select BANK0*/
	{CMC623_REG_REGMASK, 0x0000},

	{CMC623_REG_SWRESET, 0x0000},  /* SW reset*/
	{CMC623_REG_SWRESET, 0xffff},

	{CMC623_REG_RGBIFEN, 0x0001},  /* enable RGB IF*/
	{0xffff, 0xffff},
};

#endif  /* _SAMSUNG_CMC6230R_H_ */
