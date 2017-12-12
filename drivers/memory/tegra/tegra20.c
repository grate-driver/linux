/*
 * Copyright (C) 2012 NVIDIA CORPORATION.  All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include "mc.h"

static const struct tegra_mc_client tegra20_mc_clients[] = {
	{ .name = "display0a" },
	{ .name = "display0ab" },
	{ .name = "display0b" },
	{ .name = "display0bb" },
	{ .name = "display0c" },
	{ .name = "display0cb" },
	{ .name = "display1b" },
	{ .name = "display1bb" },
	{ .name = "eppup" },
	{ .name = "g2pr" },
	{ .name = "g2sr" },
	{ .name = "mpeunifbr" },
	{ .name = "viruv" },
	{ .name = "avpcarm7r" },
	{ .name = "displayhc" },
	{ .name = "displayhcb" },
	{ .name = "fdcdrd" },
	{ .name = "g2dr" },
	{ .name = "host1xdmar" },
	{ .name = "host1xr" },
	{ .name = "idxsrd" },
	{ .name = "mpcorer" },
	{ .name = "mpe_ipred" },
	{ .name = "mpeamemrd" },
	{ .name = "mpecsrd" },
	{ .name = "ppcsahbdmar" },
	{ .name = "ppcsahbslvr" },
	{ .name = "texsrd" },
	{ .name = "vdebsevr" },
	{ .name = "vdember" },
	{ .name = "vdemcer" },
	{ .name = "vdetper" },
	{ .name = "eppu" },
	{ .name = "eppv" },
	{ .name = "eppy" },
	{ .name = "mpeunifbw" },
	{ .name = "viwsb" },
	{ .name = "viwu" },
	{ .name = "viwv" },
	{ .name = "viwy" },
	{ .name = "g2dw" },
	{ .name = "avpcarm7w" },
	{ .name = "fdcdwr" },
	{ .name = "host1xw" },
	{ .name = "ispw" },
	{ .name = "mpcorew" },
	{ .name = "mpecswr" },
	{ .name = "ppcsahbdmaw" },
	{ .name = "ppcsahbslvw" },
	{ .name = "vdebsevw" },
	{ .name = "vdembew" },
	{ .name = "vdetpmw" },
};

const struct tegra_mc_soc tegra20_mc_soc = {
	.clients = tegra20_mc_clients,
	.num_clients = ARRAY_SIZE(tegra20_mc_clients),
	.num_address_bits = 32,
	.client_id_mask = 0x3f,
	.tegra20 = true,
};
