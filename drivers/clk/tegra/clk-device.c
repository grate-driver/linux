// SPDX-License-Identifier: GPL-2.0-only

#include <linux/clk.h>
#include <linux/clk-provider.h>
#include <linux/mutex.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>
#include <linux/pm_domain.h>
#include <linux/pm_opp.h>
#include <linux/pm_runtime.h>
#include <linux/slab.h>

#include <soc/tegra/common.h>

#include "clk.h"

/*
 * This driver manages performance state of the core power domain for the
 * independent PLLs and system clocks.  We created a virtual clock device
 * for such clocks, see tegra_clk_register().
 */

struct tegra_clk_device {
	struct notifier_block clk_nb;
	struct device *dev;
	struct clk_hw *hw;
	struct mutex lock;
};

static int tegra_clock_set_pd_state(struct tegra_clk_device *clk_dev,
				    unsigned long rate)
{
	struct device *dev = clk_dev->dev;
	struct dev_pm_opp *opp;
	unsigned int pstate;

	opp = dev_pm_opp_find_freq_ceil(dev, &rate);
	if (opp == ERR_PTR(-ERANGE)) {
		dev_dbg(dev, "failed to find ceil OPP for %luHz\n", rate);
		opp = dev_pm_opp_find_freq_floor(dev, &rate);
	}

	if (IS_ERR(opp)) {
		dev_err(dev, "failed to find OPP for %luHz: %pe\n", rate, opp);
		return PTR_ERR(opp);
	}

	pstate = dev_pm_opp_get_required_pstate(opp, 0);
	dev_pm_opp_put(opp);

	return dev_pm_genpd_set_performance_state(dev, pstate);
}

static int tegra_clock_change_notify(struct notifier_block *nb,
				     unsigned long msg, void *data)
{
	struct clk_notifier_data *cnd = data;
	struct tegra_clk_device *clk_dev;
	int err = 0;

	clk_dev = container_of(nb, struct tegra_clk_device, clk_nb);

	mutex_lock(&clk_dev->lock);
	switch (msg) {
	case PRE_RATE_CHANGE:
		if (cnd->new_rate > cnd->old_rate)
			err = tegra_clock_set_pd_state(clk_dev, cnd->new_rate);
		break;

	case ABORT_RATE_CHANGE:
		err = tegra_clock_set_pd_state(clk_dev, cnd->old_rate);
		break;

	case POST_RATE_CHANGE:
		if (cnd->new_rate < cnd->old_rate)
			err = tegra_clock_set_pd_state(clk_dev, cnd->new_rate);
		break;

	default:
		break;
	}
	mutex_unlock(&clk_dev->lock);

	return notifier_from_errno(err);
}

static int tegra_clock_sync_pd_state(struct tegra_clk_device *clk_dev)
{
	unsigned long rate;
	int ret = 0;

	mutex_lock(&clk_dev->lock);

	if (!pm_runtime_status_suspended(clk_dev->dev)) {
		rate = clk_hw_get_rate(clk_dev->hw);
		ret = tegra_clock_set_pd_state(clk_dev, rate);
	}

	mutex_unlock(&clk_dev->lock);

	return ret;
}

static int tegra_clock_probe(struct platform_device *pdev)
{
	struct tegra_clk_device *clk_dev;
	struct device *dev = &pdev->dev;
	struct clk *clk;
	int err;

	if (!dev->pm_domain)
		return -EINVAL;

	clk_dev = devm_kzalloc(dev, sizeof(*clk_dev), GFP_KERNEL);
	if (!clk_dev)
		return -ENOMEM;

	clk = devm_clk_get(dev, NULL);
	if (IS_ERR(clk))
		return PTR_ERR(clk);

	clk_dev->dev = dev;
	clk_dev->hw = __clk_get_hw(clk);
	clk_dev->clk_nb.notifier_call = tegra_clock_change_notify;
	mutex_init(&clk_dev->lock);

	platform_set_drvdata(pdev, clk_dev);

	err = devm_tegra_core_dev_init_opp_table_simple(dev);
	if (err)
		return err;

	err = clk_notifier_register(clk, &clk_dev->clk_nb);
	if (err) {
		dev_err(dev, "failed to register clk notifier: %d\n", err);
		return err;
	}

	/*
	 * The driver is attaching to a potentially active/resumed clock, hence
	 * we need to sync the power domain performance state in a accordance to
	 * the clock rate if clock is resumed.
	 */
	err = tegra_clock_sync_pd_state(clk_dev);
	if (err)
		goto unreg_clk;

	return 0;

unreg_clk:
	clk_notifier_unregister(clk, &clk_dev->clk_nb);

	return err;
}

static __maybe_unused int tegra_clock_pm_suspend(struct device *dev)
{
	struct tegra_clk_device *clk_dev = dev_get_drvdata(dev);

	/*
	 * Power management of the clock is entangled with the Tegra PMC
	 * GENPD because PMC driver enables/disables clocks for toggling
	 * of the PD's on/off state.
	 *
	 * The PMC GENPD is resumed in NOIRQ phase, before RPM of the clocks
	 * becomes available, hence PMC can't use clocks at the early resume
	 * phase if RPM is involved. For example when 3d clock is enabled,
	 * it may enable the parent PLL clock that needs to be RPM-resumed.
	 *
	 * Secondly, the PLL clocks may be enabled by the low level suspend
	 * code, so we need to assume that PLL is in enabled state during
	 * suspend.
	 *
	 * We will keep PLLs and system clock resumed during suspend time.
	 * All PLLs on all SoCs are low power and system clock is always-on,
	 * so practically not much is changed here.
	 */

	return clk_prepare(clk_dev->hw->clk);
}

static __maybe_unused int tegra_clock_pm_resume(struct device *dev)
{
	struct tegra_clk_device *clk_dev = dev_get_drvdata(dev);

	clk_unprepare(clk_dev->hw->clk);

	return 0;
}

static void tegra_clock_shutdown(struct platform_device *pdev)
{
	struct tegra_clk_device *clk_dev = platform_get_drvdata(pdev);

	clk_prepare(clk_dev->hw->clk);
}

static const struct dev_pm_ops tegra_clock_pm = {
	SET_SYSTEM_SLEEP_PM_OPS(tegra_clock_pm_suspend,
				tegra_clock_pm_resume)
};

static const struct of_device_id tegra_clock_match[] = {
	{ .compatible = "nvidia,tegra20-sclk" },
	{ .compatible = "nvidia,tegra30-sclk" },
	{ .compatible = "nvidia,tegra30-pllc" },
	{ .compatible = "nvidia,tegra30-plle" },
	{ .compatible = "nvidia,tegra30-pllm" },
	{ }
};

static struct platform_driver tegra_clock_driver = {
	.driver = {
		.name = "tegra-clock",
		.of_match_table = tegra_clock_match,
		.pm = &tegra_clock_pm,
		.suppress_bind_attrs = true,
	},
	.probe = tegra_clock_probe,
	.shutdown = tegra_clock_shutdown,
};
builtin_platform_driver(tegra_clock_driver);
