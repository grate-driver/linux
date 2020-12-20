/*
 * Copyright (c) 2018 NVIDIA Corporation.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#ifndef HOST1X_HW_HOST1X07_CHANNEL_H
#define HOST1X_HW_HOST1X07_CHANNEL_H

#define CHANNEL_IO_SIZE		0x100

static inline u32 host1x_channel_dmastart_r(unsigned int id)
{
	return 0x00 + id * CHANNEL_IO_SIZE;
}
#define HOST1X_CHANNEL_DMASTART(id) \
	host1x_channel_dmastart_r(id)
static inline u32 host1x_channel_dmastart_hi_r(unsigned int id)
{
	return 0x04 + id * CHANNEL_IO_SIZE;
}
#define HOST1X_CHANNEL_DMASTART_HI(id) \
	host1x_channel_dmastart_hi_r(id)
static inline u32 host1x_channel_dmaput_r(unsigned int id)
{
	return 0x08 + id * CHANNEL_IO_SIZE;
}
#define HOST1X_CHANNEL_DMAPUT(id) \
	host1x_channel_dmaput_r(id)
static inline u32 host1x_channel_dmaput_hi_r(unsigned int id)
{
	return 0x0c + id * CHANNEL_IO_SIZE;
}
#define HOST1X_CHANNEL_DMAPUT_HI(id) \
	host1x_channel_dmaput_hi_r(id)
static inline u32 host1x_channel_dmaget_r(unsigned int id)
{
	return 0x10 + id * CHANNEL_IO_SIZE;
}
#define HOST1X_CHANNEL_DMAGET(id) \
	host1x_channel_dmaget_r(id)
static inline u32 host1x_channel_dmaget_hi_r(unsigned int id)
{
	return 0x14 + id * CHANNEL_IO_SIZE;
}
#define HOST1X_CHANNEL_DMAGET_HI(id) \
	host1x_channel_dmaget_hi_r(id)
static inline u32 host1x_channel_dmaend_r(unsigned int id)
{
	return 0x18 + id * CHANNEL_IO_SIZE;
}
#define HOST1X_CHANNEL_DMAEND(id) \
	host1x_channel_dmaend_r(id)
static inline u32 host1x_channel_dmaend_hi_r(unsigned int id)
{
	return 0x1c + id * CHANNEL_IO_SIZE;
}
#define HOST1X_CHANNEL_DMAEND_HI(id) \
	host1x_channel_dmaend_hi_r(id)
static inline u32 host1x_channel_dmactrl_r(unsigned int id)
{
	return 0x20 + id * CHANNEL_IO_SIZE;
}
#define HOST1X_CHANNEL_DMACTRL(id) \
	host1x_channel_dmactrl_r(id)
static inline u32 host1x_channel_dmactrl_dmastop(void)
{
	return 1 << 0;
}
#define HOST1X_CHANNEL_DMACTRL_DMASTOP \
	host1x_channel_dmactrl_dmastop()
static inline u32 host1x_channel_dmactrl_dmastop_v(u32 r)
{
	return (r >> 0) & 0x1;
}
#define HOST1X_CHANNEL_DMACTRL_DMASTOP_V(r) \
	host1x_channel_dmactrl_dmastop_v(r)
static inline u32 host1x_channel_dmactrl_dmagetrst(void)
{
	return 1 << 1;
}
#define HOST1X_CHANNEL_DMACTRL_DMAGETRST \
	host1x_channel_dmactrl_dmagetrst()
static inline u32 host1x_channel_dmactrl_dmainitget(void)
{
	return 1 << 2;
}
#define HOST1X_CHANNEL_DMACTRL_DMAINITGET \
	host1x_channel_dmactrl_dmainitget()
static inline u32 host1x_channel_cmdproc_stop_r(unsigned int id)
{
	return 0x48 + id * CHANNEL_IO_SIZE;
}
#define HOST1X_CHANNEL_CMDPROC_STOP(id) \
	host1x_channel_cmdproc_stop_r(id)
static inline u32 host1x_channel_teardown_r(unsigned int id)
{
	return 0x4c + id * CHANNEL_IO_SIZE;
}
#define HOST1X_CHANNEL_TEARDOWN(id) \
	host1x_channel_teardown_r(id)
static inline u32 host1x_channel_fifostat_r(unsigned int id)
{
	return 0x24 + id * CHANNEL_IO_SIZE;
}
#define HOST1X_CHANNEL_FIFOSTAT(id) \
	host1x_channel_fifostat_r(id)
static inline u32 host1x_channel_fifostat_cfempty_v(u32 r)
{
	return (r >> 13) & 0x1;
}
#define HOST1X_CHANNEL_FIFOSTAT_CFEMPTY_V(r) \
	host1x_channel_fifostat_cfempty_v(r)
static inline u32 host1x_channel_cmdfifo_rdata_r(unsigned int id)
{
	return 0x28 + id * CHANNEL_IO_SIZE;
}
#define HOST1X_CHANNEL_CMDFIFO_RDATA(id) \
	host1x_channel_cmdfifo_rdata_r(id)
static inline u32 host1x_channel_cmdp_offset_r(unsigned int id)
{
	return 0x30 + id * CHANNEL_IO_SIZE;
}
#define HOST1X_CHANNEL_CMDP_OFFSET(id) \
	host1x_channel_cmdp_offset_r(id)
static inline u32 host1x_channel_cmdp_class_r(unsigned int id)
{
	return 0x34 + id * CHANNEL_IO_SIZE;
}
#define HOST1X_CHANNEL_CMDP_CLASS(id) \
	host1x_channel_cmdp_class_r(id)
static inline u32 host1x_channel_smmu_streamid_r(unsigned int id)
{
	return 0x084 + id * CHANNEL_IO_SIZE;
}
#define HOST1X_CHANNEL_SMMU_STREAMID(id) \
	host1x_channel_smmu_streamid_r(id)

#define HOST1X_SYNC_SYNCPT_CPU_INCR(x)			(0x6400 + 4 * (x))
#define HOST1X_SYNC_SYNCPT_THRESH_CPU0_INT_STATUS(x)	(0x6464 + 4 * (x))
#define HOST1X_SYNC_SYNCPT_THRESH_INT_ENABLE_CPU0(x)	(0x652c + 4 * (x))
#define HOST1X_SYNC_SYNCPT_THRESH_INT_DISABLE(x)	(0x6590 + 4 * (x))
#define HOST1X_SYNC_SYNCPT(x)				(0x8080 + 4 * (x))
#define HOST1X_SYNC_SYNCPT_INT_THRESH(x)		(0x8d00 + 4 * (x))
#define HOST1X_SYNC_SYNCPT_CH_APP(x)			(0xa604 + 4 * (x))
#define HOST1X_SYNC_SYNCPT_CH_APP_CH(v)			(((v) & 0x3f) << 8)

#endif
