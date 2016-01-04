/*
 * Motorola specific driver for a Greybus module.
 *
 * Copyright 2015 Motorola Mobility, LLC.
 * Copyright 2014 Google Inc.
 * Copyright 2014 Linaro Ltd.
 *
 * Released under the GPLv2 only.
 */

#include <linux/device.h>
#include <linux/idr.h>
#include <linux/kdev_t.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/slab.h>
#include "greybus.h"

struct gb_vendor_moto {
	struct gb_connection *connection;
	struct device *dev;
	int minor;  /* vendor minor number */
};

/* Version of the Greybus protocol we support */
#define	GB_VENDOR_MOTO_VERSION_MAJOR		0x00
#define	GB_VENDOR_MOTO_VERSION_MINOR		0x01

/* Greybus Motorola vendor specific request types */
#define	GB_VENDOR_MOTO_TYPE_CHARGE_BASE		0x02
#define	GB_VENDOR_MOTO_TYPE_GET_DMESG		0x03
#define	GB_VENDOR_MOTO_TYPE_GET_LAST_DMESG	0x04

/*
 * This is slightly less than max greybus payload size to allow for headers
 * and other overhead.
 */
#define GB_VENDOR_MOTO_DMESG_SIZE           1000

struct gb_vendor_moto_charge_base_request {
	__u8	enable;
};

struct gb_vendor_moto_dmesg_response {
	char	buf[GB_VENDOR_MOTO_DMESG_SIZE];
};

static ssize_t do_get_dmesg(struct device *dev, struct device_attribute *attr,
			    char *buf, int type)
{
	struct gb_vendor_moto *gb = dev_get_drvdata(dev);
	struct gb_vendor_moto_dmesg_response *rsp;
	int ret;

	rsp = kmalloc(sizeof(struct gb_vendor_moto_dmesg_response), GFP_KERNEL);
	if (!rsp)
		return -ENOMEM;

	ret = gb_operation_sync(gb->connection, type,
				NULL, 0, rsp, sizeof(*rsp));
	if (ret)
		goto err;

	ret = snprintf(buf, sizeof(*rsp), "%s\n", rsp->buf);

err:
	kfree(rsp);
	return ret;
}

static ssize_t dmesg_show(struct device *dev, struct device_attribute *attr,
			  char *buf)
{
	return do_get_dmesg(dev, attr, buf, GB_VENDOR_MOTO_TYPE_GET_DMESG);
}
static DEVICE_ATTR_RO(dmesg);

static ssize_t last_dmesg_show(struct device *dev,
			       struct device_attribute *attr, char *buf)
{
	return do_get_dmesg(dev, attr, buf, GB_VENDOR_MOTO_TYPE_GET_LAST_DMESG);
}
static DEVICE_ATTR_RO(last_dmesg);

static struct attribute *vendor_attrs[] = {
	&dev_attr_dmesg.attr,
	&dev_attr_last_dmesg.attr,
	NULL,
};
ATTRIBUTE_GROUPS(vendor);

static struct class vendor_class = {
	.name		= "vendor",
	.owner		= THIS_MODULE,
#if LINUX_VERSION_CODE >= KERNEL_VERSION(3,11,0)
	.dev_groups	= vendor_groups,
#endif
};

static DEFINE_IDA(minors);

static int charge_base(struct gb_vendor_moto *gb, u8 enable)
{
	struct gb_vendor_moto_charge_base_request request;

	request.enable = enable;
	return gb_operation_sync(gb->connection, GB_VENDOR_MOTO_TYPE_CHARGE_BASE,
				 &request, sizeof(request), NULL, 0);
}

static int gb_vendor_moto_connection_init(struct gb_connection *connection)
{
	struct gb_vendor_moto *gb;
	struct device *dev;
	int retval;

	gb = kzalloc(sizeof(*gb), GFP_KERNEL);
	if (!gb)
		return -ENOMEM;

	gb->connection = connection;
	connection->private = gb;

	/* Enable charging */
	retval = charge_base(gb, 1);
	if (retval)
		goto error;

	/* Create a device in sysfs */
	gb->minor = ida_simple_get(&minors, 0, 0, GFP_KERNEL);
	if (gb->minor < 0) {
		retval = gb->minor;
		goto error;
	}
	dev = device_create(&vendor_class, &connection->bundle->dev,
				MKDEV(0, 0), gb, "mod%d", gb->minor);
	if (IS_ERR(dev)) {
		retval = -EINVAL;
		goto err_ida_remove;
	}
	gb->dev = dev;

	return 0;

err_ida_remove:
	ida_simple_remove(&minors, gb->minor);
error:
	kfree(gb);
	return retval;
}

static void gb_vendor_moto_connection_exit(struct gb_connection *connection)
{
	struct gb_vendor_moto *gb = connection->private;

	ida_simple_remove(&minors, gb->minor);
	device_unregister(gb->dev);
	kfree(gb);
}

static struct gb_protocol vendor_moto_protocol = {
	.name			= "vendor-moto",
	.id			= GREYBUS_PROTOCOL_VENDOR,
	.major			= 0,
	.minor			= 1,
	.connection_init	= gb_vendor_moto_connection_init,
	.connection_exit	= gb_vendor_moto_connection_exit,
	.request_recv		= NULL,	/* no incoming requests */
};

static __init int protocol_init(void)
{
	int retval;

	retval = class_register(&vendor_class);
	if (retval)
		return retval;

	return gb_protocol_register(&vendor_moto_protocol);
}
module_init(protocol_init);

static __exit void protocol_exit(void)
{
	gb_protocol_deregister(&vendor_moto_protocol);
	class_unregister(&vendor_class);
}
module_exit(protocol_exit);

MODULE_LICENSE("GPL v2");
