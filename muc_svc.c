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
#include <linux/workqueue.h>

#include "greybus.h"

#include "muc_svc.h"
#include "mods_nw.h"

struct muc_svc_data {
	struct mods_dl_device *dld;
	atomic_t msg_num;
	struct list_head operations;
	struct platform_device *pdev;
	struct workqueue_struct *wq;
	struct kset *intf_kset;
	bool authenticate;
};
struct muc_svc_data *svc_dd;

/* Special Control Message to get IDs */
#define GB_CONTROL_TYPE_GET_IDS 0x7f
struct gb_control_get_ids_response {
	__le32    unipro_mfg_id;
	__le32    unipro_prod_id;
	__le32    ara_vend_id;
	__le32    ara_prod_id;
};

struct muc_svc_hotplug_work {
	struct work_struct work;
	struct mods_dl_device *dld;
	struct gb_svc_intf_hotplug_request hotplug;
};

/* XXX Move these into device tree? */
#define MUC_SVC_AP_INTF_ID 1
#define MUC_SVC_ENDO_ID 0x4755

#define MUC_SVC_RESPONSE_TYPE 0

#define SVC_MSG_TIMEOUT 5000

static struct gb_message *svc_gb_msg_alloc(u8 type, size_t payload_size)
{
	struct gb_message *msg;
	struct gb_operation_msg_hdr *hdr;
	size_t message_size = payload_size + sizeof(*hdr);

	msg = kzalloc(sizeof(*msg), GFP_KERNEL);
	if (!msg)
		return NULL;

	msg->buffer = kzalloc(message_size, GFP_KERNEL);
	if (!msg) {
		kfree(msg);
		return NULL;
	}

	hdr = msg->buffer;
	hdr->size = cpu_to_le16(message_size);
	hdr->operation_id = 0;
	hdr->type = type;
	hdr->result = 0;

	msg->header = hdr;
	msg->payload = payload_size ? hdr + 1 : NULL;
	msg->payload_size = payload_size;

	return msg;
}

static void svc_gb_msg_free(struct gb_message *msg)
{
	if (!msg)
		return;
	kfree(msg->buffer);
	kfree(msg);
}

struct svc_op {
	struct list_head entry;
	struct completion completion;
	struct gb_message *request;
	struct gb_message *response;
	u16 msg_id;
};

static inline struct muc_svc_data *dld_get_dd(struct mods_dl_device *dld)
{
	return (struct muc_svc_data *)dld->dl_priv;
}

static inline size_t get_gb_msg_size(struct gb_message *msg)
{
	return sizeof(*msg->header) + msg->payload_size;
}

static struct svc_op *svc_find_op(struct muc_svc_data *dd, uint16_t id)
{
	struct svc_op *e, *tmp;

	list_for_each_entry_safe(e, tmp, &dd->operations, entry)
		if (e->msg_id == id)
			return e;

	return NULL;
}

/* Route a gb_message to the mods_nw layer, adding the necessary
 * envelope that it understands.
 */
static int
svc_route_msg(struct mods_dl_device *dld, uint8_t src_cport,
		uint8_t dest_cport, struct gb_message *msg)
{
	struct muc_msg *m;
	size_t muc_payload = get_gb_msg_size(msg);
	size_t msg_size = muc_payload + sizeof(m->hdr);

	m = kmalloc(msg_size, GFP_KERNEL);
	if (!m)
		return -ENOMEM;

	memcpy(m->gb_msg, msg->buffer, muc_payload);
	m->hdr.gb_msg_size = msg->header->size;
	m->hdr.dest_cport = dest_cport;
	m->hdr.src_cport = src_cport;

	mods_nw_switch(dld, (uint8_t *)m);
	kfree(m);

	return 0;
}

static inline size_t get_gb_payload_size(size_t message_size)
{
	return message_size - sizeof(struct gb_operation_msg_hdr);
}

static int
svc_gb_conn_create(struct mods_dl_device *dld, struct gb_message *req,
		uint8_t cport, uint8_t reply)
{
	struct muc_svc_data *dd = dld_get_dd(dld);
	struct gb_svc_conn_create_request *conn = req->payload;
	struct gb_message *resp;
	int ret;

	dev_info(&dd->pdev->dev, "Create Connection: %hu:%hu to %hu:%hu\n",
			conn->intf1_id, conn->cport1_id,
			conn->intf2_id, conn->cport2_id);

	/* Create the two bi-directional connection routes */
	ret = mods_nw_add_route(conn->intf1_id, conn->cport1_id,
				conn->intf2_id, conn->cport2_id);
	if (ret) {
		dev_err(&dd->pdev->dev,
			"Failed to create route: %d:%d->%d.%d\n",
			conn->intf1_id, conn->cport1_id,
			conn->intf2_id, conn->cport2_id);
		return ret;
	}

	ret = mods_nw_add_route(conn->intf2_id, conn->cport2_id,
				conn->intf1_id, conn->cport1_id);
	if (ret) {
		dev_err(&dd->pdev->dev,
			"Failed to create route: %d:%d->%d.%d\n",
			conn->intf2_id, conn->cport2_id,
			conn->intf1_id, conn->cport1_id);
		goto del_route_1;
	}

	resp = svc_gb_msg_alloc(GB_MESSAGE_TYPE_RESPONSE, 0);
	if (!resp) {
		ret = -ENOMEM;
		goto del_route_2;
	}

	/* Copy in the original request type and operation id */
	resp->header->type |= req->header->type;
	resp->header->operation_id = req->header->operation_id;

	ret = svc_route_msg(dld, cport, reply, resp);
	if (ret) {
		dev_err(&dd->pdev->dev,
			"Failed to send response for type: %d\n",
			req->header->type);
		goto free_response;
	}

	svc_gb_msg_free(resp);

	return 0;

free_response:
	svc_gb_msg_free(resp);
del_route_2:
	mods_nw_del_route(conn->intf1_id, conn->cport1_id,
			conn->intf2_id, conn->cport2_id);
del_route_1:
	mods_nw_del_route(conn->intf1_id, conn->cport1_id,
			conn->intf2_id, conn->cport2_id);

	return ret;
}

static int
muc_svc_handle_request(struct mods_dl_device *dld, uint8_t *data,
			size_t msg_size, uint8_t cport, uint8_t reply)
{
	struct muc_svc_data *dd = dld_get_dd(dld);
	size_t payload_size = get_gb_payload_size(msg_size);
	struct gb_operation_msg_hdr hdr;
	struct svc_op *op;
	int ret;

	op = kzalloc(sizeof(*op), GFP_KERNEL);
	if (!op)
		return -ENOMEM;

	memcpy(&hdr, data, sizeof(hdr));

	op->request = svc_gb_msg_alloc(hdr.type, payload_size);
	if (!op->request) {
		ret = -ENOMEM;
		goto gb_msg_alloc;
	}

	if (payload_size)
		memcpy(op->request->header, data, msg_size);

	switch (hdr.type) {
	case GB_SVC_TYPE_INTF_DEVICE_ID:
		/* XXX Handle interface to device ID mapping */
		break;
	case GB_SVC_TYPE_INTF_RESET:
		/* XXX Handle interface reset request */
		break;
	case GB_SVC_TYPE_CONN_CREATE:
		/* XXX Handle connection create intf:cport <-> intf:cport */
		ret = svc_gb_conn_create(dld, op->request, cport, reply);
		svc_gb_msg_free(op->request);
		kfree(op);
		return ret;
	case GB_SVC_TYPE_CONN_DESTROY:
		/* XXX Handle connection destroy */
		break;
	case GB_SVC_TYPE_ROUTE_CREATE:
		/* XXX Handle route create intf:devid <-> intf:devid */
		break;
	default:
		dev_err(&dd->pdev->dev, "Unsupported type: %d\n", hdr.type);
		goto unknown_type;
	}

	/* If hdr operation id is non-zero, it expects a response */
	if (hdr.operation_id) {
		op->response = svc_gb_msg_alloc(GB_MESSAGE_TYPE_RESPONSE, 0);

		/* Copy in the original request type and operation id */
		op->response->header->type |= hdr.type;
		op->response->header->operation_id = hdr.operation_id;

		ret = svc_route_msg(dld, cport, reply, op->response);
		if (ret) {
			dev_err(&dd->pdev->dev,
				"Failed to send response for type: %d\n",
				hdr.type);
			goto free_response;
		}

		/* Done with the response */
		svc_gb_msg_free(op->response);
	}

	/* Done with the request and op */
	svc_gb_msg_free(op->request);
	kfree(op);

	return 0;

free_response:
	svc_gb_msg_free(op->response);
unknown_type:
	svc_gb_msg_free(op->request);
gb_msg_alloc:
	kfree(op);

	return ret;
}

/* Handle the incoming greybus message and complete the waiting thread, or
 * process the new incoming request.
 */
static int
svc_gb_msg_recv(struct mods_dl_device *dld, uint8_t *data,
		size_t msg_size, uint8_t dest_cport, uint8_t src_cport)
{
	struct muc_svc_data *dd = dld_get_dd(dld);
	struct svc_op *op;
	size_t payload_size = get_gb_payload_size(msg_size);
	struct gb_operation_msg_hdr hdr;

	if (msg_size < sizeof(hdr)) {
		dev_err(&dd->pdev->dev, "msg size too small: %zu\n", msg_size);
		return -EINVAL;
	}

	memcpy(&hdr, data, sizeof(hdr));

	/* If this is a response, notify the the waiter */
	if (hdr.type & GB_MESSAGE_TYPE_RESPONSE) {
		op = svc_find_op(dd, le16_to_cpu(hdr.operation_id));
		if (!op) {
			dev_err(&dd->pdev->dev, "OpID: %d unknown\n",
				le16_to_cpu(hdr.operation_id));
			return -EINVAL;
		}

		op->response = svc_gb_msg_alloc(MUC_SVC_RESPONSE_TYPE, payload_size);
		if (!op->response)
			return -ENOMEM;

		memcpy(op->response->header, data, msg_size);
		complete(&op->completion);

		return 0;
	}

	/* If not a response, process the new request */
	return muc_svc_handle_request(dld, data, msg_size,
					dest_cport, src_cport);
}

/* Send a message out the specified CPORT and wait for a response */
static struct gb_message *
svc_gb_msg_send_sync(struct mods_dl_device *dld, uint8_t *data, uint8_t type,
		size_t payload_size, uint8_t src_cport, uint8_t dest_cport)
{
	struct muc_svc_data *dd = dld_get_dd(dld);
	struct svc_op *op;
	struct gb_message *msg;
	int ret;
	uint16_t cycle;

	op = kzalloc(sizeof(*op), GFP_KERNEL);
	if (!op)
		return ERR_PTR(-ENOMEM);

	msg = svc_gb_msg_alloc(type, payload_size);
	if (!msg) {
		ret = -ENOMEM;
		goto gb_msg_alloc;
	}

	cycle = (u16)atomic_inc_return(&dd->msg_num);
	op->msg_id = cycle % U16_MAX + 1;
	init_completion(&op->completion);

	msg->header->operation_id = cpu_to_le16(op->msg_id);
	if (payload_size)
		memcpy(msg->payload, data, payload_size);
	op->request = msg;

	list_add_tail(&op->entry, &dd->operations);

	/* Send to NW Routing Layer */
	ret = svc_route_msg(dld, src_cport, dest_cport, msg);
	if (ret) {
		dev_err(&dd->pdev->dev, "failed sending svc msg: %d\n", ret);
		goto remove_op;
	}

	ret = wait_for_completion_interruptible_timeout(&op->completion,
					msecs_to_jiffies(SVC_MSG_TIMEOUT));
	if (ret <= 0) {
		dev_err(&dd->pdev->dev, "svc msg response timeout\n");
		if (!ret)
			ret = -ETIMEDOUT;
		goto remove_op;
	}

	/* Remove and free the request */
	list_del(&op->entry);
	svc_gb_msg_free(op->request);
	op->request = NULL;

	msg = op->response;
	kfree(op);

	/* XXX Check result here? */
	if (msg->header->result) {
		svc_gb_msg_free(op->response);
		return ERR_PTR(-EINVAL);
	}

	return msg;

remove_op:
	list_del(&op->entry);
	svc_gb_msg_free(op->request);
gb_msg_alloc:
	kfree(op);

	return ERR_PTR(ret);
}

static int muc_svc_version_check(struct mods_dl_device *dld)
{
	struct muc_svc_data *dd = dld_get_dd(dld);
	struct gb_protocol_version_response *ver;
	struct gb_message *msg;

	ver = kmalloc(sizeof(*ver), GFP_KERNEL);
	if (!ver)
		return -ENOMEM;

	ver->major = GB_SVC_VERSION_MAJOR;
	ver->minor = GB_SVC_VERSION_MINOR;

	msg = svc_gb_msg_send_sync(dld, (uint8_t *)ver, GB_SVC_TYPE_PROTOCOL_VERSION,
				sizeof(*ver), 0, 0);
	if (IS_ERR(msg)) {
		dev_err(&dd->pdev->dev, "Failed to get VERSION from AP\n");
		kfree(ver);
		return PTR_ERR(msg);
	}

	kfree(ver);
	ver = msg->payload;

	/* XXX We could check versions... */
	dev_info(&dd->pdev->dev, "VERSION: %hhu.%hhu\n",
		ver->major, ver->minor);

	svc_gb_msg_free(msg);

	return 0;
}

static int
muc_svc_hello_req(struct mods_dl_device *dld, uint8_t ap_intf_id)
{
	struct muc_svc_data *dd = dld_get_dd(dld);
	struct gb_message *msg;
	struct gb_svc_hello_request *hello;

	hello = kmalloc(sizeof(*hello), GFP_KERNEL);
	if (!hello)
		return -ENOMEM;

	/* Send the endo id and the AP's interface ID */
	hello->endo_id = cpu_to_le16(MUC_SVC_ENDO_ID);
	hello->interface_id = ap_intf_id;

	msg = svc_gb_msg_send_sync(dld, (uint8_t *)hello, GB_SVC_TYPE_SVC_HELLO,
				sizeof(*hello), 0, 0);
	if (IS_ERR(msg)) {
		dev_err(&dd->pdev->dev, "Failed to send HELLO to AP\n");
		kfree(hello);
		return PTR_ERR(msg);
	}

	svc_gb_msg_free(msg);
	kfree(hello);

	return 0;
}

static int
muc_svc_probe_ap(struct mods_dl_device *dld, uint8_t ap_intf_id)
{
	struct muc_svc_data *dd = dld_get_dd(dld);
	int ret;

	ret = muc_svc_version_check(dld);
	if (ret) {
		dev_err(&dd->pdev->dev, "SVC version check failed\n");
		return ret;
	}

	ret = muc_svc_hello_req(dld, ap_intf_id);
	if (ret) {
		dev_err(&dd->pdev->dev, "SVC HELLO failed\n");
		return ret;
	}

	return 0;
}

static int
muc_svc_get_hotplug_data(struct mods_dl_device *dld,
			struct gb_svc_intf_hotplug_request *hotplug,
			u8 out_cport)
{
	struct gb_control_get_ids_response *ids;
	struct muc_svc_data *dd = dld_get_dd(dld);
	struct gb_message *msg;

	/* GET_IDs has no payload */
	msg = svc_gb_msg_send_sync(dld, NULL, GB_CONTROL_TYPE_GET_IDS, 0,
					out_cport, GB_CONTROL_CPORT_ID);
	if (IS_ERR(msg)) {
		dev_err(&dd->pdev->dev, "Failed to get GET_IDS\n");
		return PTR_ERR(msg);
	}

	ids = msg->payload;

	hotplug->data.unipro_mfg_id = le32_to_cpu(ids->unipro_mfg_id);
	hotplug->data.unipro_prod_id = le32_to_cpu(ids->unipro_prod_id);
	hotplug->data.ara_vend_id = le32_to_cpu(ids->ara_vend_id);
	hotplug->data.ara_prod_id = le32_to_cpu(ids->ara_prod_id);

	dev_info(&dd->pdev->dev, "UNIPRO_IDS: %x:%x ARA_IDS: %x:%x\n",
		hotplug->data.unipro_mfg_id, hotplug->data.unipro_prod_id,
		hotplug->data.ara_vend_id, hotplug->data.ara_prod_id);

	svc_gb_msg_free(msg);

	return 0;
}

static int muc_svc_create_control_route(u8 intf_id)
{
	int ret;

	/* Temporary route to interface's Control Port, we assign the CPort
	 * on SVC same as interface since its unique.
	 */
	ret = mods_nw_add_route(MODS_INTF_SVC, intf_id, intf_id, 0);
	if (ret)
		return ret;

	ret = mods_nw_add_route(intf_id, 0, MODS_INTF_SVC, intf_id);
	if (ret)
		goto clean_route1;

	return 0;

clean_route1:
	mods_nw_del_route(MODS_INTF_SVC, intf_id, intf_id, 0);

	return ret;
}

static void muc_svc_destroy_control_route(u8 intf_id)
{
	mods_nw_del_route(MODS_INTF_SVC, intf_id, intf_id, 0);
	mods_nw_del_route(intf_id, 0, MODS_INTF_SVC, intf_id);
}

static void muc_svc_attach_work(struct work_struct *work)
{
	struct muc_svc_hotplug_work *hpw;
	struct gb_message *msg;

	hpw = container_of(work, struct muc_svc_hotplug_work, work);

	msg = svc_gb_msg_send_sync(svc_dd->dld, (uint8_t *)&hpw->hotplug,
					GB_SVC_TYPE_INTF_HOTPLUG,
					sizeof(hpw->hotplug), 0, 0);
	if (IS_ERR(msg)) {
		dev_err(&svc_dd->pdev->dev, "Failed to send HOTPLUG to AP\n");
		goto free_hpw;
	}

	dev_info(&svc_dd->pdev->dev, "Successfully sent hotplug for IID: %d\n",
			hpw->hotplug.intf_id);

	svc_gb_msg_free(msg);

free_hpw:
	kfree(hpw);
	hpw->dld->hpw = NULL;
}

static struct muc_svc_hotplug_work *
muc_svc_create_hotplug_work(struct mods_dl_device *dld, u8 intf_id)
{
	struct muc_svc_data *dd = dld_get_dd(dld);
	struct muc_svc_hotplug_work *hpw;
	int ret;

	hpw = kzalloc(sizeof(*hpw), GFP_KERNEL);
	if (!hpw)
		return ERR_PTR(-ENOMEM);

	hpw->dld = dld;
	INIT_WORK(&hpw->work, muc_svc_attach_work);

	ret = muc_svc_create_control_route(intf_id);
	if (ret) {
		dev_err(&dd->pdev->dev, "Failed to setup CONTROL route\n");
		goto free_hpw;
	}

	/* Get the hotplug IDs */
	ret = muc_svc_get_hotplug_data(dld, &hpw->hotplug, intf_id);
	if (ret)
		goto free_route;

	hpw->hotplug.intf_id = intf_id;

	muc_svc_destroy_control_route(intf_id);

	return hpw;

free_route:
	muc_svc_destroy_control_route(intf_id);
free_hpw:
	kfree(hpw);

	return ERR_PTR(ret);
}

#define kobj_to_device(k) \
	container_of(k, struct mods_dl_device, intf_kobj)

static ssize_t manifest_read(struct file *fp, struct kobject *kobj,
				struct bin_attribute *attr, char *buf,
				loff_t pos, size_t size)
{
	/* XXX Eventually return the manifest */
	return -EINVAL;
}

static ssize_t
hotplug_store(struct mods_dl_device *dev, const char *buf, size_t count)
{
	unsigned long val;

	/* If authentication is disabled, this is a no-op */
	if (!svc_dd->authenticate)
		return count;

	/* Nothing to do, there is no hotplug */
	if (!dev->hpw)
		return -EINVAL;

	if (kstrtoul(buf, 10, &val) < 0)
		return -EINVAL;

	if (val == 1)
		queue_work(svc_dd->wq, &dev->hpw->work);

	/* XXX What to do if told to 'deny' */

	return count;
}

struct muc_svc_attribute {
	struct attribute attr;
	ssize_t (*show)(struct mods_dl_device *dev, char *buf);
	ssize_t (*store)(struct mods_dl_device *dev, const char *buf,
			size_t count);
};

#define MUC_SVC_ATTR(_name, _mode, _show, _store) \
struct muc_svc_attribute muc_svc_attr_##_name = {\
	.attr = { .name = __stringify(_name), .mode = _mode }, \
	.show = _show, \
	.store = _store, \
}

static MUC_SVC_ATTR(hotplug, 0200, NULL, hotplug_store);

#define to_muc_svc_attr(a) \
	container_of(a, struct muc_svc_attribute, attr)

static ssize_t
muc_svc_sysfs_show(struct kobject *kobj, struct attribute *attr, char *buf)
{
	struct mods_dl_device *dev = kobj_to_device(kobj);
	struct muc_svc_attribute *pattr = to_muc_svc_attr(attr);

	if (!pattr->show)
		return -EIO;

	return pattr->show(dev, buf);
}

static ssize_t
muc_svc_sysfs_store(struct kobject *kobj, struct attribute *attr,
			const char *buf, size_t count)
{
	struct mods_dl_device *dev = kobj_to_device(kobj);
	struct muc_svc_attribute *pattr = to_muc_svc_attr(attr);

	if (!pattr->store)
		return -EIO;

	return pattr->store(dev, buf, count);
}

static const struct sysfs_ops muc_svc_sysfs_ops = {
	.show = muc_svc_sysfs_show,
	.store = muc_svc_sysfs_store,
};

static struct attribute *muc_svc_default_attrs[] = {
	&muc_svc_attr_hotplug.attr,
	NULL,
};

static struct kobj_type ktype_muc_svc = {
	.sysfs_ops = &muc_svc_sysfs_ops,
	.default_attrs = muc_svc_default_attrs,
};

static int muc_svc_create_dl_dev_sysfs(struct mods_dl_device *mods_dev)
{
	int err;

	mods_dev->intf_kobj.kset = svc_dd->intf_kset;
	err = kobject_init_and_add(&mods_dev->intf_kobj, &ktype_muc_svc,
					NULL, "%d", mods_dev->intf_id);
	if (err)
		goto put_kobj;

	sysfs_bin_attr_init(&mods_dev->manifest_attr);
	mods_dev->manifest_attr.attr.name = "manifest";
	mods_dev->manifest_attr.attr.mode = S_IRUGO;
	mods_dev->manifest_attr.read = manifest_read;
	mods_dev->manifest_attr.size = 0;

	err = sysfs_create_bin_file(&mods_dev->intf_kobj,
					&mods_dev->manifest_attr);
	if (err)
		goto put_kobj;

	return 0;

put_kobj:
	kobject_put(&mods_dev->intf_kobj);

	return err;
}

static void muc_svc_destroy_dl_dev_sysfs(struct mods_dl_device *mods_dev)
{
	/* XXX Only setup for certain interfaces */
	if (mods_dev->intf_id > MUC_SVC_AP_INTF_ID) {
		sysfs_remove_bin_file(&mods_dev->intf_kobj,
					&mods_dev->manifest_attr);
		kobject_put(&mods_dev->intf_kobj);
	}
}

static int muc_svc_generate_hotplug(struct mods_dl_device *mods_dev)
{
	struct muc_svc_hotplug_work *hpw;

	hpw = muc_svc_create_hotplug_work(svc_dd->dld, mods_dev->intf_id);
	if (IS_ERR(hpw))
		return PTR_ERR(hpw);

	mods_dev->hpw = hpw;

	if (svc_dd->authenticate == false)
		queue_work(svc_dd->wq, &hpw->work);

	return 0;
}

/* Notifies that the DL device is in attached state and the
 * hotplug event can be kicked off
 */
int mods_dl_dev_attached(struct mods_dl_device *mods_dev)
{
	int err;

	/* XXX Temporary method to determine this is AP */
	if (mods_dev->intf_id == MODS_INTF_AP) {
		/* Special case for AP, we'll setup the routes right away */
		err = mods_nw_add_route(MODS_INTF_SVC, 0, MODS_INTF_AP, 0);
		if (err)
			return err;

		err = mods_nw_add_route(MODS_INTF_AP,  0, MODS_INTF_SVC, 0);
		if (err)
			goto free_svc_to_ap;

		err = muc_svc_probe_ap(svc_dd->dld, MUC_SVC_AP_INTF_ID);
		if (err)
			goto free_ap_to_svc;

		return 0;
	}

	return muc_svc_generate_hotplug(mods_dev);

free_ap_to_svc:
	mods_nw_del_route(MODS_INTF_AP, 0, MODS_INTF_SVC, 0);
free_svc_to_ap:
	mods_nw_del_route(MODS_INTF_SVC, 0, MODS_INTF_AP, 0);

	return err;
}
EXPORT_SYMBOL_GPL(mods_dl_dev_attached);

struct mods_dl_device *_mods_create_dl_device(struct mods_dl_driver *drv,
		struct device *dev, u8 intf_id)
{
	struct mods_dl_device *mods_dev;
	int err;

	pr_info("%s for %s [%d]\n", __func__, dev_name(dev), intf_id);
	mods_dev = kzalloc(sizeof(*mods_dev), GFP_KERNEL);
	if (!mods_dev)
		return ERR_PTR(-ENOMEM);

	mods_dev->drv = drv;
	mods_dev->dev = dev;
	mods_dev->intf_id = intf_id;

	mods_nw_add_dl_device(mods_dev);

	/* XXX Only want this created for non AP/SVC */
	if (intf_id > MUC_SVC_AP_INTF_ID) {
		err = muc_svc_create_dl_dev_sysfs(mods_dev);
		if (err)
			goto free_dev;
	}

	return mods_dev;

free_dev:
	kfree(mods_dev);

	return ERR_PTR(err);
}

struct mods_dl_device *mods_create_dl_device(struct mods_dl_driver *drv,
		struct device *dev, u8 intf_id)
{
	/* If the SVC hasn't been fully initialized, return error */
	if (!svc_dd)
		return ERR_PTR(-ENODEV);

	return _mods_create_dl_device(drv, dev, intf_id);
}
EXPORT_SYMBOL_GPL(mods_create_dl_device);

void mods_remove_dl_device(struct mods_dl_device *dev)
{
	muc_svc_destroy_dl_dev_sysfs(dev);
	mods_nw_del_dl_device(dev);
	kfree(dev->hpw);
	kfree(dev);
}
EXPORT_SYMBOL_GPL(mods_remove_dl_device);

/* Handle the muc_msg and strip out its envelope to pass along the
 * actual gb_message we're interested in.
 */
static int
muc_svc_msg_send(struct mods_dl_device *dld, uint8_t *buf, size_t len)
{
	struct muc_msg *m = (struct muc_msg *)buf;

	return svc_gb_msg_recv(dld, m->gb_msg, m->hdr.gb_msg_size,
				m->hdr.dest_cport, m->hdr.src_cport);
}

static void muc_svc_msg_cancel(void *cookie)
{
	/* Should never happen */
}

static struct mods_dl_driver muc_svc_dl_driver = {
	.dl_priv_size = sizeof(struct muc_svc_data),
	.message_send = muc_svc_msg_send,
	.message_cancel = muc_svc_msg_cancel,
};

static int muc_svc_probe(struct platform_device *pdev)
{
	struct muc_svc_data *dd;
	int ret;

	dd = devm_kzalloc(&pdev->dev, sizeof(*dd), GFP_KERNEL);
	if (!dd)
		return -ENOMEM;

	dd->dld = _mods_create_dl_device(&muc_svc_dl_driver, &pdev->dev,
			MODS_INTF_SVC);
	if (IS_ERR(dd->dld)) {
		dev_err(&pdev->dev, "Failed to create mods DL device.\n");
		return PTR_ERR(dd->dld);
	}

	dd->wq = alloc_workqueue("muc_svc_attach", WQ_UNBOUND, 1);
	if (!dd->wq) {
		dev_err(&pdev->dev, "Failed to create attach workqueue.\n");
		ret = -ENOMEM;
		goto free_dl_dev;
	}

	dd->dld->dl_priv = dd;

	dd->pdev = pdev;
	atomic_set(&dd->msg_num, 1);
	INIT_LIST_HEAD(&dd->operations);

	/* Create an 'interfaces' directory in sysfs */
	dd->intf_kset = kset_create_and_add("mods_interfaces", NULL,
						&pdev->dev.kobj);
	if (!dd->intf_kset) {
		dev_err(&pdev->dev, "Failed to create 'interfaces' sysfs\n");
		ret = -ENOMEM;
		goto free_wq;
	}

	platform_set_drvdata(pdev, dd);

	svc_dd = dd;

	return 0;

free_wq:
	destroy_workqueue(dd->wq);
free_dl_dev:
	mods_remove_dl_device(dd->dld);

	return ret;
}

static int muc_svc_remove(struct platform_device *pdev)
{
	struct muc_svc_data *dd = platform_get_drvdata(pdev);

	kset_unregister(dd->intf_kset);
	destroy_workqueue(dd->wq);
	mods_remove_dl_device(dd->dld);

	return 0;
}

static struct platform_driver muc_svc_driver = {
	.driver = {
		.owner = THIS_MODULE,
		.name = "muc_svc",
	},
	.probe = muc_svc_probe,
	.remove  = muc_svc_remove,
};

static struct platform_device *muc_svc_device;

int __init muc_svc_init(void)
{
	int ret;

	ret = platform_driver_register(&muc_svc_driver);
	if (ret < 0) {
		pr_err("muc_svc failed to register driver\n");
		return ret;
	}

	muc_svc_device = platform_device_alloc("muc_svc", -1);
	if (!muc_svc_device) {
		ret = -ENOMEM;
		pr_err("muc_svc failed to alloc device\n");
		goto alloc_fail;
	}

	ret = platform_device_add(muc_svc_device);
	if (ret) {
		pr_err("muc_svc failed to add device: %d\n", ret);
		goto add_fail;
	}

	return 0;

add_fail:
	platform_device_put(muc_svc_device);
alloc_fail:
	platform_driver_unregister(&muc_svc_driver);

	return ret;
}

void __exit muc_svc_exit(void)
{
	platform_driver_unregister(&muc_svc_driver);
	platform_device_unregister(muc_svc_device);
}
