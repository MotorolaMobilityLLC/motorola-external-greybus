/*
 * Power Supply driver for a Greybus module.
 *
 * Copyright 2014-2015 Google Inc.
 * Copyright 2014-2015 Linaro Ltd.
 *
 * Released under the GPLv2 only.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/power_supply.h>
#include <linux/slab.h>

#include "greybus.h"

#define PROP_MAX 32

struct gb_power_supply_prop {
	enum power_supply_property	prop;
	u32				val;
	u32				previous_val;
	bool				is_writeable;
};

struct gb_power_supply {
	u8				id;
#ifdef DRIVER_OWNS_PSY_STRUCT
	struct power_supply		psy;
#define to_gb_power_supply(x) container_of(x, struct gb_power_supply, psy)
#else
	struct power_supply		*psy;
	struct power_supply_desc	desc;
#define to_gb_power_supply(x) power_supply_get_drvdata(x)
#endif
	char				name[64];
	struct gb_power_supplies	*supplies;
	struct delayed_work		work;
	char				*manufacturer;
	char				*model_name;
	char				*serial_number;
	u8				type;
	u8				properties_count;
	u8				properties_count_str;
	unsigned long			last_update;
	unsigned int			update_interval;
	bool				changed;
	struct gb_power_supply_prop	*props;
	enum power_supply_property	*props_raw;
};

struct gb_power_supplies {
	struct gb_connection	*connection;
	u8			supplies_count;
	struct gb_power_supply	*supply;
	struct mutex		supplies_lock;
};

/* cache time in milliseconds, if cache_time is set to 0 cache is disable */
static unsigned int cache_time = 1000;
/*
 * update interval initial and maximum value, between the two will
 * back-off exponential
 */
static unsigned int update_interval_init = 1 * HZ;
static unsigned int update_interval_max = 30 * HZ;

struct gb_power_supply_changes {
	enum power_supply_property	prop;
	u32				tolerance_change;
};

static const struct gb_power_supply_changes psy_props_changes[] = {
	{	.prop =			GB_POWER_SUPPLY_PROP_STATUS,
		.tolerance_change =	0,
	},
	{	.prop =			GB_POWER_SUPPLY_PROP_TEMP,
		.tolerance_change =	500,
	},
	{	.prop =			GB_POWER_SUPPLY_PROP_ONLINE,
		.tolerance_change =	0,
	},
};

static struct gb_connection *get_conn_from_psy(struct gb_power_supply *gbpsy)
{
	return gbpsy->supplies->connection;
}

static struct gb_power_supply_prop *get_psy_prop(struct gb_power_supply *gbpsy,
						 enum power_supply_property psp)
{
	int i;

	for (i = 0; i < gbpsy->properties_count; i++)
		if (gbpsy->props[i].prop == psp)
			return &gbpsy->props[i];
	return NULL;
}

static int is_psy_prop_writeable(struct gb_power_supply *gbpsy,
				     enum power_supply_property psp)
{
	struct gb_power_supply_prop *prop;

	prop = get_psy_prop(gbpsy, psp);
	if (!prop)
		return -ENOENT;
	return prop->is_writeable ? 1 : 0;
}

static int is_prop_valint(enum power_supply_property psp)
{
	return ((psp < POWER_SUPPLY_PROP_MODEL_NAME) ? 1 : 0);
}

static void next_interval(struct gb_power_supply *gbpsy)
{
	if (gbpsy->update_interval == update_interval_max)
		return;

	/* do some exponential back-off in the update interval */
	gbpsy->update_interval *= 2;
	if (gbpsy->update_interval > update_interval_max)
		gbpsy->update_interval = update_interval_max;
}

#ifdef DRIVER_OWNS_PSY_STRUCT
static void __gb_power_supply_changed(struct gb_power_supply *gbpsy)
{
	power_supply_changed(&gbpsy->psy);
}
#else
static void __gb_power_supply_changed(struct gb_power_supply *gbpsy)
{
	power_supply_changed(gbpsy->psy);
}
#endif

static void check_changed(struct gb_power_supply *gbpsy,
			  struct gb_power_supply_prop *prop)
{
	const struct gb_power_supply_changes *psyc;
	u32 val = prop->val;
	u32 prev_val = prop->previous_val;
	int i;

	for (i = 0; i < ARRAY_SIZE(psy_props_changes); i++) {
		psyc = &psy_props_changes[i];
		if (prop->prop == psyc->prop) {
			if (!psyc->tolerance_change)
				gbpsy->changed = true;
			else if (val < prev_val &&
				 prev_val - val > psyc->tolerance_change)
				gbpsy->changed = true;
			else if (val > prev_val &&
				 val - prev_val > psyc->tolerance_change)
				gbpsy->changed = true;
			break;
		}
	}
}

static int total_props(struct gb_power_supply *gbpsy)
{
	/* this return the intval plus the strval properties */
	return (gbpsy->properties_count + gbpsy->properties_count_str);
}

static void prop_append(struct gb_power_supply *gbpsy,
			enum power_supply_property prop)
{
	enum power_supply_property *new_props_raw;

	gbpsy->properties_count_str++;
	new_props_raw = krealloc(gbpsy->props_raw, total_props(gbpsy) *
				 sizeof(enum power_supply_property),
				 GFP_KERNEL);
	if (!new_props_raw)
		return;
	gbpsy->props_raw = new_props_raw;
	gbpsy->props_raw[total_props(gbpsy) - 1] = prop;
}

static int __gb_power_supply_set_name(char *init_name, char *name, size_t len)
{
	unsigned int i = 0;
	int ret = 0;
	struct power_supply *psy;

	if (!strlen(init_name))
		init_name = "gb_power_supply";
	strlcpy(name, init_name, len);

	while ((ret < len) && (psy = power_supply_get_by_name(name))) {
#ifdef PSY_HAVE_PUT
		power_supply_put(psy);
#endif
		ret = snprintf(name, len, "%s_%u", init_name, ++i);
	}
	if (ret >= len)
		return -ENOMEM;
	return i;
}

static void _gb_power_supply_append_props(struct gb_power_supply *gbpsy)
{
	if (strlen(gbpsy->manufacturer))
		prop_append(gbpsy, POWER_SUPPLY_PROP_MANUFACTURER);
	if (strlen(gbpsy->model_name))
		prop_append(gbpsy, POWER_SUPPLY_PROP_MODEL_NAME);
	if (strlen(gbpsy->serial_number))
		prop_append(gbpsy, POWER_SUPPLY_PROP_SERIAL_NUMBER);
}

static int gb_power_supply_description_get(struct gb_power_supply *gbpsy)
{
	struct gb_connection *connection = get_conn_from_psy(gbpsy);
	struct gb_power_supply_get_description_request req;
	struct gb_power_supply_get_description_response resp;
	int ret;

	req.psy_id = gbpsy->id;

	ret = gb_operation_sync(connection,
				GB_POWER_SUPPLY_TYPE_GET_DESCRIPTION,
				&req, sizeof(req), &resp, sizeof(resp));
	if (ret < 0)
		return ret;

	gbpsy->manufacturer = kstrndup(resp.manufacturer, PROP_MAX, GFP_KERNEL);
	if (!gbpsy->manufacturer)
		return -ENOMEM;
	gbpsy->model_name = kstrndup(resp.model, PROP_MAX, GFP_KERNEL);
	if (!gbpsy->model_name)
		return -ENOMEM;
	gbpsy->serial_number = kstrndup(resp.serial_number, PROP_MAX,
				       GFP_KERNEL);
	if (!gbpsy->serial_number)
		return -ENOMEM;

	gbpsy->type = le16_to_cpu(resp.type);
	gbpsy->properties_count = resp.properties_count;

	return 0;
}

static int gb_power_supply_prop_descriptors_get(struct gb_power_supply *gbpsy)
{
	struct gb_connection *connection = get_conn_from_psy(gbpsy);
	struct gb_power_supply_get_property_descriptors_request req;
	struct gb_power_supply_get_property_descriptors_response resp;
	int ret;
	int i;

	if (gbpsy->properties_count == 0)
		return 0;

	req.psy_id = gbpsy->id;

	ret = gb_operation_sync(connection,
				GB_POWER_SUPPLY_TYPE_GET_PROP_DESCRIPTORS,
				&req, sizeof(req), &resp,
				sizeof(resp) + gbpsy->properties_count *
				sizeof(struct gb_power_supply_props_desc));
	if (ret < 0)
		return ret;

	gbpsy->props = kcalloc(gbpsy->properties_count, sizeof(*gbpsy->props),
			      GFP_KERNEL);
	if (!gbpsy->props)
		return -ENOMEM;

	gbpsy->props_raw = kzalloc(gbpsy->properties_count *
				  sizeof(*gbpsy->props_raw), GFP_KERNEL);
	if (!gbpsy->props_raw)
		return -ENOMEM;

	/* Store available properties */
	for (i = 0; i < gbpsy->properties_count; i++) {
		gbpsy->props[i].prop = resp.props[i].property;
		gbpsy->props_raw[i] = resp.props[i].property;
		if (resp.props[i].is_writeable)
			gbpsy->props[i].is_writeable = true;
	}

	/*
	 * now append the properties that we already got information in the
	 * get_description operation. (char * ones)
	 */
	_gb_power_supply_append_props(gbpsy);

	return 0;
}

static int __gb_power_supply_property_update(struct gb_power_supply *gbpsy,
					     enum power_supply_property psp)
{
	struct gb_connection *connection = get_conn_from_psy(gbpsy);
	struct gb_power_supply_prop *prop;
	struct gb_power_supply_get_property_request req;
	struct gb_power_supply_get_property_response resp;
	u32 val;
	int ret;

	prop = get_psy_prop(gbpsy, psp);
	if (!prop)
		return -EINVAL;
	req.psy_id = gbpsy->id;
	req.property = (u8)psp;

	ret = gb_operation_sync(connection, GB_POWER_SUPPLY_TYPE_GET_PROPERTY,
				&req, sizeof(req), &resp, sizeof(resp));
	if (ret < 0)
		return ret;

	val = le32_to_cpu(resp.prop_val);
	if (val == prop->val)
		return 0;

	prop->previous_val = prop->val;
	prop->val = val;

	check_changed(gbpsy, prop);

	return 0;
}

static int __gb_power_supply_property_get(struct gb_power_supply *gbpsy,
					  enum power_supply_property psp,
					  union power_supply_propval *val)
{
	struct gb_power_supply_prop *prop;

	prop = get_psy_prop(gbpsy, psp);
	if (!prop)
		return -EINVAL;

	val->intval = prop->val;
	return 0;
}

static int __gb_power_supply_property_strval_get(struct gb_power_supply *gbpsy,
						enum power_supply_property psp,
						union power_supply_propval *val)
{
	switch (psp) {
	case POWER_SUPPLY_PROP_MODEL_NAME:
		val->strval = kstrndup(gbpsy->model_name, PROP_MAX, GFP_KERNEL);
		break;
	case POWER_SUPPLY_PROP_MANUFACTURER:
		val->strval = kstrndup(gbpsy->manufacturer, PROP_MAX,
				       GFP_KERNEL);
		break;
	case POWER_SUPPLY_PROP_SERIAL_NUMBER:
		val->strval = kstrndup(gbpsy->serial_number, PROP_MAX,
				       GFP_KERNEL);
		break;
	default:
		break;
	}

	return 0;
}

static int _gb_power_supply_property_get(struct gb_power_supply *gbpsy,
					 enum power_supply_property psp,
					 union power_supply_propval *val)
{
	struct gb_connection *connection = get_conn_from_psy(gbpsy);
	int ret;

	/*
	 * Properties of type const char *, were already fetched on
	 * get_description operation and should be cached in gb
	 */
	if (is_prop_valint(psp))
		ret = __gb_power_supply_property_get(gbpsy, psp, val);
	else
		ret = __gb_power_supply_property_strval_get(gbpsy, psp, val);

	if (ret < 0)
		dev_err(&connection->bundle->dev, "get property %u\n", psp);

	return 0;
}

static int gb_power_supply_status_get(struct gb_power_supply *gbpsy)
{
	int ret = 0;
	int i;

	/* check if cache is good enough */
	if (gbpsy->last_update &&
	    time_is_after_jiffies(gbpsy->last_update +
				  msecs_to_jiffies(cache_time)))
		return 0;

	for (i = 0; i < gbpsy->properties_count; i++) {
		ret = __gb_power_supply_property_update(gbpsy,
							gbpsy->props[i].prop);
		if (ret < 0)
			break;
	}

	if (ret == 0)
		gbpsy->last_update = jiffies;

	return ret;
}

static void gb_power_supply_status_update(struct gb_power_supply *gbpsy)
{
	/* check if there a change that need to be reported */
	gb_power_supply_status_get(gbpsy);

	if (!gbpsy->changed)
		return;

	gbpsy->update_interval = update_interval_init;
	__gb_power_supply_changed(gbpsy);
	gbpsy->changed = false;
}

static void gb_power_supply_work(struct work_struct *work)
{
	struct gb_power_supply *gbpsy = container_of(work,
						     struct gb_power_supply,
						     work.work);

	/*
	 * if the poll interval is not set, disable polling, this is helpful
	 * specially at unregister time.
	 */
	if (!gbpsy->update_interval)
		return;

	gb_power_supply_status_update(gbpsy);
	next_interval(gbpsy);
	schedule_delayed_work(&gbpsy->work, gbpsy->update_interval);
}

static int get_property(struct power_supply *b,
			enum power_supply_property psp,
			union power_supply_propval *val)
{
	struct gb_power_supply *gbpsy = to_gb_power_supply(b);

	gb_power_supply_status_get(gbpsy);

	return _gb_power_supply_property_get(gbpsy, psp, val);
}

static int gb_power_supply_property_set(struct gb_power_supply *gbpsy,
					enum power_supply_property psp,
					int val)
{
	struct gb_connection *connection = get_conn_from_psy(gbpsy);
	struct gb_power_supply_prop *prop;
	struct gb_power_supply_set_property_request req;
	int ret;

	prop = get_psy_prop(gbpsy, psp);
	if (!prop)
		return -EINVAL;
	req.psy_id = gbpsy->id;
	req.property = (u8)psp;
	req.prop_val = cpu_to_le32(val);

	ret = gb_operation_sync(connection, GB_POWER_SUPPLY_TYPE_SET_PROPERTY,
				&req, sizeof(req), NULL, 0);
	if (ret < 0)
		goto out;

	/* cache immediately the new value */
	prop->val = val;

out:
	return ret;
}

static int set_property(struct power_supply *b,
			enum power_supply_property psp,
			const union power_supply_propval *val)
{
	struct gb_power_supply *gbpsy = to_gb_power_supply(b);

	return gb_power_supply_property_set(gbpsy, psp, val->intval);
}

static int property_is_writeable(struct power_supply *b,
				 enum power_supply_property psp)
{
	struct gb_power_supply *gbpsy = to_gb_power_supply(b);

	return is_psy_prop_writeable(gbpsy, psp);
}


#ifdef DRIVER_OWNS_PSY_STRUCT
static int gb_power_supply_register(struct gb_power_supply *gbpsy)
{
	struct gb_connection *connection = get_conn_from_psy(gbpsy);

	gbpsy->psy.name			= gbpsy->name;
	gbpsy->psy.type			= gbpsy->type;
	gbpsy->psy.properties		= gbpsy->props_raw;
	gbpsy->psy.num_properties	= total_props(gbpsy);
	gbpsy->psy.get_property		= get_property;
	gbpsy->psy.set_property		= set_property;
	gbpsy->psy.property_is_writeable = property_is_writeable;

	return power_supply_register(&connection->bundle->dev,
				     &gbpsy->psy);
}
#else
static int gb_power_supply_register(struct gb_power_supply *gbpsy)
{
	struct gb_connection *connection = get_conn_from_psy(gbpsy);
	struct power_supply_config cfg = {};

	cfg.drv_data = gbpsy;

	gbpsy->desc.name		= gbpsy->name;
	gbpsy->desc.type		= gbpsy->type;
	gbpsy->desc.properties		= gbpsy->props_raw;
	gbpsy->desc.num_properties	= total_props(gbpsy);
	gbpsy->desc.get_property	= get_property;
	gbpsy->desc.set_property	= set_property;
	gbpsy->desc.property_is_writeable = property_is_writeable;

	gbpsy->psy = power_supply_register(&connection->bundle->dev,
					   &gbpsy->desc, &cfg);
	if (IS_ERR(gbpsy->psy))
		return PTR_ERR(gbpsy->psy);

	return 0;
}
#endif

static void _gb_power_supply_free(struct gb_power_supply *gbpsy)
{
	kfree(gbpsy->serial_number);
	kfree(gbpsy->model_name);
	kfree(gbpsy->manufacturer);
	kfree(gbpsy->props_raw);
	kfree(gbpsy->props);
	kfree(gbpsy);
}

static void _gb_power_supply_release(struct gb_power_supply *gbpsy)
{
	if (!gbpsy)
		return;

	gbpsy->update_interval = 0;

	cancel_delayed_work_sync(&gbpsy->work);
#ifdef DRIVER_OWNS_PSY_STRUCT
	power_supply_unregister(&gbpsy->psy);
#else
	power_supply_unregister(gbpsy->psy);
#endif

	_gb_power_supply_free(gbpsy);
}

static void _gb_power_supplies_release(struct gb_power_supplies *supplies)
{
	int i;

	mutex_lock(&supplies->supplies_lock);
	for (i = 0; i < supplies->supplies_count; i++)
		_gb_power_supply_release(&supplies->supply[i]);
	mutex_unlock(&supplies->supplies_lock);
}

static int gb_power_supplies_get_count(struct gb_power_supplies *supplies)
{
	struct gb_power_supply_get_supplies_response resp;
	int ret;

	ret = gb_operation_sync(supplies->connection,
				GB_POWER_SUPPLY_TYPE_GET_SUPPLIES,
				NULL, 0, &resp, sizeof(resp));
	if (ret < 0)
		return ret;

	if  (!resp.supplies_count)
		return -EINVAL;

	supplies->supplies_count = resp.supplies_count;

	return ret;
}

static int gb_power_supply_config(struct gb_power_supplies *supplies, int id)
{
	struct gb_power_supply *gbpsy = &supplies->supply[id];
	int ret;

	gbpsy->supplies = supplies;
	gbpsy->id = id;

	ret = gb_power_supply_description_get(gbpsy);
	if (ret < 0)
		goto out;

	ret = gb_power_supply_prop_descriptors_get(gbpsy);
	if (ret < 0)
		goto out;

	/* guarantee that we have an unique name, before register */
	ret = __gb_power_supply_set_name(gbpsy->model_name, gbpsy->name,
					 sizeof(gbpsy->name));
	if (ret < 0)
		goto out;

	ret = gb_power_supply_register(gbpsy);
	if (ret < 0)
		goto out;

	gbpsy->update_interval = update_interval_init;
	INIT_DELAYED_WORK(&gbpsy->work, gb_power_supply_work);
	schedule_delayed_work(&gbpsy->work, 0);

out:
	return ret;
}

static int gb_power_supplies_setup(struct gb_power_supplies *supplies)
{
	struct gb_connection *connection = supplies->connection;
	int ret;
	int i;

	mutex_lock(&supplies->supplies_lock);

	ret = gb_power_supplies_get_count(supplies);
	if (ret < 0)
		goto out;

	supplies->supply = kzalloc(supplies->supplies_count *
				     sizeof(struct gb_power_supply),
				     GFP_KERNEL);

	if (!supplies->supply)
		return -ENOMEM;

	for (i = 0; i < supplies->supplies_count; i++) {
		ret = gb_power_supply_config(supplies, i);
		if (ret < 0) {
			dev_err(&connection->bundle->dev,
				"Fail to configure supplies devices\n");
			goto out;
		}
	}
out:
	mutex_unlock(&supplies->supplies_lock);
	return ret;
}

static int gb_power_supply_event_recv(u8 type, struct gb_operation *op)
{
	struct gb_connection *connection = op->connection;
	struct gb_power_supplies *supplies = connection->private;
	struct gb_power_supply *gbpsy;
	struct gb_message *request;
	struct gb_power_supply_event_request *payload;
	u8 psy_id;
	u8 event;
	int ret = 0;

	if (type != GB_POWER_SUPPLY_TYPE_EVENT) {
		dev_err(&connection->bundle->dev,
			"Unsupported unsolicited event: %u\n", type);
		return -EINVAL;
	}

	request = op->request;

	if (request->payload_size < sizeof(*payload)) {
		dev_err(&connection->bundle->dev,
			"Wrong event size received (%zu < %zu)\n",
			request->payload_size, sizeof(*payload));
		return -EINVAL;
	}

	payload = request->payload;
	psy_id = payload->psy_id;
	mutex_lock(&supplies->supplies_lock);
	if (psy_id >= supplies->supplies_count || !&supplies->supply[psy_id]) {
		dev_err(&connection->bundle->dev,
			"Event received for unconfigured power_supply id: %d\n",
			psy_id);
		ret = -EINVAL;
		goto out_unlock;
	}

	event = payload->event;
	/*
	 * we will only handle events after setup is done and before release is
	 * running. For that just check update_interval.
	 */
	gbpsy = &supplies->supply[psy_id];
	if (gbpsy->update_interval) {
		ret = -ESHUTDOWN;
		goto out_unlock;
	}

	if (event & GB_POWER_SUPPLY_UPDATE)
		gb_power_supply_status_update(gbpsy);

out_unlock:
	mutex_unlock(&supplies->supplies_lock);
	return ret;
}

static int gb_power_supply_connection_init(struct gb_connection *connection)
{
	struct gb_power_supplies *supplies;

	supplies = kzalloc(sizeof(*supplies), GFP_KERNEL);
	if (!supplies)
		return -ENOMEM;

	supplies->connection = connection;
	connection->private = supplies;

	mutex_init(&supplies->supplies_lock);

	return gb_power_supplies_setup(supplies);
}

static void gb_power_supply_connection_exit(struct gb_connection *connection)
{
	struct gb_power_supplies *supplies = connection->private;

	_gb_power_supplies_release(supplies);
}

static struct gb_protocol power_supply_protocol = {
	.name			= "power_supply",
	.id			= GREYBUS_PROTOCOL_POWER_SUPPLY,
	.major			= GB_POWER_SUPPLY_VERSION_MAJOR,
	.minor			= GB_POWER_SUPPLY_VERSION_MINOR,
	.connection_init	= gb_power_supply_connection_init,
	.connection_exit	= gb_power_supply_connection_exit,
	.request_recv		= gb_power_supply_event_recv,
};

gb_protocol_driver(&power_supply_protocol);

MODULE_LICENSE("GPL v2");
