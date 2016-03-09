/*
 * Greybus Backlight protocol driver.
 *
 * Copyright 2016 Motorola LLC
 *
 * Released under the GPLv2 only.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": %s: " fmt, __func__

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/device.h>
#include <linux/kdev_t.h>
#include <linux/idr.h>

#include "greybus.h"

struct gb_backlight_ext_device {
	struct gb_connection	*connection;
	struct device		*dev;
	int			minor;		/* backlight minor number */
};

/* Version of the Greybus Backlight Ext protocol we support */
#define GB_BACKLIGHT_EXT_VERSION_MAJOR		0x00
#define GB_BACKLIGHT_EXT_VERSION_MINOR		0x01

/* Greybus Backlight Ext operation types */
#define GB_BACKLIGHT_EXT_SET_MODE		0x02
#define GB_BACKLIGHT_EXT_GET_MODE		0x03
#define GB_BACKLIGHT_EXT_SET_BRIGHTNESS	0x04
#define GB_BACKLIGHT_EXT_GET_BRIGHTNESS	0x05

enum gb_backlight_ext_mode {
	GB_BACKLIGHT_EXT_MODE_MIN,
	GB_BACKLIGHT_EXT_MODE_MANUAL,
	GB_BACKLIGHT_EXT_MODE_AUTO,
	GB_BACKLIGHT_EXT_MODE_MAX,
};

struct gb_backlight_ext_set_mode_request {
	__u8	mode;
} __packed;
/* set backlight mode request has no response payload */

static int gb_backlight_ext_set_mode(struct gb_connection *connection,
	u8 mode)
{
	struct gb_backlight_ext_set_mode_request request;
	int ret;

	request.mode = mode;

	ret = gb_operation_sync(connection, GB_BACKLIGHT_EXT_SET_MODE,
				&request, sizeof(request), NULL, 0);

	return ret;
}

/* get backlight mode request has no payload */
struct gb_backlight_ext_get_mode_response {
	__u8	mode;
} __packed;

static int gb_backlight_ext_get_mode(struct gb_connection *connection,
	u8 *mode)
{
	struct gb_backlight_ext_get_mode_response response;
	int ret;

	ret = gb_operation_sync(connection, GB_BACKLIGHT_EXT_GET_MODE,
				NULL, 0, &response, sizeof(response));
	if (ret)
		goto exit;

	*mode = response.mode;

exit:
	return ret;
}

struct gb_backlight_ext_set_brightness_request {
	__u8	brightness;
} __packed;
/* set backlight brightness request has no response payload */

static int gb_backlight_ext_set_brightness(struct gb_connection *connection,
	u8 brightness)
{
	struct gb_backlight_ext_set_brightness_request request;
	int ret;

	request.brightness = brightness;

	ret = gb_operation_sync(connection, GB_BACKLIGHT_EXT_SET_BRIGHTNESS,
				&request, sizeof(request), NULL, 0);

	return ret;
}

/* get backlight brightness request has no payload */
struct gb_backlight_ext_get_brightness_response {
	__u8	brightness;
} __packed;

static int gb_backlight_ext_get_brightness(struct gb_connection *connection,
	u8 *brightness)
{
	struct gb_backlight_ext_get_brightness_response response;
	int ret;

	ret = gb_operation_sync(connection, GB_BACKLIGHT_EXT_GET_BRIGHTNESS,
				NULL, 0, &response, sizeof(response));
	if (ret)
		goto exit;

	*brightness = response.brightness;

exit:
	return ret;
}

static ssize_t mode_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	struct gb_backlight_ext_device *backlight_ext = dev_get_drvdata(dev);
	u8 mode;
	int ret = gb_backlight_ext_get_mode(backlight_ext->connection, &mode);

	if (!ret &&
		(mode <= GB_BACKLIGHT_EXT_MODE_MIN ||
		mode >= GB_BACKLIGHT_EXT_MODE_MAX))
		ret = -EINVAL;

	pr_debug("mode: %d, ret: %d\n", mode, ret);

	if (ret)
		return ret;

	return scnprintf(buf, PAGE_SIZE, "%d\n", mode);
}

static ssize_t mode_store(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t count)
{
	struct gb_backlight_ext_device *backlight_ext = dev_get_drvdata(dev);
	u8 mode;
	int ret;

	if (kstrtou8(buf, 0, &mode) < 0) {
		pr_debug("invalid mode %s\n", buf);
		return -EINVAL;
	}

	if (mode <= GB_BACKLIGHT_EXT_MODE_MIN ||
		mode >= GB_BACKLIGHT_EXT_MODE_MAX) {
		pr_debug("invalid mode %d\n", mode);
		return -EINVAL;
	}

	ret = gb_backlight_ext_set_mode(backlight_ext->connection, mode);
	pr_debug("mode: %d, ret: %d\n", mode, ret);
	if (ret)
		return ret;

	return count;
}

static DEVICE_ATTR_RW(mode);

static ssize_t brightness_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	struct gb_backlight_ext_device *backlight_ext = dev_get_drvdata(dev);
	u8 brightness;
	int ret = gb_backlight_ext_get_brightness(backlight_ext->connection,
		&brightness);

	pr_debug("brightness: %d, ret: %d\n", brightness, ret);

	if (ret)
		return ret;

	return scnprintf(buf, PAGE_SIZE, "%d\n", brightness);
}

static ssize_t brightness_store(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t count)
{
	struct gb_backlight_ext_device *backlight_ext = dev_get_drvdata(dev);
	u8 brightness;
	int ret;

	if (kstrtou8(buf, 0, &brightness) < 0) {
		pr_debug("invalid brightness %s\n", buf);
		return -EINVAL;
	}

	ret = gb_backlight_ext_set_brightness(backlight_ext->connection,
		brightness);
	pr_debug("brightness: %d, ret: %d\n", brightness, ret);
	if (ret)
		return ret;

	return count;
}

static DEVICE_ATTR_RW(brightness);

static struct attribute *backlight_ext_attrs[] = {
	&dev_attr_mode.attr,
	&dev_attr_brightness.attr,
	NULL,
};
ATTRIBUTE_GROUPS(backlight_ext);

static struct class backlight_ext_class = {
	.name		= "backlight-ext",
	.owner		= THIS_MODULE,
	.dev_groups	= backlight_ext_groups,
};

static DEFINE_IDA(minors);

static int gb_backlight_ext_connection_init(struct gb_connection *connection)
{
	struct gb_backlight_ext_device *backlight_ext;
	struct device *dev;
	int retval;

	backlight_ext = kzalloc(sizeof(*backlight_ext), GFP_KERNEL);
	if (!backlight_ext)
		return -ENOMEM;

	backlight_ext->connection = connection;
	connection->private = backlight_ext;

	backlight_ext->minor = ida_simple_get(&minors, 0, 0, GFP_KERNEL);
	if (backlight_ext->minor < 0) {
		retval = backlight_ext->minor;
		goto error;
	}

	dev = device_create(&backlight_ext_class, &connection->bundle->dev,
		MKDEV(0, 0), backlight_ext, "backlight-ext%d",
		backlight_ext->minor);
	if (IS_ERR(dev)) {
		retval = PTR_ERR(dev);
		goto err_ida_remove;
	}
	backlight_ext->dev = dev;

	return 0;

err_ida_remove:
	ida_simple_remove(&minors, backlight_ext->minor);
error:
	kfree(backlight_ext);
	return retval;
}

static void gb_backlight_ext_connection_exit(struct gb_connection *connection)
{
	struct gb_backlight_ext_device *backlight_ext = connection->private;

	device_unregister(backlight_ext->dev);
	ida_simple_remove(&minors, backlight_ext->minor);
	kfree(backlight_ext);
}

static struct gb_protocol backlight_ext_protocol = {
	.name			= "backlight-ext",
	.id			= GREYBUS_PROTOCOL_BACKLIGHT_EXT,
	.major			= GB_BACKLIGHT_EXT_VERSION_MAJOR,
	.minor			= GB_BACKLIGHT_EXT_VERSION_MINOR,
	.connection_init	= gb_backlight_ext_connection_init,
	.connection_exit	= gb_backlight_ext_connection_exit,
};

static __init int backlight_ext_protocol_init(void)
{
	int ret;

	ret = class_register(&backlight_ext_class);
	if (ret)
		return ret;

	ret = gb_protocol_register(&backlight_ext_protocol);
	if (ret)
		class_unregister(&backlight_ext_class);

	return ret;
}
module_init(backlight_ext_protocol_init);

static __exit void backlight_ext_protocol_exit(void)
{
	gb_protocol_deregister(&backlight_ext_protocol);
	class_unregister(&backlight_ext_class);
	ida_destroy(&minors);
}
module_exit(backlight_ext_protocol_exit);

MODULE_LICENSE("GPL v2");
