/*
 * linux/arch/arm/mach-tegra/pinmux.c
 *
 * Copyright (C) 2010 Google, Inc.
 * Copyright (C) 2011 NVIDIA Corporation.
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#include <linux/init.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/of.h>
#include <linux/spinlock.h>
#include <linux/syscore_ops.h>
#include <linux/io.h>

#include "gpio-names.h"
#include "iomap.h"
#include "pinmux.h"

#if defined(CONFIG_ARCH_TEGRA_2x_SOC)
#include "pinmux-t2-tables.c"
#else
#include "pinmux-t3-tables.c"
#endif

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

static DEFINE_SPINLOCK(mux_lock);

static const char *pingroup_name(enum tegra_pingroup pg)
{
	if (pg < 0 || pg >=  TEGRA_MAX_PINGROUP)
		return "<UNKNOWN>";

	return pingroups[pg].name;
}

static const char *func_name(enum tegra_mux_func func)
{
	if (func == TEGRA_MUX_RSVD1)
		return "RSVD1";

	if (func == TEGRA_MUX_RSVD2)
		return "RSVD2";

	if (func == TEGRA_MUX_RSVD3)
		return "RSVD3";

	if (func == TEGRA_MUX_RSVD4)
		return "RSVD4";

	if (func == TEGRA_MUX_INVALID)
		return "INVALID";

	if (func < 0 || func >=  TEGRA_MAX_MUX)
		return "<UNKNOWN>";

	return tegra_mux_names[func];
}


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

#if !defined(CONFIG_ARCH_TEGRA_2x_SOC)
static const char *lock_name(unsigned long val)
{
	switch (val) {
	case TEGRA_PIN_LOCK_DEFAULT:
		return "LOCK_DEFUALT";

	case TEGRA_PIN_LOCK_DISABLE:
		return "LOCK_DISABLE";

	case TEGRA_PIN_LOCK_ENABLE:
		return "LOCK_ENABLE";
	default:
		return "LOCK_DEFAULT";
	}
}

static const char *od_name(unsigned long val)
{
	switch (val) {
	case TEGRA_PIN_OD_DEFAULT:
		return "OD_DEFAULT";

	case TEGRA_PIN_OD_DISABLE:
		return "OD_DISABLE";

	case TEGRA_PIN_OD_ENABLE:
		return "OD_ENABLE";
	default:
		return "OD_DEFAULT";
	}
}

static const char *ioreset_name(unsigned long val)
{
	switch (val) {
	case TEGRA_PIN_IO_RESET_DEFAULT:
		return "IO_RESET_DEFAULT";

	case TEGRA_PIN_IO_RESET_DISABLE:
		return "IO_RESET_DISABLE";

	case TEGRA_PIN_IO_RESET_ENABLE:
		return "IO_RESET_ENABLE";
	default:
		return "IO_RESET_DEFAULT";
	}
}
#endif

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

// static inline unsigned long pg_readl(unsigned long offset)
// {
// 	return readl(IO_TO_VIRT(TEGRA_APB_MISC_BASE) + offset);
// }
//
// static inline void pg_writel(unsigned long value, unsigned long offset)
// {
// 	writel(value, IO_TO_VIRT(TEGRA_APB_MISC_BASE) + offset);
// }

static int tegra_pinmux_set_func(const struct tegra_pingroup_config *config)
{
	int mux = -1;
	int i;
	int find = 0;
	unsigned long reg;
	unsigned long flags;
	enum tegra_pingroup pg = config->pingroup;
	enum tegra_mux_func func = config->func;

	if (pg < 0 || pg >=  TEGRA_MAX_PINGROUP)
		return -ERANGE;

	if (pingroups[pg].mux_reg <= 0)
		return -EINVAL;

	if (func == TEGRA_MUX_INVALID) {
		pr_err("The pingroup %s is not recommended for option %s\n",
				pingroup_name(pg), func_name(func));
		WARN_ON(1);
		return -EINVAL;
	}

	if (func < 0)
		return -ERANGE;

	if (func == TEGRA_MUX_SAFE)
		func = pingroups[pg].func_safe;

	if (func & TEGRA_MUX_RSVD) {
		for (i = 0; i < 4; i++) {
			if (pingroups[pg].funcs[i] & TEGRA_MUX_RSVD)
				mux = i;

			if (pingroups[pg].funcs[i] == func) {
				mux = i;
				find = 1;
				break;
			}
		}
	} else {
		for (i = 0; i < 4; i++) {
			if (pingroups[pg].funcs[i] == func) {
				mux = i;
				find = 1;
				break;
			}
		}
	}

	if (mux < 0) {
		pr_err("The pingroup %s is not supported option %s\n",
			pingroup_name(pg), func_name(func));
		WARN_ON(1);
		return -EINVAL;
	}

	if (!find)
		pr_warn("The pingroup %s was configured to %s instead of %s\n",
			pingroup_name(pg), func_name(pingroups[pg].funcs[mux]),
			func_name(func));

	spin_lock_irqsave(&mux_lock, flags);

	reg = pg_readl(pingroups[pg].mux_reg);
	reg &= ~(0x3 << pingroups[pg].mux_bit);
	reg |= mux << pingroups[pg].mux_bit;
#if defined(TEGRA_PINMUX_HAS_IO_DIRECTION)
	reg &= ~(0x1 << 5);
	reg |= ((config->io & 0x1) << 5);
#endif
	pg_writel(reg, pingroups[pg].mux_reg);

	spin_unlock_irqrestore(&mux_lock, flags);

	return 0;
}

int tegra_pinmux_get_func(enum tegra_pingroup pg)
{
	int mux = -1;
	unsigned long reg;
	unsigned long flags;

	if (pg < 0 || pg >=  TEGRA_MAX_PINGROUP)
		return -ERANGE;

	if (pingroups[pg].mux_reg <= 0)
		return -EINVAL;

	spin_lock_irqsave(&mux_lock, flags);

	reg = pg_readl(pingroups[pg].mux_reg);
	mux = (reg >> pingroups[pg].mux_bit) & 0x3;

	spin_unlock_irqrestore(&mux_lock, flags);

	return mux;
}

int tegra_pinmux_set_tristate(enum tegra_pingroup pg,
	enum tegra_tristate tristate)
{
	unsigned long reg;
	unsigned long flags;

	if (pg < 0 || pg >=  TEGRA_MAX_PINGROUP)
		return -ERANGE;

	if (pingroups[pg].tri_reg <= 0)
		return -EINVAL;

	spin_lock_irqsave(&mux_lock, flags);

	reg = pg_readl(pingroups[pg].tri_reg);
	reg &= ~(0x1 << pingroups[pg].tri_bit);
	if (tristate)
		reg |= 1 << pingroups[pg].tri_bit;
	pg_writel(reg, pingroups[pg].tri_reg);

	spin_unlock_irqrestore(&mux_lock, flags);

	return 0;
}

int tegra_pinmux_set_io(enum tegra_pingroup pg,
	enum tegra_pin_io input)
{
#if defined(TEGRA_PINMUX_HAS_IO_DIRECTION)
	unsigned long io;

	if (pg < 0 || pg >=  TEGRA_MAX_PINGROUP)
		return -ERANGE;

	io = pg_readl(pingroups[pg].mux_reg);
	if (input)
		io |= 0x20;
	else
		io &= ~(1 << 5);
	pg_writel(io, pingroups[pg].mux_reg);
#endif
	return 0;
}
EXPORT_SYMBOL_GPL(tegra_pinmux_set_io);

#if !defined(CONFIG_ARCH_TEGRA_2x_SOC)
static int tegra_pinmux_set_lock(enum tegra_pingroup pg,
	enum tegra_pin_lock lock)
{
	unsigned long reg;
	unsigned long flags;

	if (pg < 0 || pg >=  TEGRA_MAX_PINGROUP)
		return -ERANGE;

	if (pingroups[pg].mux_reg <= 0)
		return -EINVAL;

	if ((lock == TEGRA_PIN_LOCK_DEFAULT) || (pingroups[pg].lock_bit < 0))
		return 0;

	spin_lock_irqsave(&mux_lock, flags);

	reg = pg_readl(pingroups[pg].mux_reg);
	reg &= ~(0x1 << pingroups[pg].lock_bit);
	if (lock == TEGRA_PIN_LOCK_ENABLE)
		reg |= (0x1 << pingroups[pg].lock_bit);

	pg_writel(reg, pingroups[pg].mux_reg);

	spin_unlock_irqrestore(&mux_lock, flags);
	return 0;
}

static int tegra_pinmux_set_od(enum tegra_pingroup pg,
	enum tegra_pin_od od)
{
	unsigned long reg;
	unsigned long flags;

	if (pg < 0 || pg >=  TEGRA_MAX_PINGROUP)
		return -ERANGE;

	if (pingroups[pg].mux_reg <= 0)
		return -EINVAL;

	if ((od == TEGRA_PIN_OD_DEFAULT) || (pingroups[pg].od_bit < 0))
		return 0;

	spin_lock_irqsave(&mux_lock, flags);

	reg = pg_readl(pingroups[pg].mux_reg);
	reg &= ~(0x1 << pingroups[pg].od_bit);
	if (od == TEGRA_PIN_OD_ENABLE)
		reg |= 1 << pingroups[pg].od_bit;

	pg_writel(reg, pingroups[pg].mux_reg);

	spin_unlock_irqrestore(&mux_lock, flags);

	return 0;
}

static int tegra_pinmux_set_ioreset(enum tegra_pingroup pg,
	enum tegra_pin_ioreset ioreset)
{
	unsigned long reg;
	unsigned long flags;

	if (pg < 0 || pg >=  TEGRA_MAX_PINGROUP)
		return -ERANGE;

	if (pingroups[pg].mux_reg <= 0)
		return -EINVAL;

	if ((ioreset == TEGRA_PIN_IO_RESET_DEFAULT) || (pingroups[pg].ioreset_bit < 0))
		return 0;

	spin_lock_irqsave(&mux_lock, flags);

	reg = pg_readl(pingroups[pg].mux_reg);
	reg &= ~(0x1 << pingroups[pg].ioreset_bit);
	if (ioreset == TEGRA_PIN_IO_RESET_ENABLE)
		reg |= 1 << pingroups[pg].ioreset_bit;

	pg_writel(reg, pingroups[pg].mux_reg);

	spin_unlock_irqrestore(&mux_lock, flags);

	return 0;
}
#endif

int tegra_pinmux_set_pullupdown(enum tegra_pingroup pg,
	enum tegra_pullupdown pupd)
{
	unsigned long reg;
	unsigned long flags;

	if (pg < 0 || pg >=  TEGRA_MAX_PINGROUP)
		return -ERANGE;

	if (pingroups[pg].pupd_reg <= 0)
		return -EINVAL;

	if (pupd != TEGRA_PUPD_NORMAL &&
	    pupd != TEGRA_PUPD_PULL_DOWN &&
	    pupd != TEGRA_PUPD_PULL_UP)
		return -EINVAL;


	spin_lock_irqsave(&mux_lock, flags);

	reg = pg_readl(pingroups[pg].pupd_reg);
	reg &= ~(0x3 << pingroups[pg].pupd_bit);
	reg |= pupd << pingroups[pg].pupd_bit;
	pg_writel(reg, pingroups[pg].pupd_reg);

	spin_unlock_irqrestore(&mux_lock, flags);

	return 0;
}

static void tegra_pinmux_config_pingroup(const struct tegra_pingroup_config *config)
{
	enum tegra_pingroup pingroup = config->pingroup;
	enum tegra_mux_func func     = config->func;
	enum tegra_pullupdown pupd   = config->pupd;
	enum tegra_tristate tristate = config->tristate;
#if !defined(CONFIG_ARCH_TEGRA_2x_SOC)
	enum tegra_pin_lock lock     = config->lock;
	enum tegra_pin_od od         = config->od;
	enum tegra_pin_ioreset ioreset = config->ioreset;
#endif
	int err;

	if (pingroups[pingroup].mux_reg > 0) {
		err = tegra_pinmux_set_func(config);
		if (err < 0)
			pr_err("pinmux: can't set pingroup %s func to %s: %d\n",
			       pingroup_name(pingroup), func_name(func), err);
	}

	if (pingroups[pingroup].pupd_reg > 0) {
		err = tegra_pinmux_set_pullupdown(pingroup, pupd);
		if (err < 0)
			pr_err("pinmux: can't set pingroup %s pullupdown to %s: %d\n",
			       pingroup_name(pingroup), pupd_name(pupd), err);
	}

	if (pingroups[pingroup].tri_reg > 0) {
		err = tegra_pinmux_set_tristate(pingroup, tristate);
		if (err < 0)
			pr_err("pinmux: can't set pingroup %s tristate to %s: %d\n",
			       pingroup_name(pingroup), tri_name(func), err);
	}

#if !defined(CONFIG_ARCH_TEGRA_2x_SOC)
	if (pingroups[pingroup].mux_reg > 0) {
		err = tegra_pinmux_set_lock(pingroup, lock);
		if (err < 0)
			pr_err("pinmux: can't set pingroup %s lock to %s: %d\n",
			       pingroup_name(pingroup), lock_name(func), err);
	}

	if (pingroups[pingroup].mux_reg > 0) {
		err = tegra_pinmux_set_od(pingroup, od);
		if (err < 0)
			pr_err("pinmux: can't set pingroup %s od to %s: %d\n",
			       pingroup_name(pingroup), od_name(func), err);
	}

	if (pingroups[pingroup].mux_reg > 0) {
		err = tegra_pinmux_set_ioreset(pingroup, ioreset);
		if (err < 0)
			pr_err("pinmux: can't set pingroup %s ioreset to %s: %d\n",
			       pingroup_name(pingroup), ioreset_name(func), err);
	}
#endif
}

void tegra_pinmux_config_table(const struct tegra_pingroup_config *config, int len)
{
	int i;

	for (i = 0; i < len; i++)
		tegra_pinmux_config_pingroup(&config[i]);
}
EXPORT_SYMBOL(tegra_pinmux_config_table);

static const char *drive_pinmux_name(enum tegra_drive_pingroup pg)
{
	if (pg < 0 || pg >=  TEGRA_MAX_DRIVE_PINGROUP)
		return "<UNKNOWN>";

	return drive_pingroups[pg].name;
}

static const char *enable_name(unsigned long val)
{
	return val ? "ENABLE" : "DISABLE";
}

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

static int tegra_drive_pinmux_set_hsm(enum tegra_drive_pingroup pg,
	enum tegra_hsm hsm)
{
	unsigned long flags;
	u32 reg;
	if (pg < 0 || pg >=  TEGRA_MAX_DRIVE_PINGROUP)
		return -ERANGE;

	if (hsm != TEGRA_HSM_ENABLE && hsm != TEGRA_HSM_DISABLE)
		return -EINVAL;

	spin_lock_irqsave(&mux_lock, flags);

	reg = pg_readl(drive_pingroups[pg].reg);
	if (hsm == TEGRA_HSM_ENABLE)
		reg |= (1 << 2);
	else
		reg &= ~(1 << 2);
	pg_writel(reg, drive_pingroups[pg].reg);

	spin_unlock_irqrestore(&mux_lock, flags);

	return 0;
}

static int tegra_drive_pinmux_set_schmitt(enum tegra_drive_pingroup pg,
	enum tegra_schmitt schmitt)
{
	unsigned long flags;
	u32 reg;
	if (pg < 0 || pg >=  TEGRA_MAX_DRIVE_PINGROUP)
		return -ERANGE;

	if (schmitt != TEGRA_SCHMITT_ENABLE && schmitt != TEGRA_SCHMITT_DISABLE)
		return -EINVAL;

	spin_lock_irqsave(&mux_lock, flags);

	reg = pg_readl(drive_pingroups[pg].reg);
	if (schmitt == TEGRA_SCHMITT_ENABLE)
		reg |= (1 << 3);
	else
		reg &= ~(1 << 3);
	pg_writel(reg, drive_pingroups[pg].reg);

	spin_unlock_irqrestore(&mux_lock, flags);

	return 0;
}

static int tegra_drive_pinmux_set_drive(enum tegra_drive_pingroup pg,
	enum tegra_drive drive)
{
	unsigned long flags;
	u32 reg;
	if (pg < 0 || pg >=  TEGRA_MAX_DRIVE_PINGROUP)
		return -ERANGE;

	if (drive < 0 || drive >= TEGRA_MAX_DRIVE)
		return -EINVAL;

	spin_lock_irqsave(&mux_lock, flags);

	reg = pg_readl(drive_pingroups[pg].reg);
	reg &= ~(0x3 << 4);
	reg |= drive << 4;
	pg_writel(reg, drive_pingroups[pg].reg);

	spin_unlock_irqrestore(&mux_lock, flags);

	return 0;
}

static int tegra_drive_pinmux_set_pull_down(enum tegra_drive_pingroup pg,
	enum tegra_pull_strength pull_down)
{
	unsigned long flags;
	u32 reg;

	if (pg < 0 || pg >=  TEGRA_MAX_DRIVE_PINGROUP)
		return -ERANGE;

	if (pull_down < 0 || pull_down >= TEGRA_MAX_PULL)
		return -EINVAL;

	spin_lock_irqsave(&mux_lock, flags);

	reg = pg_readl(drive_pingroups[pg].reg);
	reg &= ~(drive_pingroups[pg].drvdown_mask <<
		drive_pingroups[pg].drvdown_offset);
	reg |= pull_down << drive_pingroups[pg].drvdown_offset;
	pg_writel(reg, drive_pingroups[pg].reg);

	spin_unlock_irqrestore(&mux_lock, flags);

	return 0;
}

static int tegra_drive_pinmux_set_pull_up(enum tegra_drive_pingroup pg,
	enum tegra_pull_strength pull_up)
{
	unsigned long flags;
	u32 reg;

	if (pg < 0 || pg >=  TEGRA_MAX_DRIVE_PINGROUP)
		return -ERANGE;

	if (pull_up < 0 || pull_up >= TEGRA_MAX_PULL)
		return -EINVAL;

	spin_lock_irqsave(&mux_lock, flags);

	reg = pg_readl(drive_pingroups[pg].reg);
	reg &= ~(drive_pingroups[pg].drvup_mask <<
		drive_pingroups[pg].drvup_offset);
	reg |= pull_up << drive_pingroups[pg].drvup_offset;
	pg_writel(reg, drive_pingroups[pg].reg);

	spin_unlock_irqrestore(&mux_lock, flags);

	return 0;
}

static int tegra_drive_pinmux_set_slew_rising(enum tegra_drive_pingroup pg,
	enum tegra_slew slew_rising)
{
	unsigned long flags;
	u32 reg;
	if (pg < 0 || pg >=  TEGRA_MAX_DRIVE_PINGROUP)
		return -ERANGE;

	if (slew_rising < 0 || slew_rising >= TEGRA_MAX_SLEW)
		return -EINVAL;

	spin_lock_irqsave(&mux_lock, flags);

	reg = pg_readl(drive_pingroups[pg].reg);
	reg &= ~(drive_pingroups[pg].slewrise_mask <<
		drive_pingroups[pg].slewrise_offset);
	reg |= slew_rising << drive_pingroups[pg].slewrise_offset;
	pg_writel(reg, drive_pingroups[pg].reg);

	spin_unlock_irqrestore(&mux_lock, flags);

	return 0;
}

static int tegra_drive_pinmux_set_slew_falling(enum tegra_drive_pingroup pg,
	enum tegra_slew slew_falling)
{
	unsigned long flags;
	u32 reg;
	if (pg < 0 || pg >=  TEGRA_MAX_DRIVE_PINGROUP)
		return -ERANGE;

	if (slew_falling < 0 || slew_falling >= TEGRA_MAX_SLEW)
		return -EINVAL;

	spin_lock_irqsave(&mux_lock, flags);

	reg = pg_readl(drive_pingroups[pg].reg);
	reg &= ~(drive_pingroups[pg].slewfall_mask <<
		drive_pingroups[pg].slewfall_offset);
	reg |= slew_falling << drive_pingroups[pg].slewfall_offset;
	pg_writel(reg, drive_pingroups[pg].reg);

	spin_unlock_irqrestore(&mux_lock, flags);

	return 0;
}

static void tegra_drive_pinmux_config_pingroup(enum tegra_drive_pingroup pingroup,
					  enum tegra_hsm hsm,
					  enum tegra_schmitt schmitt,
					  enum tegra_drive drive,
					  enum tegra_pull_strength pull_down,
					  enum tegra_pull_strength pull_up,
					  enum tegra_slew slew_rising,
					  enum tegra_slew slew_falling)
{
	int err;

	err = tegra_drive_pinmux_set_hsm(pingroup, hsm);
	if (err < 0)
		pr_err("pinmux: can't set pingroup %s hsm to %s: %d\n",
			drive_pinmux_name(pingroup),
			enable_name(hsm), err);

	err = tegra_drive_pinmux_set_schmitt(pingroup, schmitt);
	if (err < 0)
		pr_err("pinmux: can't set pingroup %s schmitt to %s: %d\n",
			drive_pinmux_name(pingroup),
			enable_name(schmitt), err);

	err = tegra_drive_pinmux_set_drive(pingroup, drive);
	if (err < 0)
		pr_err("pinmux: can't set pingroup %s drive to %s: %d\n",
			drive_pinmux_name(pingroup),
			drive_name(drive), err);

	err = tegra_drive_pinmux_set_pull_down(pingroup, pull_down);
	if (err < 0)
		pr_err("pinmux: can't set pingroup %s pull down to %d: %d\n",
			drive_pinmux_name(pingroup),
			pull_down, err);

	err = tegra_drive_pinmux_set_pull_up(pingroup, pull_up);
	if (err < 0)
		pr_err("pinmux: can't set pingroup %s pull up to %d: %d\n",
			drive_pinmux_name(pingroup),
			pull_up, err);

	err = tegra_drive_pinmux_set_slew_rising(pingroup, slew_rising);
	if (err < 0)
		pr_err("pinmux: can't set pingroup %s rising slew to %s: %d\n",
			drive_pinmux_name(pingroup),
			slew_name(slew_rising), err);

	err = tegra_drive_pinmux_set_slew_falling(pingroup, slew_falling);
	if (err < 0)
		pr_err("pinmux: can't set pingroup %s falling slew to %s: %d\n",
			drive_pinmux_name(pingroup),
			slew_name(slew_falling), err);
}

void tegra_drive_pinmux_config_table(struct tegra_drive_pingroup_config *config,
	int len)
{
	int i;

	for (i = 0; i < len; i++)
		tegra_drive_pinmux_config_pingroup(config[i].pingroup,
						     config[i].hsm,
						     config[i].schmitt,
						     config[i].drive,
						     config[i].pull_down,
						     config[i].pull_up,
						     config[i].slew_rising,
						     config[i].slew_falling);
}

void tegra_pinmux_set_safe_pinmux_table(const struct tegra_pingroup_config *config,
	int len)
{
	int i;
	struct tegra_pingroup_config c;

	for (i = 0; i < len; i++) {
		int err;
		c = config[i];
		if (c.pingroup < 0 || c.pingroup >= TEGRA_MAX_PINGROUP) {
			WARN_ON(1);
			continue;
		}
		c.func = pingroups[c.pingroup].func_safe;
		err = tegra_pinmux_set_func(&c);
		if (err < 0)
			pr_err("%s: tegra_pinmux_set_func returned %d setting "
			       "%s to %s\n", __func__, err,
			       pingroup_name(c.pingroup), func_name(c.func));
	}
}

void tegra_pinmux_config_pinmux_table(const struct tegra_pingroup_config *config,
	int len)
{
	int i;

	for (i = 0; i < len; i++) {
		int err;
		if (config[i].pingroup < 0 ||
		    config[i].pingroup >= TEGRA_MAX_PINGROUP) {
			WARN_ON(1);
			continue;
		}
		err = tegra_pinmux_set_func(&config[i]);
		if (err < 0)
			pr_err("%s: tegra_pinmux_set_func returned %d setting "
			       "%s to %s\n", __func__, err,
			       pingroup_name(config[i].pingroup),
			       func_name(config[i].func));
	}
}

void tegra_pinmux_config_tristate_table(const struct tegra_pingroup_config *config,
	int len, enum tegra_tristate tristate)
{
	int i;
	int err;
	enum tegra_pingroup pingroup;

	for (i = 0; i < len; i++) {
		pingroup = config[i].pingroup;
		if (pingroups[pingroup].tri_reg > 0) {
			err = tegra_pinmux_set_tristate(pingroup, tristate);
			if (err < 0)
				pr_err("pinmux: can't set pingroup %s tristate"
					" to %s: %d\n",	pingroup_name(pingroup),
					tri_name(tristate), err);
		}
	}
}

void tegra_pinmux_config_pullupdown_table(const struct tegra_pingroup_config *config,
	int len, enum tegra_pullupdown pupd)
{
	int i;
	int err;
	enum tegra_pingroup pingroup;

	for (i = 0; i < len; i++) {
		pingroup = config[i].pingroup;
		if (pingroups[pingroup].pupd_reg > 0) {
			err = tegra_pinmux_set_pullupdown(pingroup, pupd);
			if (err < 0)
				pr_err("pinmux: can't set pingroup %s pullupdown"
					" to %s: %d\n",	pingroup_name(pingroup),
					pupd_name(pupd), err);
		}
	}
}

#ifdef	CONFIG_DEBUG_FS

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
#ifdef CONFIG_ARCH_TEGRA_2x_SOC
	if (!of_machine_is_compatible("nvidia,tegra20"))
		return 0;
#else
	if (!of_machine_is_compatible("nvidia,tegra30"))
		return 0;
#endif
	(void) debugfs_create_file("tegra_pinmux", S_IRUGO,
					NULL, NULL, &debug_fops);
	(void) debugfs_create_file("tegra_pinmux_drive", S_IRUGO,
					NULL, NULL, &debug_drive_fops);
	return 0;
}
late_initcall(tegra_pinmux_debuginit);
#endif
