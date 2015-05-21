/*
 * SD/MMC Greybus driver.
 *
 * Copyright 2014 Google Inc.
 * Copyright 2014 Linaro Ltd.
 *
 * Released under the GPLv2 only.
 */

#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/mmc/host.h>

#include "greybus.h"

struct gb_sdio_host {
	struct gb_connection *connection;
	struct mmc_host	*mmc;
	struct mmc_request *mrq;
	// FIXME - some lock?
};

static void gb_sd_request(struct mmc_host *mmc, struct mmc_request *mrq)
{
	// FIXME - do something here...
}

static void gb_sd_set_ios(struct mmc_host *mmc, struct mmc_ios *ios)
{
	// FIXME - do something here...
}

static int gb_sd_get_ro(struct mmc_host *mmc)
{
	// FIXME - do something here...
	return 0;
}

static const struct mmc_host_ops gb_sd_ops = {
	.request	= gb_sd_request,
	.set_ios	= gb_sd_set_ios,
	.get_ro		= gb_sd_get_ro,
};

static int gb_sdio_connection_init(struct gb_connection *connection)
{
	struct mmc_host *mmc;
	struct gb_sdio_host *host;

	mmc = mmc_alloc_host(sizeof(*host), &connection->dev);
	if (!mmc)
		return -ENOMEM;

	host = mmc_priv(mmc);
	host->mmc = mmc;

	mmc->ops = &gb_sd_ops;
	// FIXME - set up size limits we can handle.
	// FIXME - register the host controller.

	host->connection = connection;
	connection->private = host;
	return 0;
}

static void gb_sdio_connection_exit(struct gb_connection *connection)
{
	struct mmc_host *mmc;
	struct gb_sdio_host *host;

	host = connection->private;
	if (!host)
		return;

	mmc = host->mmc;
	mmc_remove_host(mmc);
	mmc_free_host(mmc);
	connection->private = NULL;
}

static struct gb_protocol sdio_protocol = {
	.name			= "sdio",
	.id			= GREYBUS_PROTOCOL_SDIO,
	.major			= 0,
	.minor			= 1,
	.connection_init	= gb_sdio_connection_init,
	.connection_exit	= gb_sdio_connection_exit,
	.request_recv		= NULL,	/* no incoming requests */
};

gb_gpbridge_protocol_driver(sdio_protocol);
