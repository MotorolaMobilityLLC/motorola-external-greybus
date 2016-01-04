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
 *
 */
#define pr_fmt(fmt) "INI: " fmt

#include <linux/module.h>

extern int muc_core_init(void);
extern void muc_core_exit(void);

extern int muc_spi_init(void);
extern void muc_spi_exit(void);

extern int muc_svc_init(void);
extern void muc_svc_exit(void);

extern int mods_nw_init(void);
extern void mods_nw_exit(void);

static int __init mods_init(void)
{
	int err = 0;

	err |= muc_core_init();
	err |= muc_svc_init();
	err |= muc_spi_init();
	err |= mods_nw_init();

	return (err ? -ENODEV : 0);
}

static void __exit mods_exit(void)
{
	muc_spi_exit();
	mods_nw_exit();
	muc_svc_exit();
	muc_core_exit();
}

module_init(mods_init);
module_exit(mods_exit);

MODULE_DESCRIPTION("Mods Driver");
MODULE_AUTHOR("Motorola");
MODULE_LICENSE("GPL");
