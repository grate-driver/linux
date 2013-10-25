/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * MFD driver for Acer Iconia Tab A500 Embedded Controller
 */

#ifndef __MFD_A500_EC_H
#define __MFD_A500_EC_H

#include <linux/types.h>

struct ec_reg_data {
	u8  addr;
	u16 timeout;
};

#define EC_REG_DATA(_name, _addr, _timeout)				\
static const struct ec_reg_data EC_##_name = {				\
	.addr = _addr,							\
	.timeout = _timeout,						\
};									\
static const __maybe_unused struct ec_reg_data *_name = &EC_##_name

int a500_ec_read_word_data_locked(const struct ec_reg_data *reg_data);
int a500_ec_read_word_data(const struct ec_reg_data *reg_data);
int a500_ec_write_word_data_locked(const struct ec_reg_data *reg_data, u16 value);
int a500_ec_write_word_data(const struct ec_reg_data *reg_data, u16 value);
void a500_ec_lock(void);
void a500_ec_unlock(void);

#endif	/* __MFD_A500_EC_H */
