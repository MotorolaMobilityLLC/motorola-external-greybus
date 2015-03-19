/*
 * Greybus "AP" message loop handling
 *
 * Copyright 2014 Google Inc.
 * Copyright 2014 Linaro Ltd.
 *
 * Released under the GPLv2 only.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/types.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/workqueue.h>
#include <linux/device.h>
#include "svc_msg.h"
#include "greybus_manifest.h"
#include "greybus.h"

struct ap_msg {
	u8 *data;
	size_t size;
	struct greybus_host_device *hd;
	struct work_struct event;
};

static struct workqueue_struct *ap_workqueue;

static struct svc_msg *svc_msg_alloc(enum svc_function_id id)
{
	struct svc_msg *svc_msg;

	svc_msg = kzalloc((sizeof *svc_msg), GFP_KERNEL);
	if (!svc_msg)
		return NULL;

	// FIXME - verify we are only sending function IDs we should be
	svc_msg->header.function_id = id;
	return svc_msg;
}

static void svc_msg_free(struct svc_msg *svc_msg)
{
	kfree(svc_msg);
}

static int svc_msg_send(struct svc_msg *svc_msg, struct greybus_host_device *hd)
{
	int retval;

	// FIXME - Do we need to do more than just pass it to the hd and then
	// free it?
	retval = hd->driver->submit_svc(svc_msg, hd);

	svc_msg_free(svc_msg);
	return retval;
}


int svc_set_route_send(struct gb_bundle *bundle,
			       struct greybus_host_device *hd)
{
	struct svc_msg *svc_msg;

	svc_msg = svc_msg_alloc(SVC_FUNCTION_UNIPRO_NETWORK_MANAGEMENT);
	if (!svc_msg)
		return -ENOMEM;

	svc_msg->header.message_type = SVC_MSG_DATA;
	svc_msg->header.payload_length =
		cpu_to_le16(sizeof(struct svc_function_unipro_set_route));
	svc_msg->management.set_route.device_id = bundle->device_id;

	return svc_msg_send(svc_msg, hd);
}

static void svc_handshake(struct svc_function_handshake *handshake,
			  int payload_length, struct greybus_host_device *hd)
{
	struct svc_msg *svc_msg;

	if (payload_length != sizeof(*handshake)) {
		dev_err(hd->parent,
			"Illegal size of svc handshake message %d\n",
			payload_length);
		return;
	}

	/* A new SVC communication channel, let's verify a supported version */
	if ((handshake->version_major != GREYBUS_VERSION_MAJOR) ||
	    (handshake->version_minor != GREYBUS_VERSION_MINOR)) {
		dev_warn(hd->parent,
			"received invalid greybus version %u.%u\n",
			handshake->version_major, handshake->version_minor);
		return;
	}

	/* Validate that the handshake came from the SVC */
	if (handshake->handshake_type != SVC_HANDSHAKE_SVC_HELLO) {
		/* we don't know what to do with this, log it and return */
		dev_dbg(hd->parent, "received invalid handshake type %d\n",
			handshake->handshake_type);
		return;
	}

	/* Send back a AP_HELLO message */
	svc_msg = svc_msg_alloc(SVC_FUNCTION_HANDSHAKE);
	if (!svc_msg)
		return;

	svc_msg->header.message_type = SVC_MSG_DATA;
	svc_msg->header.payload_length =
		cpu_to_le16(sizeof(*handshake));
	svc_msg->handshake.version_major = GREYBUS_VERSION_MAJOR;
	svc_msg->handshake.version_minor = GREYBUS_VERSION_MINOR;
	svc_msg->handshake.handshake_type = SVC_HANDSHAKE_AP_HELLO;

	(void)svc_msg_send(svc_msg, hd);
}

static void svc_management(struct svc_function_unipro_management *management,
			   int payload_length, struct greybus_host_device *hd)
{
	struct gb_interface *intf;
	int ret;

	if (payload_length != sizeof(*management)) {
		dev_err(hd->parent,
			"Illegal size of svc management message %d\n",
			payload_length);
		return;
	}

	switch (management->management_packet_type) {
	case SVC_MANAGEMENT_AP_ID:
		hd->device_id = management->ap_id.device_id;
		break;
	case SVC_MANAGEMENT_LINK_UP:
		intf = gb_interface_find(hd, management->link_up.module_id);
		if (!intf) {
			dev_err(hd->parent, "Module ID %d not found\n",
				management->link_up.module_id);
			return;
		}
		ret = gb_bundle_init(intf,
				management->link_up.interface_id,
				management->link_up.device_id);
		if (ret)
			dev_err(hd->parent,
				"error %d initializing interface %hhu bundle %hhu\n",
				ret, management->link_up.module_id,
				management->link_up.interface_id);
		break;
	default:
		dev_err(hd->parent, "Unhandled UniPro management message\n");
	}
}

static void svc_hotplug(struct svc_function_hotplug *hotplug,
			int payload_length, struct greybus_host_device *hd)
{
	u8 module_id = hotplug->module_id;

	switch (hotplug->hotplug_event) {
	case SVC_HOTPLUG_EVENT:
		/* Add a new module to the system */
		if (payload_length < 0x03) {
			/* Hotplug message is at lest 3 bytes big */
			dev_err(hd->parent,
				"Illegal size of svc hotplug message %d\n",
				payload_length);
			return;
		}
		dev_dbg(hd->parent, "module id %d added\n", module_id);
		gb_add_interface(hd, module_id, hotplug->data,
				 payload_length - 0x02);
		break;

	case SVC_HOTUNPLUG_EVENT:
		/* Remove a module from the system */
		if (payload_length != 0x02) {
			/* Hotunplug message is only 2 bytes big */
			dev_err(hd->parent,
				"Illegal size of svc hotunplug message %d\n",
				payload_length);
			return;
		}
		dev_dbg(hd->parent, "module id %d removed\n", module_id);
		gb_remove_interface(hd, module_id);
		break;

	default:
		dev_err(hd->parent,
			"Received invalid hotplug message type %d\n",
			hotplug->hotplug_event);
		break;
	}
}

static void svc_power(struct svc_function_power *power,
		      int payload_length, struct greybus_host_device *hd)
{
	u8 module_id = power->module_id;

	/*
	 * The AP is only allowed to get a Battery Status message, not a Battery
	 * Status Request
	 */
	if (power->power_type != SVC_POWER_BATTERY_STATUS) {
		dev_err(hd->parent, "Received invalid power type %d\n",
			power->power_type);
		return;
	}

	/*
	 * As struct struct svc_function_power_battery_status_request is 0 bytes
	 * big, we can just check the union of the whole structure to validate
	 * the size of this message.
	 */
	if (payload_length != sizeof(*power)) {
		dev_err(hd->parent,
			"Illegal size of svc power message %d\n",
			payload_length);
		return;
	}

	dev_dbg(hd->parent, "power status for module id %d is %d\n",
		module_id, power->status.status);

	// FIXME - do something with the power information, like update our
	// battery information...
}

static void svc_epm(struct svc_function_epm *epm,
		    int payload_length, struct greybus_host_device *hd)
{
	/* What?  An AP should not get this message */
	dev_err(hd->parent, "Got an EPM message???\n");
}

static void svc_suspend(struct svc_function_suspend *suspend,
			int payload_length, struct greybus_host_device *hd)
{
	/* What?  An AP should not get this message */
	dev_err(hd->parent, "Got an suspend message???\n");
}

static struct svc_msg *convert_ap_message(struct ap_msg *ap_msg)
{
	struct svc_msg *svc_msg;
	struct svc_msg_header *header;
	struct greybus_host_device *hd = ap_msg->hd;

	svc_msg = (struct svc_msg *)ap_msg->data;
	header = &svc_msg->header;

	/* Validate the message type */
	if (header->message_type != SVC_MSG_DATA) {
		dev_err(hd->parent, "message type %d received?\n",
			header->message_type);
		return NULL;
	}

	/*
	 * The validation of the size of the message buffer happens in each
	 * svc_* function, due to the different types of messages, keeping the
	 * logic for each message only in one place.
	 */

	return svc_msg;
}

static void ap_process_event(struct work_struct *work)
{
	struct svc_msg *svc_msg;
	struct greybus_host_device *hd;
	struct ap_msg *ap_msg;
	int payload_length;

	ap_msg = container_of(work, struct ap_msg, event);
	hd = ap_msg->hd;

	/* Turn the "raw" data into a real message */
	svc_msg = convert_ap_message(ap_msg);
	if (!svc_msg)
		return;

	payload_length = le16_to_cpu(svc_msg->header.payload_length);

	/* Look at the message to figure out what to do with it */
	switch (svc_msg->header.function_id) {
	case SVC_FUNCTION_HANDSHAKE:
		svc_handshake(&svc_msg->handshake, payload_length, hd);
		break;
	case SVC_FUNCTION_UNIPRO_NETWORK_MANAGEMENT:
		svc_management(&svc_msg->management, payload_length, hd);
		break;
	case SVC_FUNCTION_HOTPLUG:
		svc_hotplug(&svc_msg->hotplug, payload_length, hd);
		break;
	case SVC_FUNCTION_POWER:
		svc_power(&svc_msg->power, payload_length, hd);
		break;
	case SVC_FUNCTION_EPM:
		svc_epm(&svc_msg->epm, payload_length, hd);
		break;
	case SVC_FUNCTION_SUSPEND:
		svc_suspend(&svc_msg->suspend, payload_length, hd);
		break;
	default:
		dev_err(hd->parent, "received invalid SVC function ID %d\n",
			svc_msg->header.function_id);
	}

	/* clean the message up */
	kfree(ap_msg->data);
	kfree(ap_msg);
}

int greybus_svc_in(struct greybus_host_device *hd, u8 *data, int size)
{
	struct ap_msg *ap_msg;

	/*
	 * Totally naive copy the message into a new structure that we slowly
	 * create and add it to the list.  Let's get this working, the odds of
	 * this being any "slow path" for AP messages is really low at this
	 * point in time, but you never know, so this comment is here to point
	 * out that maybe we should use a slab allocator, or even just not copy
	 * the data, but use it directly and force the urbs to be "new" each
	 * time.
	 */

	/* Note - this can, and will, be called in interrupt context. */
	ap_msg = kmalloc(sizeof(*ap_msg), GFP_ATOMIC);
	if (!ap_msg)
		return -ENOMEM;
	ap_msg->data = kmalloc(size, GFP_ATOMIC);
	if (!ap_msg->data) {
		kfree(ap_msg);
		return -ENOMEM;
	}
	memcpy(ap_msg->data, data, size);
	ap_msg->size = size;
	ap_msg->hd = hd;

	INIT_WORK(&ap_msg->event, ap_process_event);
	queue_work(ap_workqueue, &ap_msg->event);

	return 0;
}
EXPORT_SYMBOL_GPL(greybus_svc_in);

int gb_ap_init(void)
{
	ap_workqueue = alloc_ordered_workqueue("greybus_ap", 0);
	if (!ap_workqueue)
		return -ENOMEM;

	return 0;
}

void gb_ap_exit(void)
{
	destroy_workqueue(ap_workqueue);
	ap_workqueue = NULL;
}


