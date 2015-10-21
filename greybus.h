/*
 * Greybus driver and device API
 *
 * Copyright 2014-2015 Google Inc.
 * Copyright 2014-2015 Linaro Ltd.
 *
 * Released under the GPLv2 only.
 */

#ifndef __LINUX_GREYBUS_H
#define __LINUX_GREYBUS_H

#ifdef __KERNEL__

#include <linux/kernel.h>
#include <linux/types.h>
#include <linux/list.h>
#include <linux/slab.h>
#include <linux/device.h>
#include <linux/module.h>
#include <linux/idr.h>

#include "kernel_ver.h"
#include "greybus_id.h"
#include "greybus_manifest.h"
#include "greybus_protocols.h"
#include "manifest.h"
#include "endo.h"
#include "svc.h"
#include "firmware.h"
#include "module.h"
#include "control.h"
#include "interface.h"
#include "bundle.h"
#include "connection.h"
#include "protocol.h"
#include "operation.h"


/* Matches up with the Greybus Protocol specification document */
#define GREYBUS_VERSION_MAJOR	0x00
#define GREYBUS_VERSION_MINOR	0x01

#define GREYBUS_DEVICE_ID_MATCH_DEVICE \
	(GREYBUS_DEVICE_ID_MATCH_VENDOR | GREYBUS_DEVICE_ID_MATCH_PRODUCT)

#define GREYBUS_DEVICE(v, p)					\
	.match_flags	= GREYBUS_DEVICE_ID_MATCH_DEVICE,	\
	.vendor		= (v),					\
	.product	= (p),

#define GREYBUS_DEVICE_SERIAL(s)				\
	.match_flags	= GREYBUS_DEVICE_ID_MATCH_SERIAL,	\
	.serial_number	= (s),

/* Maximum number of CPorts */
#define CPORT_ID_MAX	4095		/* UniPro max id is 4095 */
#define CPORT_ID_BAD	U16_MAX

/* For SP1 hardware, we are going to "hardcode" each device to have all logical
 * blocks in order to be able to address them as one unified "unit".  Then
 * higher up layers will then be able to talk to them as one logical block and
 * properly know how they are hooked together (i.e. which i2c port is on the
 * same module as the gpio pins, etc.)
 *
 * So, put the "private" data structures here in greybus.h and link to them off
 * of the "main" gb_module structure.
 */

struct greybus_host_device;

/* Greybus "Host driver" structure, needed by a host controller driver to be
 * able to handle both SVC control as well as "real" greybus messages
 */
struct greybus_host_driver {
	size_t	hd_priv_size;

	int (*cport_enable)(struct greybus_host_device *hd, u16 cport_id);
	int (*cport_disable)(struct greybus_host_device *hd, u16 cport_id);
	int (*message_send)(struct greybus_host_device *hd, u16 dest_cport_id,
			struct gb_message *message, gfp_t gfp_mask);
	void (*message_cancel)(struct gb_message *message);
	int (*latency_tag_enable)(struct greybus_host_device *hd, u16 cport_id);
	int (*latency_tag_disable)(struct greybus_host_device *hd,
				   u16 cport_id);
};

struct greybus_host_device {
	struct kref kref;
	struct device *parent;
	const struct greybus_host_driver *driver;

	struct list_head interfaces;
	struct list_head connections;
	struct ida cport_id_map;
	u8 device_id;

	/* Number of CPorts supported by the UniPro IP */
	size_t num_cports;

	/* Host device buffer constraints */
	size_t buffer_size_max;

	struct gb_endo *endo;
	struct gb_connection *initial_svc_connection;
	struct gb_svc *svc;

	/* Private data for the host driver */
	unsigned long hd_priv[0] __aligned(sizeof(s64));
};

struct greybus_host_device *greybus_create_hd(struct greybus_host_driver *hd,
					      struct device *parent,
					      size_t buffer_size_max,
					      size_t num_cports);
void greybus_remove_hd(struct greybus_host_device *hd);

struct greybus_driver {
	const char *name;

	int (*probe)(struct gb_bundle *bundle,
		     const struct greybus_bundle_id *id);
	void (*disconnect)(struct gb_bundle *bundle);

	int (*suspend)(struct gb_bundle *bundle, pm_message_t message);
	int (*resume)(struct gb_bundle *bundle);

	const struct greybus_bundle_id *id_table;

	struct device_driver driver;
};
#define to_greybus_driver(d) container_of(d, struct greybus_driver, driver)

/* Don't call these directly, use the module_greybus_driver() macro instead */
int greybus_register_driver(struct greybus_driver *driver,
			    struct module *module, const char *mod_name);
void greybus_deregister_driver(struct greybus_driver *driver);

/* define to get proper THIS_MODULE and KBUILD_MODNAME values */
#define greybus_register(driver) \
	greybus_register_driver(driver, THIS_MODULE, KBUILD_MODNAME)
#define greybus_deregister(driver) \
	greybus_deregister_driver(driver)

/**
 * module_greybus_driver() - Helper macro for registering a Greybus driver
 * @__greybus_driver: greybus_driver structure
 *
 * Helper macro for Greybus drivers to set up proper module init / exit
 * functions.  Replaces module_init() and module_exit() and keeps people from
 * printing pointless things to the kernel log when their driver is loaded.
 */
#define module_greybus_driver(__greybus_driver)	\
	module_driver(__greybus_driver, greybus_register, greybus_deregister)

int greybus_disabled(void);

void gb_debugfs_init(void);
void gb_debugfs_cleanup(void);
struct dentry *gb_debugfs_get(void);

extern struct bus_type greybus_bus_type;

extern struct device_type greybus_endo_type;
extern struct device_type greybus_module_type;
extern struct device_type greybus_interface_type;
extern struct device_type greybus_bundle_type;
extern struct device_type greybus_connection_type;

static inline int is_gb_endo(const struct device *dev)
{
	return dev->type == &greybus_endo_type;
}

static inline int is_gb_module(const struct device *dev)
{
	return dev->type == &greybus_module_type;
}

static inline int is_gb_interface(const struct device *dev)
{
	return dev->type == &greybus_interface_type;
}

static inline int is_gb_bundle(const struct device *dev)
{
	return dev->type == &greybus_bundle_type;
}

static inline bool cport_id_valid(struct greybus_host_device *hd, u16 cport_id)
{
	return cport_id != CPORT_ID_BAD && cport_id < hd->num_cports;
}

#endif /* __KERNEL__ */
#endif /* __LINUX_GREYBUS_H */
