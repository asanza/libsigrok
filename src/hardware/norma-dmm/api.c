/*
 * This file is part of the libsigrok project.
 *
 * Copyright (C) 2013 Matthias Heidbrink <m-sigrok@heidbrink.biz>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

/** @file
 *  Norma DM9x0/Siemens B102x DMMs driver.
 *  @internal
 */

#include <config.h>
#include "protocol.h"

static const uint32_t scanopts[] = {
	SR_CONF_CONN,
	SR_CONF_SERIALCOMM,
};

static const uint32_t devopts[] = {
	SR_CONF_MULTIMETER,
	SR_CONF_CONTINUOUS,
	SR_CONF_LIMIT_SAMPLES | SR_CONF_SET,
	SR_CONF_LIMIT_MSEC | SR_CONF_SET,
};

#define BUF_MAX 50

#define SERIALCOMM "4800/8n1/dtr=1/rts=0/flow=1"

SR_PRIV struct sr_dev_driver norma_dmm_driver_info;
SR_PRIV struct sr_dev_driver siemens_b102x_driver_info;

static const char *get_brandstr(struct sr_dev_driver *drv)
{
	return (drv == &norma_dmm_driver_info) ? "Norma" : "Siemens";
}

static const char *get_typestr(int type, struct sr_dev_driver *drv)
{
	static const char *nameref[5][2] = {
		{"DM910", "B1024"},
		{"DM920", "B1025"},
		{"DM930", "B1026"},
		{"DM940", "B1027"},
		{"DM950", "B1028"},
	};

	if ((type < 1) || (type > 5))
		return "Unknown type!";

	return nameref[type - 1][(drv == &siemens_b102x_driver_info)];
}

static GSList *scan(struct sr_dev_driver *drv, GSList *options)
{
	struct sr_dev_inst *sdi;
	struct drv_context *drvc;
	struct dev_context *devc;
	struct sr_config *src;
	struct sr_serial_dev_inst *serial;
	GSList *l, *devices;
	int len, cnt, auxtype;
	const char *conn, *serialcomm;
	char *buf;
	char req[10];

	devices = NULL;
	drvc = drv->context;
	drvc->instances = NULL;
	conn = serialcomm = NULL;

	for (l = options; l; l = l->next) {
		src = l->data;
		switch (src->key) {
		case SR_CONF_CONN:
			conn = g_variant_get_string(src->data, NULL);
			break;
		case SR_CONF_SERIALCOMM:
			serialcomm = g_variant_get_string(src->data, NULL);
			break;
		}
	}
	if (!conn)
		return NULL;
	if (!serialcomm)
		serialcomm = SERIALCOMM;

	serial = sr_serial_dev_inst_new(conn, serialcomm);

	if (serial_open(serial, SERIAL_RDWR) != SR_OK)
		return NULL;

	serial_flush(serial);

	buf = g_malloc(BUF_MAX);

	snprintf(req, sizeof(req), "%s\r\n",
		 nmadmm_requests[NMADMM_REQ_IDN].req_str);
	g_usleep(150 * 1000); /* Wait a little to allow serial port to settle. */
	for (cnt = 0; cnt < 7; cnt++) {
		if (serial_write_blocking(serial, req, strlen(req),
				serial_timeout(serial, strlen(req))) < 0) {
			sr_err("Unable to send identification request.");
			return NULL;
		}
		len = BUF_MAX;
		serial_readline(serial, &buf, &len, NMADMM_TIMEOUT_MS);
		if (!len)
			continue;
		buf[BUF_MAX - 1] = '\0';

		/* Match ID string, e.g. "1834 065 V1.06,IF V1.02" (DM950). */
		if (g_regex_match_simple("^1834 [^,]*,IF V*", (char *)buf, 0, 0)) {
			auxtype = xgittoint(buf[7]);
			sr_spew("%s %s DMM %s detected!", get_brandstr(drv), get_typestr(auxtype, drv), buf + 9);

			sdi = g_malloc0(sizeof(struct sr_dev_inst));
			sdi->status = SR_ST_INACTIVE;
			sdi->vendor = g_strdup(get_brandstr(drv));
			sdi->model = g_strdup(get_typestr(auxtype, drv));
			sdi->version = g_strdup(buf + 9);
			devc = g_malloc0(sizeof(struct dev_context));
			sr_sw_limits_init(&devc->limits);
			devc->type = auxtype;
			devc->version = g_strdup(&buf[9]);
			sdi->conn = serial;
			sdi->priv = devc;
			sdi->driver = drv;
			sr_channel_new(sdi, 0, SR_CHANNEL_ANALOG, TRUE, "P1");
			drvc->instances = g_slist_append(drvc->instances, sdi);
			devices = g_slist_append(devices, sdi);
			break;
		}

		/*
		 * The interface of the DM9x0 contains a cap that needs to
		 * charge for up to 10s before the interface works, if not
		 * powered externally. Therefore wait a little to improve
		 * chances.
		 */
		if (cnt == 3) {
			sr_info("Waiting 5s to allow interface to settle.");
			g_usleep(5 * 1000 * 1000);
		}
	}

	g_free(buf);

	serial_close(serial);
	if (!devices)
		sr_serial_dev_inst_free(serial);

	return devices;
}

static int dev_close(struct sr_dev_inst *sdi)
{
	struct dev_context *devc;

	std_serial_dev_close(sdi);

	/* Free dynamically allocated resources. */
	if ((devc = sdi->priv) && devc->version) {
		g_free(devc->version);
		devc->version = NULL;
	}

	return SR_OK;
}

static int config_set(uint32_t key, GVariant *data, const struct sr_dev_inst *sdi,
		const struct sr_channel_group *cg)
{
	struct dev_context *devc;

	(void)cg;

	if (sdi->status != SR_ST_ACTIVE)
		return SR_ERR_DEV_CLOSED;

	if (!(devc = sdi->priv)) {
		sr_err("sdi->priv was NULL.");
		return SR_ERR_BUG;
	}

	return sr_sw_limits_config_set(&devc->limits, key, data);
}

static int config_list(uint32_t key, GVariant **data, const struct sr_dev_inst *sdi,
		const struct sr_channel_group *cg)
{
	(void)sdi;
	(void)cg;

	switch (key) {
	case SR_CONF_SCAN_OPTIONS:
		*data = g_variant_new_fixed_array(G_VARIANT_TYPE_UINT32,
				scanopts, ARRAY_SIZE(scanopts), sizeof(uint32_t));
		break;
	case SR_CONF_DEVICE_OPTIONS:
		*data = g_variant_new_fixed_array(G_VARIANT_TYPE_UINT32,
				devopts, ARRAY_SIZE(devopts), sizeof(uint32_t));
		break;
	default:
		return SR_ERR_NA;
	}

	return SR_OK;
}

static int dev_acquisition_start(const struct sr_dev_inst *sdi)
{
	struct dev_context *devc;
	struct sr_serial_dev_inst *serial;

	if (sdi->status != SR_ST_ACTIVE)
		return SR_ERR_DEV_CLOSED;

	devc = sdi->priv;

	sr_sw_limits_acquisition_start(&devc->limits);
	std_session_send_df_header(sdi, LOG_PREFIX);

	/* Poll every 100ms, or whenever some data comes in. */
	serial = sdi->conn;
	serial_source_add(sdi->session, serial, G_IO_IN, 100,
			norma_dmm_receive_data, (void *)sdi);

	return SR_OK;
}

static int dev_acquisition_stop(struct sr_dev_inst *sdi)
{
	return std_serial_dev_acquisition_stop(sdi, dev_close,
			sdi->conn, LOG_PREFIX);
}

SR_PRIV struct sr_dev_driver norma_dmm_driver_info = {
	.name = "norma-dmm",
	.longname = "Norma DM9x0 DMMs",
	.api_version = 1,
	.init = std_init,
	.cleanup = std_cleanup,
	.scan = scan,
	.dev_list = std_dev_list,
	.dev_clear = NULL,
	.config_get = NULL,
	.config_set = config_set,
	.config_list = config_list,
	.dev_open = std_serial_dev_open,
	.dev_close = dev_close,
	.dev_acquisition_start = dev_acquisition_start,
	.dev_acquisition_stop = dev_acquisition_stop,
	.context = NULL,
};

SR_PRIV struct sr_dev_driver siemens_b102x_driver_info = {
	.name = "siemens-b102x",
	.longname = "Siemens B102x DMMs",
	.api_version = 1,
	.init = std_init,
	.cleanup = std_cleanup,
	.scan = scan,
	.dev_list = std_dev_list,
	.dev_clear = NULL,
	.config_get = NULL,
	.config_set = config_set,
	.config_list = config_list,
	.dev_open = std_serial_dev_open,
	.dev_close = dev_close,
	.dev_acquisition_start = dev_acquisition_start,
	.dev_acquisition_stop = dev_acquisition_stop,
	.context = NULL,
};
