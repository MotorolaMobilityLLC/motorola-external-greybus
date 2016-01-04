/*
 * Copyright (C) 2015 Motorola Mobility, Inc.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#include <linux/delay.h>
#include <linux/err.h>
#include <linux/module.h>
#include <linux/platform_device.h>

#include "greybus.h"
#include "mods_nw.h"

static struct gb_host_device *g_hd;

struct mods_ap_data {
	struct mods_dl_device *dld;
	struct gb_host_device *hd;
};

/* got a message from the nw switch forward it to greybus */
static int mods_ap_message_send(struct mods_dl_device *dld,
		uint8_t *buf, size_t len)
{
	struct muc_msg *msg = (struct muc_msg *)buf;

	greybus_data_rcvd(g_hd, msg->hdr.dest_cport, msg->gb_msg, msg->hdr.size);
	return 0;
}

static struct mods_dl_driver mods_ap_dl_driver = {
	.dl_priv_size		= sizeof(struct mods_ap_data),
	.message_send		= mods_ap_message_send,
};

/* received a message from the AP to send to the switch */
static int mods_ap_msg_send(struct gb_host_device *hd,
		u16 hd_cport_id,
		struct gb_message *message,
		gfp_t gfp_mask)
{
	size_t buffer_size;
	struct gb_connection *connection;
	struct muc_msg *msg;
	struct mods_ap_data *data;
	struct mods_dl_device *dl;
	int rv = -EINVAL;

	if (message->payload_size > PAYLOAD_MAX_SIZE)
		return -E2BIG;

	data = (struct mods_ap_data *)hd->hd_priv;
	dl = data->dld;

	connection = gb_connection_hd_find(hd, hd_cport_id);
	if (!connection) {
		pr_err("Invalid cport supplied to send\n");
		return -EINVAL;
	}

	buffer_size = sizeof(*message->header) + message->payload_size;

	msg = (struct muc_msg *)kzalloc(buffer_size +
			sizeof(struct muc_msg_hdr), gfp_mask);
	if (!msg)
		return -ENOMEM;

	msg->hdr.dest_cport = connection->intf_cport_id;
	msg->hdr.src_cport = connection->hd_cport_id;
	msg->hdr.size = buffer_size + sizeof(struct muc_msg_hdr);
	memcpy(&msg->gb_msg[0], message->buffer, buffer_size);

	/* hand off to the nw layer */
	rv = mods_nw_switch(dl, (uint8_t *)msg);

	/* Tell submitter that the message send (attempt) is
	 * complete and save the status.
	 */
	greybus_message_sent(hd, message, rv);

	kfree(msg);

	return rv;
}

static void mods_ap_msg_cancel(struct gb_message *message)
{
	/* nothing currently */
}

static struct gb_hd_driver mods_ap_host_driver = {
	.hd_priv_size		= sizeof(struct mods_ap_data),
	.message_send		= mods_ap_msg_send,
	.message_cancel		= mods_ap_msg_cancel,
};

static int mods_ap_probe(struct platform_device *pdev)
{
	struct mods_ap_data *ap_data;

	/* setup host device */
	g_hd = greybus_create_hd(&mods_ap_host_driver, &pdev->dev,
			PAYLOAD_MAX_SIZE);
	if (IS_ERR(g_hd)) {
		dev_err(&pdev->dev, "Unable to create greybus host driver.\n");
		return PTR_ERR(g_hd);
	}
	ap_data = (struct mods_ap_data *)&g_hd->hd_priv;
	ap_data->hd = g_hd;
	platform_set_drvdata(pdev, ap_data);

	/* create our data link device */
	ap_data->dld = mods_create_dl_device(&mods_ap_dl_driver,
			&pdev->dev, MODS_DL_ROLE_AP);
	if (IS_ERR(ap_data->dld)) {
		dev_err(&pdev->dev, "%s: Unable to create greybus host driver.\n",
		        __func__);
		return PTR_ERR(ap_data->dld);
	}
	
	return 0;
}

static int mods_ap_remove(struct platform_device *pdev)
{
	return 0;
}

static struct platform_driver mods_ap_driver = {
	.driver = {
		.owner = THIS_MODULE,
		.name = "mods_ap",
	},
	.probe = mods_ap_probe,
	.remove = mods_ap_remove,
};

static struct platform_device *mods_ap_device;

int __init mods_ap_init(void)
{
	int err;
	err = platform_driver_register(&mods_ap_driver);
	if (err) {
		pr_err("mods ap failed to register driver\n");
		return err;
	}

	mods_ap_device = platform_device_alloc("mods_ap", -1);
	if (!mods_ap_device) {
		err = -ENOMEM;
		pr_err("mods ap failed to alloc device\n");
		goto alloc_fail;
	}

	err = platform_device_add(mods_ap_device);
	if (err) {
		pr_err("mods ap failed to add device: %d\n", err);
		goto add_fail;
	}

	return 0;
add_fail:
	platform_device_put(mods_ap_device);
alloc_fail:
	platform_driver_unregister(&mods_ap_driver);
	return err;
}

void __exit mods_ap_exit(void)
{
	platform_device_unregister(mods_ap_device);
	platform_driver_unregister(&mods_ap_driver);
}
