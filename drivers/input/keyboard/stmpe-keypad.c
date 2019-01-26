// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) ST-Ericsson SA 2010
 *
 * Author: Rabin Vincent <rabin.vincent@stericsson.com> for ST-Ericsson
 */

#include <linux/module.h>
#include <linux/slab.h>
#include <linux/input.h>
#include <linux/interrupt.h>
#include <linux/platform_device.h>
#include <linux/input/matrix_keypad.h>
#include <linux/mfd/stmpe.h>

/* These are at the same addresses in all STMPE variants, except 1801 */
#define STMPE_KPC_COL			0x60
#define STMPE_KPC_ROW_MSB		0x61
#define STMPE_KPC_ROW_LSB		0x62
#define STMPE_KPC_CTRL_MSB		0x63
#define STMPE_KPC_CTRL_LSB		0x64
#define STMPE_KPC_COMBI_KEY_0		0x65
#define STMPE_KPC_COMBI_KEY_1		0x66
#define STMPE_KPC_COMBI_KEY_2		0x67
#define STMPE_KPC_DATA_BYTE0		0x68
#define STMPE_KPC_DATA_BYTE1		0x69
#define STMPE_KPC_DATA_BYTE2		0x6a
#define STMPE_KPC_DATA_BYTE3		0x6b
#define STMPE_KPC_DATA_BYTE4		0x6c

#define STMPE_KPC_CTRL_LSB_SCAN		(0x1 << 0)
#define STMPE_KPC_CTRL_LSB_DEBOUNCE	(0x7f << 1)
#define STMPE_KPC_CTRL_MSB_SCAN_COUNT	(0xf << 4)

#define STMPE_KPC_ROW_MSB_ROWS		0xff

#define STMPE_KPC_DATA_UP		(0x1 << 7)
#define STMPE_KPC_DATA_ROW		(0xf << 3)
#define STMPE_KPC_DATA_COL		(0x7 << 0)
#define STMPE_KPC_DATA_NOKEY_MASK	0x78

#define STMPE_KEYPAD_MAX_DEBOUNCE	127
#define STMPE_KEYPAD_MAX_SCAN_COUNT	15

#define STMPE_KEYPAD_MAX_ROWS		10
#define STMPE_KEYPAD_MAX_COLS		12
#define STMPE_KEYPAD_ROW_SHIFT		3
#define STMPE_KEYPAD_KEYMAP_MAX_SIZE \
	(STMPE_KEYPAD_MAX_ROWS * STMPE_KEYPAD_MAX_COLS)


#define STMPE1601_NUM_DATA	5
#define STMPE1801_NUM_DATA	5
#define STMPE2401_NUM_DATA	3
#define STMPE2403_NUM_DATA	5

/* Make sure it covers all cases above */
#define MAX_NUM_DATA		5

/**
 * struct stmpe_keypad_variant - model-specific attributes
 * @auto_increment: whether the KPC_DATA_BYTE register address
 *		    auto-increments on multiple read
 * @set_pullup: whether the pins need to have their pull-ups set
 * @num_data: number of data bytes
 * @num_normal_data: number of normal keys' data bytes
 * @max_cols: maximum number of columns supported
 * @max_rows: maximum number of rows supported
 * @col_gpios: bitmask of gpios which can be used for columns
 * @row_gpios: bitmask of gpios which can be used for rows
 * @col_regs: registers for setting column pins
 * @row_regs: registers for setting row pins
 * @data_regs: registers for reading key data
 * @ctrl_msb_reg: register for setting scan count
 * @ctrl_lsb_reg: register for setting debounce time
 * @cmd_reg: register for toggling scan mode
 */
struct stmpe_keypad_variant {
	bool		auto_increment;
	bool		set_pullup;
	int		num_data;
	int		num_normal_data;
	int		max_cols;
	int		max_rows;
	unsigned int	col_gpios;
	unsigned int	row_gpios;

#define MAX_COL_REGS 3
#define MAX_ROW_REGS 3
#define MAX_DATA_REGS 5

	u8 col_regs[MAX_COL_REGS];
	u8 row_regs[MAX_ROW_REGS];
	u8 data_regs[MAX_DATA_REGS];
	u8 ctrl_msb_reg;
	u8 ctrl_lsb_reg;
	u8 cmd_reg;
	bool read_inverted;
};

static const struct stmpe_keypad_variant stmpe_keypad_variants[] = {
	[STMPE1601] = {
		.auto_increment		= true,
		.num_data		= STMPE1601_NUM_DATA,
		.num_normal_data	= 3,
		.max_cols		= 8,
		.max_rows		= 8,
		.col_gpios		= 0x000ff,	/* GPIO 0 - 7 */
		.row_gpios		= 0x0ff00,	/* GPIO 8 - 15 */
		.col_regs		= { 0x60 },
		.row_regs		= { 0x62, 0x61 },
		.data_regs		= { 0x68, 0x69, 0x6a, 0x6b, 0x6c },
		.ctrl_msb_reg		= 0x63,
		.ctrl_lsb_reg		= 0x64,
		.cmd_reg		= 0x64,
		.read_inverted		= 0,
	},
	[STMPE1801] = {
		.auto_increment		= true,
		.num_data		= STMPE1801_NUM_DATA,
		.num_normal_data	= 3,
		.max_cols		= 10,
		.max_rows		= 8,
		.col_gpios		= 0x3ff00,	/* GPIO 8 - 17 */
		.row_gpios		= 0x000ff,	/* GPIO 0 - 7 */
		.col_regs		= { 0x31, 0x32 },
		.row_regs		= { 0x30 },
		.data_regs		= { 0x3a, 0x3b, 0x3c, 0x3d, 0x3e },
		.ctrl_msb_reg		= 0x33,
		.ctrl_lsb_reg		= 0x34,
		.cmd_reg		= 0x36,
		.read_inverted		= 1,
	},
	[STMPE2401] = {
		.auto_increment		= false,
		.set_pullup		= true,
		.num_data		= STMPE2401_NUM_DATA,
		.num_normal_data	= 2,
		.max_cols		= 8,
		.max_rows		= 12,
		.col_gpios		= 0x0000ff,	/* GPIO 0 - 7*/
		.row_gpios		= 0x1f7f00,	/* GPIO 8-14, 16-20 */
		.col_regs		= { 0x60 },
		.row_regs		= { 0x62, 0x61 },
		.data_regs		= { 0x68, 0x69, 0x6a, 0x6b, 0x6c },
		.ctrl_msb_reg		= 0x63,
		.ctrl_lsb_reg		= 0x64,
		.cmd_reg		= 0x64,
		.read_inverted		= 0,
	},
	[STMPE2403] = {
		.auto_increment		= true,
		.set_pullup		= true,
		.num_data		= STMPE2403_NUM_DATA,
		.num_normal_data	= 3,
		.max_cols		= 8,
		.max_rows		= 12,
		.col_gpios		= 0x0000ff,	/* GPIO 0 - 7*/
		.row_gpios		= 0x1fef00,	/* GPIO 8-14, 16-20 */
		.col_regs		= { 0x60 },
		.row_regs		= { 0x62, 0x61 },
		.data_regs		= { 0x68, 0x69, 0x6a, 0x6b, 0x6c },
		.ctrl_msb_reg		= 0x63,
		.ctrl_lsb_reg		= 0x64,
		.cmd_reg		= 0x64,
		.read_inverted		= 0,
	},
};

/**
 * struct stmpe_keypad - STMPE keypad state container
 * @stmpe: pointer to parent STMPE device
 * @input: spawned input device
 * @variant: STMPE variant
 * @debounce_ms: debounce interval, in ms.  Maximum is
 *		 %STMPE_KEYPAD_MAX_DEBOUNCE.
 * @scan_count: number of key scanning cycles to confirm key data.
 *		Maximum is %STMPE_KEYPAD_MAX_SCAN_COUNT.
 * @no_autorepeat: disable key autorepeat
 * @rows: bitmask for the rows
 * @cols: bitmask for the columns
 * @keymap: the keymap
 */
struct stmpe_keypad {
	struct stmpe *stmpe;
	struct input_dev *input;
	const struct stmpe_keypad_variant *variant;
	unsigned int debounce_ms;
	unsigned int scan_count;
	bool no_autorepeat;
	unsigned int rows;
	unsigned int cols;
	unsigned short keymap[STMPE_KEYPAD_KEYMAP_MAX_SIZE];
};

static int stmpe_keypad_read_data(struct stmpe_keypad *keypad, u8 *data)
{
	const struct stmpe_keypad_variant *variant = keypad->variant;
	struct stmpe *stmpe = keypad->stmpe;
	int ret;
	int i;

	if (variant->auto_increment)
		return stmpe_block_read(stmpe, variant->data_regs[0],
					variant->num_data, data);

	for (i = 0; i < variant->num_data; i++) {
		ret = stmpe_reg_read(stmpe, variant->data_regs[i]);
		if (ret < 0)
			return ret;

		data[i] = ret;
	}

	return 0;
}

static irqreturn_t stmpe_keypad_irq(int irq, void *dev)
{
	struct stmpe_keypad *keypad = dev;
	struct input_dev *input = keypad->input;
	const struct stmpe_keypad_variant *variant = keypad->variant;
	u8 fifo[MAX_NUM_DATA];
	int ret;
	int i;

	ret = stmpe_keypad_read_data(keypad, fifo);
	if (ret < 0)
		return IRQ_NONE;

	for (i = 0; i < variant->num_normal_data; i++) {
		u8 data = fifo[i];
		int row = (data & STMPE_KPC_DATA_ROW) >> 3;
		int col = data & STMPE_KPC_DATA_COL;
		int code = variant->read_inverted ?
			MATRIX_SCAN_CODE(col, row, STMPE_KEYPAD_ROW_SHIFT):
			MATRIX_SCAN_CODE(row, col, STMPE_KEYPAD_ROW_SHIFT);
		bool up = data & STMPE_KPC_DATA_UP;

		if ((data & STMPE_KPC_DATA_NOKEY_MASK)
			== STMPE_KPC_DATA_NOKEY_MASK)
			continue;

		input_event(input, EV_MSC, MSC_SCAN, code);
		input_report_key(input, keypad->keymap[code], !up);
		input_sync(input);
	}

	return IRQ_HANDLED;
}

static int stmpe_keypad_altfunc_init(struct stmpe_keypad *keypad)
{
	const struct stmpe_keypad_variant *variant = keypad->variant;
	unsigned int col_gpios = variant->col_gpios;
	unsigned int row_gpios = variant->row_gpios;
	struct stmpe *stmpe = keypad->stmpe;
	u8 pureg = stmpe->regs[STMPE_IDX_GPPUR_LSB];
	unsigned int pins = 0;
	unsigned int pu_pins = 0;
	int ret;
	int i;

	/*
	 * Figure out which pins need to be set to the keypad alternate
	 * function.
	 *
	 * {cols,rows}_gpios are bitmasks of which pins on the chip can be used
	 * for the keypad.
	 *
	 * keypad->{cols,rows} are a bitmask of which pins (of the ones useable
	 * for the keypad) are used on the board.
	 */

	for (i = 0; i < variant->max_cols; i++) {
		int num = __ffs(col_gpios);

		if (keypad->cols & (1 << i)) {
			pins |= 1 << num;
			pu_pins |= 1 << num;
		}

		col_gpios &= ~(1 << num);
	}

	for (i = 0; i < variant->max_rows; i++) {
		int num = __ffs(row_gpios);

		if (keypad->rows & (1 << i))
			pins |= 1 << num;

		row_gpios &= ~(1 << num);
	}

	ret = stmpe_set_altfunc(stmpe, pins, STMPE_BLOCK_KEYPAD);
	if (ret)
		return ret;

	/*
	 * On STMPE24xx, set pin bias to pull-up on all keypad input
	 * pins (columns), this incidentally happen to be maximum 8 pins
	 * and placed at GPIO0-7 so only the LSB of the pull up register
	 * ever needs to be written.
	 */
	if (variant->set_pullup) {
		u8 val;

		ret = stmpe_reg_read(stmpe, pureg);
		if (ret)
			return ret;

		/* Do not touch unused pins, may be used for GPIO */
		val = ret & ~pu_pins;
		val |= pu_pins;

		ret = stmpe_reg_write(stmpe, pureg, val);
	}

	return 0;
}

static int stmpe_keypad_chip_init(struct stmpe_keypad *keypad)
{
	const struct stmpe_keypad_variant *variant = keypad->variant;
	struct stmpe *stmpe = keypad->stmpe;
	int ret, val, i;

	if (keypad->debounce_ms > STMPE_KEYPAD_MAX_DEBOUNCE)
		return -EINVAL;

	if (keypad->scan_count > STMPE_KEYPAD_MAX_SCAN_COUNT)
		return -EINVAL;

	ret = stmpe_enable(stmpe, STMPE_BLOCK_KEYPAD);
	if (ret < 0)
		return ret;

	ret = stmpe_keypad_altfunc_init(keypad);
	if (ret < 0)
		return ret;

	val = keypad->cols;
	i = 0;
	do {
		ret = stmpe_reg_write(stmpe, variant->col_regs[i++], val & 0xff);
		if (ret < 0)
			return ret;
	} while ((val >>= 8) != 0);

	val = keypad->rows;
	i = 0;
	do {
		ret = stmpe_reg_write(stmpe, variant->row_regs[i++], val & 0xff);
		if (ret < 0)
			return ret;
	} while ((val >>= 8) != 0);

	ret = stmpe_set_bits(stmpe, variant->ctrl_msb_reg,
			     STMPE_KPC_CTRL_MSB_SCAN_COUNT,
			     keypad->scan_count << 4);
	if (ret < 0)
		return ret;

	ret = stmpe_set_bits(stmpe, variant->ctrl_lsb_reg,
			      STMPE_KPC_CTRL_LSB_DEBOUNCE,
			      (keypad->debounce_ms << 1));
	if (ret < 0)
		return ret;

	return stmpe_set_bits(stmpe, variant->cmd_reg,
			      STMPE_KPC_CTRL_LSB_SCAN,
			      STMPE_KPC_CTRL_LSB_SCAN);
}

static void stmpe_keypad_fill_used_pins(struct stmpe_keypad *keypad,
					u32 used_rows, u32 used_cols)
{
	int row, col;

	for (row = 0; row < used_rows; row++) {
		for (col = 0; col < used_cols; col++) {
			int code = MATRIX_SCAN_CODE(row, col,
						    STMPE_KEYPAD_ROW_SHIFT);
			if (keypad->keymap[code] != KEY_RESERVED) {
				keypad->rows |= 1 << row;
				keypad->cols |= 1 << col;
			}
		}
	}
}

static int stmpe_keypad_probe(struct platform_device *pdev)
{
	struct stmpe *stmpe = dev_get_drvdata(pdev->dev.parent);
	struct device_node *np = pdev->dev.of_node;
	struct stmpe_keypad *keypad;
	struct input_dev *input;
	u32 rows;
	u32 cols;
	int error;
	int irq;

	irq = platform_get_irq(pdev, 0);
	if (irq < 0)
		return irq;

	keypad = devm_kzalloc(&pdev->dev, sizeof(struct stmpe_keypad),
			      GFP_KERNEL);
	if (!keypad)
		return -ENOMEM;

	keypad->stmpe = stmpe;
	keypad->variant = &stmpe_keypad_variants[stmpe->partnum];

	of_property_read_u32(np, "debounce-interval", &keypad->debounce_ms);
	of_property_read_u32(np, "st,scan-count", &keypad->scan_count);
	keypad->no_autorepeat = of_property_read_bool(np, "st,no-autorepeat");

	input = devm_input_allocate_device(&pdev->dev);
	if (!input)
		return -ENOMEM;

	input->name = "STMPE keypad";
	input->id.bustype = BUS_I2C;
	input->dev.parent = &pdev->dev;

	error = matrix_keypad_parse_properties(&pdev->dev, &rows, &cols);
	if (error)
		return error;

	error = matrix_keypad_build_keymap(NULL, NULL, rows, cols,
					   keypad->keymap, input);
	if (error)
		return error;

	input_set_capability(input, EV_MSC, MSC_SCAN);
	if (!keypad->no_autorepeat)
		__set_bit(EV_REP, input->evbit);

	stmpe_keypad_fill_used_pins(keypad, rows, cols);

	keypad->input = input;

	error = stmpe_keypad_chip_init(keypad);
	if (error < 0)
		return error;

	error = devm_request_threaded_irq(&pdev->dev, irq,
					  NULL, stmpe_keypad_irq,
					  IRQF_ONESHOT, "stmpe-keypad", keypad);
	if (error) {
		dev_err(&pdev->dev, "unable to get irq: %d\n", error);
		return error;
	}

	error = input_register_device(input);
	if (error) {
		dev_err(&pdev->dev,
			"unable to register input device: %d\n", error);
		return error;
	}

	platform_set_drvdata(pdev, keypad);

	return 0;
}

static int stmpe_keypad_remove(struct platform_device *pdev)
{
	struct stmpe_keypad *keypad = platform_get_drvdata(pdev);

	stmpe_disable(keypad->stmpe, STMPE_BLOCK_KEYPAD);

	return 0;
}

static struct platform_driver stmpe_keypad_driver = {
	.driver.name	= "stmpe-keypad",
	.driver.owner	= THIS_MODULE,
	.probe		= stmpe_keypad_probe,
	.remove		= stmpe_keypad_remove,
};
module_platform_driver(stmpe_keypad_driver);

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("STMPExxxx keypad driver");
MODULE_AUTHOR("Rabin Vincent <rabin.vincent@stericsson.com>");
