/*
 * Copyright (C) 2015 Motorola Mobility LLC
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#ifndef __MUC_H__
#define __MUC_H__

enum {
	MUC_GPIO_DET_N    = 0,
	MUC_GPIO_BPLUS_EN = 1,
	MUC_GPIO_VBUS_EN  = 2,
	MUC_MAX_GPIOS
};

#define MUC_MAX_SEQ (MUC_MAX_GPIOS*8)
#define MUC_MAXDATA_LENGTH 256

struct muc_data {
	struct device *dev;
	struct mutex lock;
	int hw_initialized;
	atomic_t enabled;

	struct switch_dev muc_detected;

	/* Configuration */
	int gpios[MUC_MAX_GPIOS];
	int irq;
	u32 det_hysteresis;
	u32 en_seq[MUC_MAX_SEQ];
	size_t en_seq_len;
	u32 dis_seq[MUC_MAX_SEQ];
	size_t dis_seq_len;
};

/* Global functions */
int muc_gpio_init(struct device *dev, struct muc_data *cdata);
bool muc_vbus_is_enabled(struct muc_data *cdata);
void muc_vbus_enable(struct muc_data *cdata);
void muc_vbus_disable(struct muc_data *cdata);
int muc_intr_setup(struct muc_data *cdata, struct device *dev);

/* Global variables */
extern struct muc_data *muc_misc_data;

#endif  /* __MUC_H__ */

