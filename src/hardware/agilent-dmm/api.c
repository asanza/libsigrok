/*
 * This file is part of the libsigrok project.
 *
 * Copyright (C) 2012 Bert Vermeulen <bert@biot.com>
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

#include <config.h>
#include <glib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <string.h>
#include <libsigrok/libsigrok.h>
#include "libsigrok-internal.h"
#include "agilent-dmm.h"

static const uint32_t scanopts[] = {
	SR_CONF_CONN,
	SR_CONF_SERIALCOMM,
};

static const uint32_t drvopts[] = {
	SR_CONF_MULTIMETER,
};

static const uint32_t devopts[] = {
	SR_CONF_CONTINUOUS,
	SR_CONF_LIMIT_SAMPLES | SR_CONF_SET,
	SR_CONF_LIMIT_MSEC | SR_CONF_SET,
};

extern const struct agdmm_job agdmm_jobs_u12xx[];
extern const struct agdmm_recv agdmm_recvs_u123x[];
extern const struct agdmm_recv agdmm_recvs_u124x[];
extern const struct agdmm_recv agdmm_recvs_u125x[];

/* This works on all the Agilent U12xxA series, although the
 * U127xA can apparently also run at 19200/8n1. */
#define SERIALCOMM "9600/8n1"

static const struct agdmm_profile supported_agdmm[] = {
	{ AGILENT_U1231, "U1231A", agdmm_jobs_u12xx, agdmm_recvs_u123x },
	{ AGILENT_U1232, "U1232A", agdmm_jobs_u12xx, agdmm_recvs_u123x },
	{ AGILENT_U1233, "U1233A", agdmm_jobs_u12xx, agdmm_recvs_u123x },

	{ AGILENT_U1241, "U1241A", agdmm_jobs_u12xx, agdmm_recvs_u124x },
	{ AGILENT_U1242, "U1242A", agdmm_jobs_u12xx, agdmm_recvs_u124x },
	{ AGILENT_U1241, "U1241B", agdmm_jobs_u12xx, agdmm_recvs_u124x },
	{ AGILENT_U1242, "U1242B", agdmm_jobs_u12xx, agdmm_recvs_u124x },

	{ AGILENT_U1251, "U1251A", agdmm_jobs_u12xx, agdmm_recvs_u125x },
	{ AGILENT_U1252, "U1252A", agdmm_jobs_u12xx, agdmm_recvs_u125x },
	{ AGILENT_U1253, "U1253A", agdmm_jobs_u12xx, agdmm_recvs_u125x },
	{ AGILENT_U1251, "U1251B", agdmm_jobs_u12xx, agdmm_recvs_u125x },
	{ AGILENT_U1252, "U1252B", agdmm_jobs_u12xx, agdmm_recvs_u125x },
	{ AGILENT_U1253, "U1253B", agdmm_jobs_u12xx, agdmm_recvs_u125x },
	ALL_ZERO
};

SR_PRIV struct sr_dev_driver agdmm_driver_info;

static GSList *scan(struct sr_dev_driver *di, GSList *options)
{
	struct sr_dev_inst *sdi;
	struct drv_context *drvc;
	struct dev_context *devc;
	struct sr_config *src;
	struct sr_serial_dev_inst *serial;
	GSList *l, *devices;
	int len, i;
	const char *conn, *serialcomm;
	char *buf, **tokens;

	drvc = di->context;
	drvc->instances = NULL;

	devices = NULL;
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
	if (serial_write_blocking(serial, "*IDN?\r\n", 7, SERIAL_WRITE_TIMEOUT_MS) < 7) {
		sr_err("Unable to send identification string.");
		return NULL;
	}

	len = 128;
	buf = g_malloc(len);
	serial_readline(serial, &buf, &len, 250);
	if (!len)
		return NULL;

	tokens = g_strsplit(buf, ",", 4);
	if (!strcmp("Agilent Technologies", tokens[0])
			&& tokens[1] && tokens[2] && tokens[3]) {
		for (i = 0; supported_agdmm[i].model; i++) {
			if (strcmp(supported_agdmm[i].modelname, tokens[1]))
				continue;
			sdi = g_malloc0(sizeof(struct sr_dev_inst));
			sdi->status = SR_ST_INACTIVE;
			sdi->vendor = g_strdup("Agilent");
			sdi->model = g_strdup(tokens[1]);
			sdi->version = g_strdup(tokens[3]);
			devc = g_malloc0(sizeof(struct dev_context));
			devc->profile = &supported_agdmm[i];
			devc->cur_mq = -1;
			sdi->inst_type = SR_INST_SERIAL;
			sdi->conn = serial;
			sdi->priv = devc;
			sdi->driver = di;
			sr_channel_new(sdi, 0, SR_CHANNEL_ANALOG, TRUE, "P1");
			drvc->instances = g_slist_append(drvc->instances, sdi);
			devices = g_slist_append(devices, sdi);
			break;
		}
	}
	g_strfreev(tokens);
	g_free(buf);

	serial_close(serial);
	if (!devices)
		sr_serial_dev_inst_free(serial);

	return devices;
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

	switch (key) {
	case SR_CONF_LIMIT_MSEC:
		/* TODO: not yet implemented */
		devc->limit_msec = g_variant_get_uint64(data);
		break;
	case SR_CONF_LIMIT_SAMPLES:
		devc->limit_samples = g_variant_get_uint64(data);
		break;
	default:
		return SR_ERR_NA;
	}

	return SR_OK;
}

static int config_list(uint32_t key, GVariant **data, const struct sr_dev_inst *sdi,
		const struct sr_channel_group *cg)
{
	(void)cg;

	switch (key) {
	case SR_CONF_SCAN_OPTIONS:
		*data = g_variant_new_fixed_array(G_VARIANT_TYPE_UINT32,
				scanopts, ARRAY_SIZE(scanopts), sizeof(uint32_t));
		break;
	case SR_CONF_DEVICE_OPTIONS:
		if (!sdi)
			*data = g_variant_new_fixed_array(G_VARIANT_TYPE_UINT32,
					drvopts, ARRAY_SIZE(drvopts), sizeof(uint32_t));
		else
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
	struct sr_serial_dev_inst *serial;

	if (sdi->status != SR_ST_ACTIVE)
		return SR_ERR_DEV_CLOSED;

	std_session_send_df_header(sdi, LOG_PREFIX);

	/* Poll every 100ms, or whenever some data comes in. */
	serial = sdi->conn;
	serial_source_add(sdi->session, serial, G_IO_IN, 100,
			agdmm_receive_data, (void *)sdi);

	return SR_OK;
}

static int dev_acquisition_stop(struct sr_dev_inst *sdi)
{
	return std_serial_dev_acquisition_stop(sdi, std_serial_dev_close,
			sdi->conn, LOG_PREFIX);
}

SR_PRIV struct sr_dev_driver agdmm_driver_info = {
	.name = "agilent-dmm",
	.longname = "Agilent U12xx series DMMs",
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
	.dev_close = std_serial_dev_close,
	.dev_acquisition_start = dev_acquisition_start,
	.dev_acquisition_stop = dev_acquisition_stop,
	.context = NULL,
};
