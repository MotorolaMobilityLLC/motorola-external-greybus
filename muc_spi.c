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
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301, USA.
 */

#include <linux/delay.h>
#include <linux/err.h>
#include <linux/module.h>
#include <linux/of_gpio.h>
#include <linux/of_irq.h>
#include <linux/slice_attach.h>
#include <linux/spi/spi.h>

#include "endo.h"
#include "greybus.h"
#include "svc_msg.h"

#define MUC_MSG_SIZE_MAX	(1024)
#define MUC_PAYLOAD_SIZE_MAX	(MUC_MSG_SIZE_MAX - sizeof(struct muc_msg))

/* Size of payload of individual SPI packet (in bytes) */
#define MUC_SPI_PAYLOAD_SZ_MAX	(32)

#define HDR_BIT_VALID		(0x01 << 7)
#define HDR_BIT_MORE		(0x01 << 6)
#define HDR_BIT_RSVD		(0x3F << 0)

/* SVC message header + 2 bytes of payload */
#define HP_BASE_SIZE		(sizeof(struct svc_msg_header) + 2)

#define LU_PAYLOAD_SIZE         (sizeof(struct svc_function_unipro_management))
#define LU_MSG_SIZE             (sizeof(struct svc_msg_header) + LU_PAYLOAD_SIZE)

#define MIN(a, b)		(((a) < (b)) ? (a) : (b))

struct muc_spi_data {
	struct spi_device *spi;
	struct greybus_host_device *hd;
	bool present;
	struct notifier_block attach_nb;   /* attach/detach notifications */

	int gpio_wake_n;
	int gpio_rdy_n;

	__u8 has_tranceived;

	/* Preallocated buffer for network messages */
	__u8 network_buffer[MUC_MSG_SIZE_MAX];

	/*
	 * Buffer to hold incoming payload (which could be spread across
	 * multiple packets)
	 */
	__u8 rcvd_payload[MUC_MSG_SIZE_MAX];
	int rcvd_payload_idx;
};

#pragma pack(push, 1)
struct muc_msg
{
  __le16  size;
  __u8    dest_cport;
  __u8    src_cport;
  __u8    gb_msg[0];
};

struct muc_spi_msg
{
  __u8    hdr_bits;
  __u8    data[MUC_SPI_PAYLOAD_SZ_MAX];
  __le16  crc16;
};
#pragma pack(pop)

/* vendor only manifest */
static unsigned char manifest[] = {
  0x4c, 0x00, 0x00, 0x01, 0x08, 0x00, 0x01, 0x00, 0x01, 0x02, 0x00, 0x00,
  0x1c, 0x00, 0x02, 0x00, 0x16, 0x01, 0x4d, 0x6f, 0x74, 0x6f, 0x72, 0x6f,
  0x6c, 0x61, 0x20, 0x4d, 0x6f, 0x62, 0x69, 0x6c, 0x69, 0x74, 0x79, 0x2c,
  0x20, 0x4c, 0x4c, 0x43, 0x14, 0x00, 0x02, 0x00, 0x0e, 0x02, 0x45, 0x76,
  0x65, 0x72, 0x79, 0x64, 0x61, 0x79, 0x20, 0x53, 0x6c, 0x69, 0x63, 0x65,
  0x08, 0x00, 0x04, 0x00, 0x06, 0x00, 0x00, 0xff, 0x08, 0x00, 0x03, 0x00,
  0x00, 0x00, 0x00, 0x00
};
#define MANIFEST_SIZE 76

static void parse_rx_dl(struct greybus_host_device *hd, uint8_t *rx_buf);

static inline struct muc_spi_data *hd_to_dd(struct greybus_host_device *hd)
{
	return (struct muc_spi_data *)&hd->hd_priv;
}

static int muc_spi_transfer(struct greybus_host_device *hd, uint8_t *tx_buf)
{
	struct muc_spi_data *dd = hd_to_dd(hd);
	uint8_t rx_buf[sizeof(struct muc_spi_msg)];
	struct spi_transfer t[] = {
		{
			.tx_buf = tx_buf,
			.rx_buf = rx_buf,
			.len = sizeof(struct muc_spi_msg),
		},
	};
	int ret;

	if (dd->has_tranceived) {
		/* Wait for RDY to be deasserted */
		while (!gpio_get_value(dd->gpio_rdy_n));
	}
	dd->has_tranceived = 1;

	/* Assert WAKE */
	gpio_set_value(dd->gpio_wake_n, 0);

	/* Wait for RDY to be asserted */
	while (gpio_get_value(dd->gpio_rdy_n));

	/* Deassert WAKE */
	gpio_set_value(dd->gpio_wake_n, 1);

	ret = spi_sync_transfer(dd->spi, t, 1);

	if (!ret) {
		parse_rx_dl(hd, rx_buf);
	}

	return ret;
}

static void parse_rx_nw(struct greybus_host_device *hd, uint8_t *buf, int len)
{
	struct muc_msg *m = (struct muc_msg *)buf;

	if (m->size >= len) {
		printk(KERN_ERR "%s: Received an invalid message\n", __func__);
		return;
	}

	greybus_data_rcvd(hd, m->dest_cport, m->gb_msg, m->size);
}

static void parse_rx_dl(struct greybus_host_device *hd, uint8_t *buf)
{
	struct muc_spi_data *dd = hd_to_dd(hd);
	struct muc_spi_msg *m = (struct muc_spi_msg *)buf;

	if (!(m->hdr_bits & HDR_BIT_VALID)) {
		/* Received a dummy packet - nothing to do! */
		return;
	}

	if (dd->rcvd_payload_idx >= MUC_MSG_SIZE_MAX) {
		printk(KERN_ERR "%s: Too many packets received!\n", __func__);
		return;
	}

	memcpy(&dd->rcvd_payload[dd->rcvd_payload_idx], m->data,
	       MUC_SPI_PAYLOAD_SZ_MAX);
	dd->rcvd_payload_idx += MUC_SPI_PAYLOAD_SZ_MAX;

	if (m->hdr_bits & HDR_BIT_MORE) {
		/* Need additional packets */
		muc_spi_transfer(hd, NULL);
		return;
	}

	parse_rx_nw(hd, dd->rcvd_payload, dd->rcvd_payload_idx);
	memset(dd->rcvd_payload, 0, MUC_MSG_SIZE_MAX);
	dd->rcvd_payload_idx = 0;
}

static irqreturn_t muc_spi_isr(int irq, void *data)
{
	struct greybus_host_device *hd = data;
	struct muc_spi_data *dd = hd_to_dd(hd);

	/* Any interrupt while the MuC is not attached would be spurious */
	if (!dd->present)
		return IRQ_HANDLED;

	muc_spi_transfer(hd, NULL);
	return IRQ_HANDLED;
}

static void send_hot_plug(struct greybus_host_device *hd, int iid)
{
	struct svc_msg *msg;

	msg = kzalloc(HP_BASE_SIZE + MANIFEST_SIZE, GFP_KERNEL);
	if (!msg)
		return;

	msg->header.function_id = SVC_FUNCTION_HOTPLUG;
	msg->header.message_type = SVC_MSG_DATA;
	msg->header.payload_length = MANIFEST_SIZE + 2;
	msg->hotplug.hotplug_event = SVC_HOTPLUG_EVENT;
	msg->hotplug.interface_id = iid;
	memcpy(msg->hotplug.data, manifest, MANIFEST_SIZE);

	/* Send up hotplug message */
	greybus_svc_in(hd, (u8 *)msg, HP_BASE_SIZE + MANIFEST_SIZE);

	pr_info("SVC -> AP hotplug event (plug) sent\n");
	kfree(msg);
}

static void send_hot_unplug(struct greybus_host_device *hd, int iid)
{
	struct svc_msg msg;

	msg.header.function_id = SVC_FUNCTION_HOTPLUG;
	msg.header.message_type = SVC_MSG_DATA;
	msg.header.payload_length = 2;
	msg.hotplug.hotplug_event = SVC_HOTUNPLUG_EVENT;
	msg.hotplug.interface_id = iid;

	/* Send up hotplug message */
	greybus_svc_in(hd, (u8 *)&msg, HP_BASE_SIZE);

	pr_info("SVC -> AP hotplug event (unplug) sent\n");
}

static void send_link_up(struct greybus_host_device *hd, int iid, int did)
{
	struct svc_msg msg;

	msg.header.function_id = SVC_FUNCTION_UNIPRO_NETWORK_MANAGEMENT;
	msg.header.message_type = SVC_MSG_DATA;
	msg.header.payload_length = LU_PAYLOAD_SIZE;
	msg.management.management_packet_type = SVC_MANAGEMENT_LINK_UP;
	msg.management.link_up.interface_id = iid;
	msg.management.link_up.device_id = did;

	/* Send up link up message */
	greybus_svc_in(hd, (u8 *)&msg,  LU_MSG_SIZE);

	pr_info("SVC -> AP Link Up (%d:%d) message sent\n", iid, did);
}

static int muc_attach(struct notifier_block *nb,
		      unsigned long now_present, void *not_used)
{
	struct muc_spi_data *dd = container_of(nb, struct muc_spi_data, attach_nb);
	struct greybus_host_device *hd = dd->hd;
	struct spi_device *spi = dd->spi;

	if (now_present != dd->present) {
		printk("%s: MuC attach state = %lu\n", __func__, now_present);

		dd->present = now_present;

		if (now_present) {
			if (devm_request_threaded_irq(&spi->dev, spi->irq,
						      NULL, muc_spi_isr,
						      IRQF_TRIGGER_LOW |
						      IRQF_ONESHOT,
						      "muc_spi", hd))
				printk(KERN_ERR "%s: Unable to request irq.\n",
				       __func__);
			send_hot_plug(hd, 1);
			send_link_up(hd, 1, 2);
		} else {
			devm_free_irq(&spi->dev, spi->irq, hd);
			send_hot_unplug(hd, 1);
		}
	}
	return NOTIFY_OK;
}

static int muc_spi_message_send_dl(struct greybus_host_device *hd,
				   const void *buf, size_t len)
{
	struct muc_spi_msg *m;
	size_t remaining = len;

	while (remaining > 0) {
		m = kzalloc(sizeof(struct muc_spi_msg), GFP_KERNEL);
		if (!m)
			return -ENOMEM;

		m->hdr_bits |= HDR_BIT_VALID;
		m->hdr_bits |= (remaining > MUC_SPI_PAYLOAD_SZ_MAX) ? HDR_BIT_MORE : 0;
		memcpy(m->data, buf, MIN(len, MUC_SPI_PAYLOAD_SZ_MAX));
		m->crc16 = 0; /* TODO */

		muc_spi_transfer(hd, (uint8_t *)m);

		remaining -= MIN(len, MUC_SPI_PAYLOAD_SZ_MAX);
		kfree(m);
	}

	return 0;
}

static int muc_spi_message_send(struct greybus_host_device *hd, u16 hd_cport_id,
				  struct gb_message *message, gfp_t gfp_mask)
{
	struct muc_spi_data *dd = hd_to_dd(hd);
	struct gb_connection *connection;
	struct muc_msg *m = (struct muc_msg *)dd->network_buffer;

	connection = gb_connection_hd_find(hd, hd_cport_id);

	if (!connection) {
		pr_err("Invalid cport supplied to send\n");
		return -EINVAL;
	}

	m->size = sizeof(*message->header) + message->payload_size;
	m->dest_cport = connection->intf_cport_id;
	m->src_cport = connection->hd_cport_id;
	memcpy(m->gb_msg, message->buffer, m->size);

	printk("%s: AP (CPort %d) -> Module (CPort %d)\n",
	       __func__, connection->hd_cport_id, connection->intf_cport_id);

	muc_spi_message_send_dl(hd, m, m->size + sizeof(struct muc_msg));
	return 0;
}

/*
 * The cookie value supplied is the value that message_send()
 * returned to its caller.  It identifies the buffer that should be
 * canceled.  This function must also handle (which is to say,
 * ignore) a null cookie value.
 */
static void muc_spi_message_cancel(struct gb_message *message)
{
	printk("%s: enter\n", __func__);
}

static int muc_spi_submit_svc(struct svc_msg *svc_msg,
			      struct greybus_host_device *hd)
{
	/* Currently don't have an SVC! */
	return 0;
}

static struct greybus_host_driver muc_spi_host_driver = {
	.hd_priv_size		= sizeof(struct muc_spi_data),
	.message_send		= muc_spi_message_send,
	.message_cancel		= muc_spi_message_cancel,
	.submit_svc		= muc_spi_submit_svc,
};

static int muc_spi_gpio_init(struct muc_spi_data *dd)
{
	struct device_node *np = dd->spi->dev.of_node;
	int ret;

	dd->gpio_wake_n = of_get_gpio(np, 0);
	dd->gpio_rdy_n = of_get_gpio(np, 1);

	ret = gpio_request_one(dd->gpio_wake_n, GPIOF_OUT_INIT_HIGH,
			       "muc_wake_n");
	if (ret)
		return ret;

	return gpio_request_one(dd->gpio_rdy_n, GPIOF_IN, "muc_rdy_n");
}

static int muc_spi_probe(struct spi_device *spi)
{
	struct muc_spi_data *dd;
	struct greybus_host_device *hd;
	u16 endo_id = 0x4755;
	u8 ap_intf_id = 0x01;
	int retval;

	pr_info("%s: enter\n", __func__);

	if (spi->irq < 0) {
		pr_err("%s: IRQ not defined\n", __func__);
		return -EINVAL;
	}

	hd = greybus_create_hd(&muc_spi_host_driver, &spi->dev,
			       MUC_PAYLOAD_SIZE_MAX);
	if (IS_ERR(hd)) {
		printk(KERN_ERR "%s: Unable to create greybus host driver.\n",
		       __func__);
		return PTR_ERR(hd);
	}

	retval = greybus_endo_setup(hd, endo_id, ap_intf_id);
	if (retval)
		return retval;

	dd = hd_to_dd(hd);
	dd->hd = hd;
	dd->spi = spi;
	dd->attach_nb.notifier_call = muc_attach;
	muc_spi_gpio_init(dd);

	spi_set_drvdata(spi, dd);

	register_slice_attach_notifier(&dd->attach_nb);

	return 0;
}

static int muc_spi_remove(struct spi_device *spi)
{
	struct muc_spi_data *dd = spi_get_drvdata(spi);

	pr_info("%s: enter\n", __func__);

	gpio_free(dd->gpio_wake_n);
	gpio_free(dd->gpio_rdy_n);

	unregister_slice_attach_notifier(&dd->attach_nb);
	greybus_remove_hd(dd->hd);
	spi_set_drvdata(spi, NULL);

	return 0;
}

#ifdef CONFIG_OF
static const struct of_device_id of_muc_spi_match[] = {
	{ .compatible = "moto,muc_spi", },
	{},
};
#endif

static const struct spi_device_id muc_spi_id[] = {
	{ "muc_spi", 0 },
	{ }
};

static struct spi_driver muc_spi_driver = {
	.driver = {
		.owner = THIS_MODULE,
		.name = "muc_spi",
		.of_match_table = of_match_ptr(of_muc_spi_match),
	},
	.id_table = muc_spi_id,
	.probe = muc_spi_probe,
	.remove  = muc_spi_remove,
};

module_spi_driver(muc_spi_driver);

MODULE_AUTHOR("Motorola Mobility, LLC");
MODULE_DESCRIPTION("Mods uC (MuC) SPI bus driver");
MODULE_LICENSE("GPL");