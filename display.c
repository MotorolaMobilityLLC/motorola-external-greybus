/*
 * Greybus Display protocol driver.
 *
 * Copyright 2015 Motorola LLC
 *
 * Released under the GPLv2 only.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/device.h>
#include <linux/kdev_t.h>
#include <linux/idr.h>

#include <linux/mod_display_comm.h>

#include "greybus.h"

struct gb_display_device {
	struct gb_connection	*connection;
	struct device		*dev;
	int			minor;		/* display minor number */
};

/* Version of the Greybus display protocol we support */
#define	GB_DISPLAY_VERSION_MAJOR		0x00
#define	GB_DISPLAY_VERSION_MINOR		0x01

/* Greybus Display operation types */
#define	GB_DISPLAY_HOST_READY			0x02
#define	GB_DISPLAY_GET_CONFIG_SIZE		0x03
#define	GB_DISPLAY_GET_CONFIG			0x04
#define	GB_DISPLAY_SET_CONFIG			0x05
#define	GB_DISPLAY_GET_STATE			0x06
#define	GB_DISPLAY_SET_STATE			0x07
#define	GB_DISPLAY_NOTIFICATION			0x08

#define GB_DISPLAY_NOTIFY_FAILURE		0x01
#define GB_DISPLAY_NOTIFY_AVAILABLE		0x02
#define GB_DISPLAY_NOTIFY_UNAVAILABLE		0x03
#define GB_DISPLAY_NOTIFY_CONNECT		0x04
#define GB_DISPLAY_NOTIFY_DISCONNECT		0x05

/* host ready request has no payload */
/* host ready request has no response */
static int host_ready(void *data)
{
	struct gb_display_device *disp = (struct gb_display_device *)data;
	int ret;

	ret = gb_operation_sync(disp->connection, GB_DISPLAY_HOST_READY,
				NULL, 0, NULL, 0);

	return ret;
}

/* get display config size request has no payload */
struct gb_display_get_display_config_size_response {
	__le32	size;
} __packed;

/* get display config request has no payload */
struct gb_display_get_display_config_response {
	__u8	display_type;
	__u8	config_type;
	__u8	reserved[2];
	__u8	data[0];
} __packed;

#define MAX_DISPLAY_CONFIG_SIZE 1024

static int get_display_config(void *data, struct mod_display_panel_config **display_config)
{
	struct gb_display_device *disp = (struct gb_display_device *)data;
	struct gb_display_get_display_config_size_response size_response;
	struct gb_display_get_display_config_response *config_response;
	struct mod_display_panel_config *config;
	u32 config_size;
	u32 config_response_size;
	int ret;

	ret = gb_operation_sync(disp->connection, GB_DISPLAY_GET_CONFIG_SIZE,
				 NULL, 0, &size_response, sizeof(size_response));
	if (ret)
		goto exit;

	config_size = le32_to_cpu(size_response.size);
	if (config_size > MAX_DISPLAY_CONFIG_SIZE) {
		dev_err(disp->dev, "Config size too large: %d\n", config_size);
		ret = -EINVAL;
		goto exit;
	}

	config_response_size = config_size + sizeof(*config_response);

	config_response = kmalloc(config_response_size, GFP_KERNEL);
	if (!config_response) {
		ret = -ENOMEM;
		goto exit;
	}

	ret = gb_operation_sync(disp->connection, GB_DISPLAY_GET_CONFIG,
				 NULL, 0, config_response, config_response_size);

	if (ret)
		goto free_config_response;

	config = kzalloc(config_size + sizeof(*config), GFP_KERNEL);
	if (!config) {
		ret = -ENOMEM;
		goto free_config_response;
	}

	config->display_type = config_response->display_type;
	config->config_type = config_response->config_type;
	config->edid_buf_size = config_size;
	memcpy(config->edid_buf, config_response->data, config_size);

	*display_config = config;

free_config_response:
	kfree(config_response);
exit:
	return ret;
}

struct gb_display_set_display_config_request {
	__u8	index;
} __packed;
/* set display config has no response */

static int set_display_config(void *data, u8 index)
{
	struct gb_display_device *disp = (struct gb_display_device *)data;
	struct gb_display_set_display_config_request request;
	int ret;

	request.index = index;

	ret = gb_operation_sync(disp->connection, GB_DISPLAY_SET_CONFIG,
				&request, sizeof(request), NULL, 0);

	return ret;
}

/* get display config request has no payload */
struct gb_display_get_display_state_response {
	__u8	state;
} __packed;

static int get_display_state(void *data, u8 *state)
{
	struct gb_display_device *disp = (struct gb_display_device *)data;
	struct gb_display_get_display_state_response response;
	int ret;

	ret = gb_operation_sync(disp->connection, GB_DISPLAY_GET_STATE,
				NULL, 0, &response, sizeof(response));
	if (ret)
		goto exit;

	*state = response.state;

exit:
	return ret;
}

struct gb_display_set_display_state_request {
	__u8	state;
} __packed;
/* set display state has no response */

static int set_display_state(void *data, u8 state)
{
	struct gb_display_device *disp = (struct gb_display_device *)data;
	struct gb_display_set_display_state_request request;
	int ret;

	request.state = state;

	ret = gb_operation_sync(disp->connection, GB_DISPLAY_SET_STATE,
				&request, sizeof(request), NULL, 0);

	return ret;
}

static ssize_t display_config_show(struct device *dev,
				   struct device_attribute *attr, char *buf)
{
	struct gb_display_device *disp = dev_get_drvdata(dev);
	struct mod_display_panel_config *display_config;
	char dump_buf[64];
	char *ptr;
	int cnt, tot = 0;
	size_t len;
	int ret;

	ret = get_display_config((void *)disp, &display_config);
	if (ret) {
		dev_err(dev, "Failed to get config: %d\n", ret);
		return ret;
	}

	ptr = display_config->edid_buf;

	for (cnt = display_config->edid_buf_size; cnt > 0; cnt -= 16) {
		hex_dump_to_buffer(ptr, min(cnt, 16), 16, 1, dump_buf, sizeof(dump_buf), false);
		len = scnprintf(buf + tot, PAGE_SIZE - tot, "%s\n", dump_buf);
		ptr += 16;
		tot += len;
		if (tot >= PAGE_SIZE)
			break;
	}

	kfree(display_config);

	return tot;
}
static DEVICE_ATTR_RO(display_config);

static ssize_t connect_store(struct device *dev, struct device_attribute *attr,
	const char *buf, size_t count)
{
	u32 connect;
	int ret;

	ret = kstrtou32(buf, 10, &connect);
	if (ret < 0) {
		dev_err(dev, "%s: Could not parse connect value %d\n", __func__,
			ret);
		return ret;
	}

	switch (connect) {
	case 0:
		dev_info(dev, "%s: Forcing disconnect\n", __func__);
		mod_display_notification(MOD_NOTIFY_DISCONNECT);
		break;
	case 1:
		dev_info(dev, "%s: Forcing connect\n", __func__);
		mod_display_notification(MOD_NOTIFY_CONNECT);
		break;
	default:
		dev_err(dev, "%s: Invalid value: %d\n", __func__, connect);
		return -EINVAL;
	}

	return count;
}
static DEVICE_ATTR_WO(connect);

static ssize_t display_state_store(struct device *dev, struct device_attribute *attr,
	const char *buf, size_t count)
{
	struct gb_display_device *disp = dev_get_drvdata(dev);
	u32 display_state;
	int ret;

	ret = kstrtou32(buf, 10, &display_state);
	if (ret < 0) {
		dev_err(dev, "%s: Could not parse display_state value %d\n", __func__,
			ret);
		return ret;
	}

	switch (display_state) {
	case 0:
		dev_info(dev, "%s: Setting display state OFF\n", __func__);
		set_display_state(disp, 0);
		break;
	case 1:
		dev_info(dev, "%s: Setting display state ON\n", __func__);
		set_display_state(disp, 1);
		break;
	default:
		dev_err(dev, "%s: Invalid value: %d\n", __func__, display_state);
		return -EINVAL;
	}

	return count;
}
static DEVICE_ATTR_WO(display_state);

static struct attribute *display_attrs[] = {
	&dev_attr_display_config.attr,
	&dev_attr_connect.attr,
	&dev_attr_display_state.attr,
	NULL,
};
ATTRIBUTE_GROUPS(display);

static struct class display_class = {
	.name		= "display",
	.owner		= THIS_MODULE,
	.dev_groups	= display_groups,
};

static DEFINE_IDA(minors);

struct gb_display_notification_request {
	__u8	event;
} __packed;

static int gb_display_event_recv(u8 type, struct gb_operation *op)
{
	struct gb_connection *connection = op->connection;
	struct gb_display_notification_request *request;

	/* By convention, the AP initiates the version operation */
	switch (type) {
	case GB_REQUEST_TYPE_PROTOCOL_VERSION:
		dev_err(&connection->bundle->dev,
			"module-initiated version operation\n");
		return -EINVAL;
	case GB_DISPLAY_NOTIFICATION:
		request = op->request->payload;
		switch(request->event) {
		case GB_DISPLAY_NOTIFY_FAILURE:
			dev_err(&connection->bundle->dev,
				"GB_DISPLAY_NOTIFY_FAILURE\n");
			mod_display_notification(MOD_NOTIFY_FAILURE);
			break;
		case GB_DISPLAY_NOTIFY_AVAILABLE:
			dev_dbg(&connection->bundle->dev,
				"GB_DISPLAY_NOTIFY_AVAILABLE\n");
			mod_display_notification(MOD_NOTIFY_AVAILABLE);
			break;
		case GB_DISPLAY_NOTIFY_UNAVAILABLE:
			dev_dbg(&connection->bundle->dev,
				"GB_DISPLAY_NOTIFY_UNAVAILABLE\n");
			mod_display_notification(MOD_NOTIFY_UNAVAILABLE);
			break;
		case GB_DISPLAY_NOTIFY_CONNECT:
			dev_dbg(&connection->bundle->dev,
				"GB_DISPLAY_NOTIFY_CONNECT\n");
			mod_display_notification(MOD_NOTIFY_CONNECT);
			break;
		case GB_DISPLAY_NOTIFY_DISCONNECT:
			dev_dbg(&connection->bundle->dev,
				"GB_DISPLAY_NOTIFY_DISCONNECT\n");
			mod_display_notification(MOD_NOTIFY_DISCONNECT);
			break;
		default:
			dev_err(&connection->bundle->dev,
				"unsupported event: %u\n", request->event);
			return -EINVAL;
		}
		return 0;
	default:
		dev_err(&connection->bundle->dev,
			"unsupported request: %hhu\n", type);
		return -EINVAL;
	}
}

struct mod_display_comm_ops mod_display_comm_ops = {
	.host_ready = host_ready,
	.get_display_config = get_display_config,
	.set_display_config = set_display_config,
	.get_display_state = get_display_state,
	.set_display_state = set_display_state,
};

static struct mod_display_comm_data mod_display_comm = {
	.ops = &mod_display_comm_ops,
};

static int gb_display_connection_init(struct gb_connection *connection)
{
	struct gb_display_device *disp;
	struct device *dev;
	int retval;

	if (mod_display_comm_ops.data) {
		pr_err("%s: Only one display connection is supported at a time",
			__func__);
		return -EBUSY;
	}

	disp = kzalloc(sizeof(*disp), GFP_KERNEL);
	if (!disp)
		return -ENOMEM;

	disp->connection = connection;
	connection->private = disp;

	disp->minor = ida_simple_get(&minors, 0, 0, GFP_KERNEL);
	if (disp->minor < 0) {
		retval = disp->minor;
		goto error;
	}
	dev = device_create(&display_class, &connection->bundle->dev, MKDEV(0, 0), disp,
			    "display%d", disp->minor);
	if (IS_ERR(dev)) {
		retval = PTR_ERR(dev);
		goto err_ida_remove;
	}
	disp->dev = dev;

	mod_display_comm_ops.data = disp;
	mod_display_register_comm(&mod_display_comm);

	return 0;

err_ida_remove:
	ida_simple_remove(&minors, disp->minor);
error:
	kfree(disp);
	return retval;
}

static void gb_display_connection_exit(struct gb_connection *connection)
{
	struct gb_display_device *disp = connection->private;

	mod_display_unregister_comm(&mod_display_comm);
	mod_display_comm_ops.data = NULL;

#if LINUX_VERSION_CODE <= KERNEL_VERSION(3,11,0)
	sysfs_remove_group(&disp->dev->kobj, display_groups[0]);
#endif
	device_unregister(disp->dev);
	ida_simple_remove(&minors, disp->minor);
	kfree(disp);
}

static struct gb_protocol display_protocol = {
	.name			= "display",
	.id			= GREYBUS_PROTOCOL_MODS_DISPLAY,
	.major			= GB_DISPLAY_VERSION_MAJOR,
	.minor			= GB_DISPLAY_VERSION_MINOR,
	.connection_init	= gb_display_connection_init,
	.connection_exit	= gb_display_connection_exit,
	.request_recv		= gb_display_event_recv,
};

static __init int protocol_init(void)
{
	int retval;

	retval = class_register(&display_class);
	if (retval)
		return retval;

	return gb_protocol_register(&display_protocol);
}
module_init(protocol_init);

static __exit void protocol_exit(void)
{
	gb_protocol_deregister(&display_protocol);
	class_unregister(&display_class);
	ida_destroy(&minors);
}
module_exit(protocol_exit);

MODULE_LICENSE("GPL v2");