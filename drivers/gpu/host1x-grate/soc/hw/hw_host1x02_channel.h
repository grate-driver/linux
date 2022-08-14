/*
 * Copyright (c) 2013 NVIDIA Corporation.
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

 /*
  * Function naming determines intended use:
  *
  *     <x>_r(void) : Returns the offset for register <x>.
  *
  *     <x>_w(void) : Returns the word offset for word (4 byte) element <x>.
  *
  *     <x>_<y>_s(void) : Returns size of field <y> of register <x> in bits.
  *
  *     <x>_<y>_f(u32 v) : Returns a value based on 'v' which has been shifted
  *         and masked to place it at field <y> of register <x>.  This value
  *         can be |'d with others to produce a full register value for
  *         register <x>.
  *
  *     <x>_<y>_m(void) : Returns a mask for field <y> of register <x>.  This
  *         value can be ~'d and then &'d to clear the value of field <y> for
  *         register <x>.
  *
  *     <x>_<y>_<z>_f(void) : Returns the constant value <z> after being shifted
  *         to place it at field <y> of register <x>.  This value can be |'d
  *         with others to produce a full register value for <x>.
  *
  *     <x>_<y>_v(u32 r) : Returns the value of field <y> from a full register
  *         <x> value 'r' after being shifted to place its LSB at bit 0.
  *         This value is suitable for direct comparison with other unshifted
  *         values appropriate for use in field <y> of register <x>.
  *
  *     <x>_<y>_<z>_v(void) : Returns the constant value for <z> defined for
  *         field <y> of register <x>.  This value is suitable for direct
  *         comparison with unshifted values appropriate for use in field <y>
  *         of register <x>.
  */

#ifndef HOST1X_HW_HOST1X02_CHANNEL_H
#define HOST1X_HW_HOST1X02_CHANNEL_H

#define CHANNEL_IO_SIZE		0x4000

static inline u32 host1x_channel_fifostat_r(unsigned int id)
{
	return 0x0 + id * CHANNEL_IO_SIZE;
}
#define HOST1X_CHANNEL_FIFOSTAT(id) \
	host1x_channel_fifostat_r(id)
static inline u32 host1x_channel_fifostat_cfempty_v(u32 r)
{
	return (r >> 10) & 0x1;
}
#define HOST1X_CHANNEL_FIFOSTAT_CFEMPTY_V(r) \
	host1x_channel_fifostat_cfempty_v(r)
static inline u32 host1x_channel_dmastart_r(unsigned int id)
{
	return 0x14 + id * CHANNEL_IO_SIZE;
}
#define HOST1X_CHANNEL_DMASTART(id) \
	host1x_channel_dmastart_r(id)
static inline u32 host1x_channel_dmaput_r(unsigned int id)
{
	return 0x18 + id * CHANNEL_IO_SIZE;
}
#define HOST1X_CHANNEL_DMAPUT(id) \
	host1x_channel_dmaput_r(id)
static inline u32 host1x_channel_dmaget_r(unsigned int id)
{
	return 0x1c + id * CHANNEL_IO_SIZE;
}
#define HOST1X_CHANNEL_DMAGET(id) \
	host1x_channel_dmaget_r(id)
static inline u32 host1x_channel_dmaend_r(unsigned int id)
{
	return 0x20 + id * CHANNEL_IO_SIZE;
}
#define HOST1X_CHANNEL_DMAEND(id) \
	host1x_channel_dmaend_r(id)
static inline u32 host1x_channel_dmactrl_r(unsigned int id)
{
	return 0x24 + id * CHANNEL_IO_SIZE;
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

#endif
