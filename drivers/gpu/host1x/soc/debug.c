/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2010 Google, Inc.
 * Author: Erik Gilling <konkers@android.com>
 *
 * Copyright (C) 2011-2013 NVIDIA Corporation
 */

#define INVALID_PAYLOAD		0xffffffff

static unsigned int
show_channel_command(struct host1x_dbg_output *o, u32 val, u32 *payload)
{
	unsigned int mask, subop, num, opcode;

	if (val == HOST1X_OPCODE_NOP) {
		host1x_debug_cont(o, "NOP\n");
		return 0;
	}

	opcode = val >> 28;

	switch (opcode) {
	case HOST1X_OPCODE_SETCLASS:
		mask = val & 0x3f;
		if (mask) {
			host1x_debug_cont(o, "SETCL(class=%03x, offset=%03x, mask=%02x, [",
					  val >> 6 & 0x3ff,
					  val >> 16 & 0xfff, mask);
			return hweight8(mask);
		}

		host1x_debug_cont(o, "SETCL(class=%03x)\n", val >> 6 & 0x3ff);
		return 0;

	case HOST1X_OPCODE_INCR:
		num = val & 0xffff;
		host1x_debug_cont(o, "INCR(offset=%03x, [", val >> 16 & 0xfff);
		if (!num)
			host1x_debug_cont(o, "])\n");

		return num;

	case HOST1X_OPCODE_NONINCR:
		num = val & 0xffff;
		host1x_debug_cont(o, "NONINCR(offset=%03x, [",
				  val >> 16 & 0xfff);
		if (!num)
			host1x_debug_cont(o, "])\n");

		return num;

	case HOST1X_OPCODE_MASK:
		mask = val & 0xffff;
		host1x_debug_cont(o, "MASK(offset=%03x, mask=%03x, [",
				  val >> 16 & 0xfff, mask);
		if (!mask)
			host1x_debug_cont(o, "])\n");

		return hweight16(mask);

	case HOST1X_OPCODE_IMM:
		host1x_debug_cont(o, "IMM(offset=%03x, data=%03x)\n",
				  val >> 16 & 0xfff, val & 0xffff);
		return 0;

	case HOST1X_OPCODE_RESTART:
		host1x_debug_cont(o, "RESTART(offset=%08x)\n", val << 4);
		return 0;

	case HOST1X_OPCODE_GATHER:
		host1x_debug_cont(o, "GATHER(offset=%03x, insert=%d, type=%d, count=%04x, addr=[",
				  val >> 16 & 0xfff, val >> 15 & 0x1,
				  val >> 14 & 0x1, val & 0x3fff);
		return 1;

#if HOST1X_HW >= 6
	case HOST1X_OPCODE_SETSTRMID:
		host1x_debug_cont(o, "SETSTRMID(offset=%06x)\n",
				  val & 0x3fffff);
		return 0;

	case HOST1X_OPCODE_SETAPPID:
		host1x_debug_cont(o, "SETAPPID(appid=%02x)\n", val & 0xff);
		return 0;

	case HOST1X_OPCODE_SETPYLD:
		*payload = val & 0xffff;
		host1x_debug_cont(o, "SETPYLD(data=%04x)\n", *payload);
		return 0;

	case HOST1X_OPCODE_INCR_W:
	case HOST1X_OPCODE_NONINCR_W:
		host1x_debug_cont(o, "%s(offset=%06x, ",
				  opcode == HOST1X_OPCODE_INCR_W ?
					"INCR_W" : "NONINCR_W",
				  val & 0x3fffff);
		if (*payload == 0) {
			host1x_debug_cont(o, "[])\n");
			return 0;
		} else if (*payload == INVALID_PAYLOAD) {
			host1x_debug_cont(o, "unknown)\n");
			return 0;
		} else {
			host1x_debug_cont(o, "[");
			return *payload;
		}

	case HOST1X_OPCODE_GATHER_W:
		host1x_debug_cont(o, "GATHER_W(count=%04x, addr=[",
				  val & 0x3fff);
		return 2;

	case HOST1X_OPCODE_RESTART_W:
		host1x_debug_cont(o, "RESTART_W(addr=[");
		return 2;
#endif

	case HOST1X_OPCODE_EXTEND:
		subop = val >> 24 & 0xf;
		if (subop == HOST1X_OPCODE_EXTEND_ACQUIRE_MLOCK)
			host1x_debug_cont(o, "ACQUIRE_MLOCK(index=%d)\n",
					  val & 0xff);
		else if (subop == HOST1X_OPCODE_EXTEND_RELEASE_MLOCK)
			host1x_debug_cont(o, "RELEASE_MLOCK(index=%d)\n",
					  val & 0xff);
		else
			host1x_debug_cont(o, "EXTEND_UNKNOWN(%08x)\n", val);
		return 0;

	default:
		host1x_debug_cont(o, "UNKNOWN\n");
		return 0;
	}
}

static void
parse_cmdstream(struct host1x_dbg_output *o, dma_addr_t dmaaddr, u32 *vaddr,
		unsigned int words)
{
	u32 payload = INVALID_PAYLOAD;
	unsigned int data_count;
	unsigned int i;

	for (i = 0, data_count = 0; i < words; i++) {
		u32 addr = dmaaddr + i * 4;
		u32 val = *(vaddr + i);

		if (!data_count) {
			host1x_debug_output(o, "%08x: %08x: ", addr, val);
			data_count = show_channel_command(o, val, &payload);
		} else {
			host1x_debug_cont(o, "%08x%s", val,
					  data_count > 1 ? ", " : "])\n");
			data_count--;
		}
	}

	if (data_count)
		host1x_debug_cont(o, "CMDSTREAM ended unexpectedly!\n");
}

static void
host1x_soc_dump_cmdbuf(struct host1x_dbg_output *o, struct host1x_bo *bo,
		       unsigned int num_words)
{
	parse_cmdstream(o, bo->dmaaddr, bo->vaddr, num_words);
}
