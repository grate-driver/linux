// SPDX-License-Identifier: GPL-2.0+
/*
 * NVIDIA Tegra SoC Core Power Domain Driver
 */

#include <linux/mutex.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>
#include <linux/pm_domain.h>
#include <linux/pm_opp.h>
#include <linux/slab.h>

#include <soc/tegra/common.h>

static struct lock_class_key tegra_core_domain_lock_class;
static bool tegra_core_domain_state_synced;
static DEFINE_MUTEX(tegra_core_lock);

static int tegra_genpd_set_performance_state(struct generic_pm_domain *genpd,
					     unsigned int level)
{
	struct dev_pm_opp *opp;
	int err;

	opp = dev_pm_opp_find_level_ceil(&genpd->dev, &level);
	if (IS_ERR(opp)) {
		dev_err(&genpd->dev, "failed to find OPP for level %u: %pe\n",
			level, opp);
		return PTR_ERR(opp);
	}

	mutex_lock(&tegra_core_lock);
	err = dev_pm_opp_set_voltage(&genpd->dev, opp);
	mutex_unlock(&tegra_core_lock);

	dev_pm_opp_put(opp);

	if (err) {
		dev_err(&genpd->dev, "failed to set voltage to %duV: %d\n",
			level, err);
		return err;
	}

	return 0;
}

static unsigned int
tegra_genpd_opp_to_performance_state(struct generic_pm_domain *genpd,
				     struct dev_pm_opp *opp)
{
	return dev_pm_opp_get_level(opp);
}

static int tegra_core_domain_probe(struct platform_device *pdev)
{
	struct generic_pm_domain *genpd;
	struct opp_table *opp_table;
	const char *rname = "power";
	int err;

	genpd = devm_kzalloc(&pdev->dev, sizeof(*genpd), GFP_KERNEL);
	if (!genpd)
		return -ENOMEM;

	genpd->name = pdev->dev.of_node->name;
	genpd->set_performance_state = tegra_genpd_set_performance_state;
	genpd->opp_to_performance_state = tegra_genpd_opp_to_performance_state;

	opp_table = devm_pm_opp_set_regulators(&pdev->dev, &rname, 1);
	if (IS_ERR(opp_table))
		return dev_err_probe(&pdev->dev, PTR_ERR(opp_table),
				     "failed to set OPP regulator\n");

	err = pm_genpd_init(genpd, NULL, false);
	if (err) {
		dev_err(&pdev->dev, "failed to init genpd: %d\n", err);
		return err;
	}

	/*
	 * We have a "PMC -> Core" hierarchy of the power domains where
	 * PMC needs to resume and change performance (voltage) of the
	 * Core domain from the PMC GENPD on/off callbacks, hence we need
	 * to annotate the lock in order to remove confusion from the
	 * lockdep checker when a nested access happens.
	 */
	lockdep_set_class(&genpd->mlock, &tegra_core_domain_lock_class);

	err = of_genpd_add_provider_simple(pdev->dev.of_node, genpd);
	if (err) {
		dev_err(&pdev->dev, "failed to add genpd: %d\n", err);
		goto remove_genpd;
	}

	return 0;

remove_genpd:
	pm_genpd_remove(genpd);

	return err;
}

bool tegra_soc_core_domain_state_synced(void)
{
	return tegra_core_domain_state_synced;
}

static void tegra_core_domain_sync_state(struct device *dev)
{
	tegra_core_domain_state_synced = true;

	mutex_lock(&tegra_core_lock);
	dev_pm_opp_sync_regulators(dev);
	mutex_unlock(&tegra_core_lock);
}

static const struct of_device_id tegra_core_domain_match[] = {
	{ .compatible = "nvidia,tegra20-core-domain", },
	{ .compatible = "nvidia,tegra30-core-domain", },
	{ }
};

static struct platform_driver tegra_core_domain_driver = {
	.driver = {
		.name = "tegra-core-power",
		.of_match_table = tegra_core_domain_match,
		.suppress_bind_attrs = true,
		.sync_state = tegra_core_domain_sync_state,
	},
	.probe = tegra_core_domain_probe,
};
builtin_platform_driver(tegra_core_domain_driver);
