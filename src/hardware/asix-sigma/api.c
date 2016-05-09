/*
 * This file is part of the libsigrok project.
 *
 * Copyright (C) 2010-2012 Håvard Espeland <gus@ping.uio.no>,
 * Copyright (C) 2010 Martin Stensgård <mastensg@ping.uio.no>
 * Copyright (C) 2010 Carl Henrik Lunde <chlunde@ping.uio.no>
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

/*
 * ASIX SIGMA/SIGMA2 logic analyzer driver
 */

#include <config.h>
#include "protocol.h"

SR_PRIV struct sr_dev_driver asix_sigma_driver_info;

/*
 * Channel numbers seem to go from 1-16, according to this image:
 * http://tools.asix.net/img/sigma_sigmacab_pins_720.jpg
 * (the cable has two additional GND pins, and a TI and TO pin)
 */
static const char *channel_names[] = {
	"1", "2", "3", "4", "5", "6", "7", "8",
	"9", "10", "11", "12", "13", "14", "15", "16",
};

static const uint32_t drvopts[] = {
	SR_CONF_LOGIC_ANALYZER,
};

static const uint32_t devopts[] = {
	SR_CONF_LIMIT_MSEC | SR_CONF_GET | SR_CONF_SET,
	SR_CONF_LIMIT_SAMPLES | SR_CONF_SET,
	SR_CONF_SAMPLERATE | SR_CONF_GET | SR_CONF_SET | SR_CONF_LIST,
	SR_CONF_TRIGGER_MATCH | SR_CONF_LIST,
	SR_CONF_CAPTURE_RATIO | SR_CONF_GET | SR_CONF_SET,
};

static const int32_t trigger_matches[] = {
	SR_TRIGGER_ZERO,
	SR_TRIGGER_ONE,
	SR_TRIGGER_RISING,
	SR_TRIGGER_FALLING,
};


static int dev_clear(const struct sr_dev_driver *di)
{
	return std_dev_clear(di, sigma_clear_helper);
}

static GSList *scan(struct sr_dev_driver *di, GSList *options)
{
	struct sr_dev_inst *sdi;
	struct drv_context *drvc;
	struct dev_context *devc;
	GSList *devices;
	struct ftdi_device_list *devlist;
	char serial_txt[10];
	uint32_t serial;
	int ret;
	unsigned int i;

	(void)options;

	drvc = di->context;

	devices = NULL;

	devc = g_malloc0(sizeof(struct dev_context));

	ftdi_init(&devc->ftdic);

	/* Look for SIGMAs. */

	if ((ret = ftdi_usb_find_all(&devc->ftdic, &devlist,
	    USB_VENDOR, USB_PRODUCT)) <= 0) {
		if (ret < 0)
			sr_err("ftdi_usb_find_all(): %d", ret);
		goto free;
	}

	/* Make sure it's a version 1 or 2 SIGMA. */
	ftdi_usb_get_strings(&devc->ftdic, devlist->dev, NULL, 0, NULL, 0,
			     serial_txt, sizeof(serial_txt));
	sscanf(serial_txt, "%x", &serial);

	if (serial < 0xa6010000 || serial > 0xa602ffff) {
		sr_err("Only SIGMA and SIGMA2 are supported "
		       "in this version of libsigrok.");
		goto free;
	}

	sr_info("Found ASIX SIGMA - Serial: %s", serial_txt);

	devc->cur_samplerate = samplerates[0];
	devc->period_ps = 0;
	devc->limit_msec = 0;
	devc->cur_firmware = -1;
	devc->num_channels = 0;
	devc->samples_per_event = 0;
	devc->capture_ratio = 50;
	devc->use_triggers = 0;

	/* Register SIGMA device. */
	sdi = g_malloc0(sizeof(struct sr_dev_inst));
	sdi->status = SR_ST_INITIALIZING;
	sdi->vendor = g_strdup(USB_VENDOR_NAME);
	sdi->model = g_strdup(USB_MODEL_NAME);
	sdi->driver = di;

	for (i = 0; i < ARRAY_SIZE(channel_names); i++)
		sr_channel_new(sdi, i, SR_CHANNEL_LOGIC, TRUE, channel_names[i]);

	devices = g_slist_append(devices, sdi);
	drvc->instances = g_slist_append(drvc->instances, sdi);
	sdi->priv = devc;

	/* We will open the device again when we need it. */
	ftdi_list_free(&devlist);

	return devices;

free:
	ftdi_deinit(&devc->ftdic);
	g_free(devc);
	return NULL;
}

static int dev_open(struct sr_dev_inst *sdi)
{
	struct dev_context *devc;
	int ret;

	devc = sdi->priv;

	/* Make sure it's an ASIX SIGMA. */
	if ((ret = ftdi_usb_open_desc(&devc->ftdic,
		USB_VENDOR, USB_PRODUCT, USB_DESCRIPTION, NULL)) < 0) {

		sr_err("ftdi_usb_open failed: %s",
		       ftdi_get_error_string(&devc->ftdic));

		return 0;
	}

	sdi->status = SR_ST_ACTIVE;

	return SR_OK;
}

static int dev_close(struct sr_dev_inst *sdi)
{
	struct dev_context *devc;

	devc = sdi->priv;

	/* TODO */
	if (sdi->status == SR_ST_ACTIVE)
		ftdi_usb_close(&devc->ftdic);

	sdi->status = SR_ST_INACTIVE;

	return SR_OK;
}

static int config_get(uint32_t key, GVariant **data, const struct sr_dev_inst *sdi,
		const struct sr_channel_group *cg)
{
	struct dev_context *devc;

	(void)cg;

	if (!sdi)
		return SR_ERR;
	devc = sdi->priv;

	switch (key) {
	case SR_CONF_SAMPLERATE:
		*data = g_variant_new_uint64(devc->cur_samplerate);
		break;
	case SR_CONF_LIMIT_MSEC:
		*data = g_variant_new_uint64(devc->limit_msec);
		break;
	case SR_CONF_CAPTURE_RATIO:
		*data = g_variant_new_uint64(devc->capture_ratio);
		break;
	default:
		return SR_ERR_NA;
	}

	return SR_OK;
}

static int config_set(uint32_t key, GVariant *data, const struct sr_dev_inst *sdi,
		const struct sr_channel_group *cg)
{
	struct dev_context *devc;
	uint64_t tmp;
	int ret;

	(void)cg;

	if (sdi->status != SR_ST_ACTIVE)
		return SR_ERR_DEV_CLOSED;

	devc = sdi->priv;

	ret = SR_OK;
	switch (key) {
	case SR_CONF_SAMPLERATE:
		ret = sigma_set_samplerate(sdi, g_variant_get_uint64(data));
		break;
	case SR_CONF_LIMIT_MSEC:
		tmp = g_variant_get_uint64(data);
		if (tmp > 0)
			devc->limit_msec = g_variant_get_uint64(data);
		else
			ret = SR_ERR;
		break;
	case SR_CONF_LIMIT_SAMPLES:
		tmp = g_variant_get_uint64(data);
		devc->limit_msec = tmp * 1000 / devc->cur_samplerate;
		break;
	case SR_CONF_CAPTURE_RATIO:
		tmp = g_variant_get_uint64(data);
		if (tmp <= 100)
			devc->capture_ratio = tmp;
		else
			ret = SR_ERR;
		break;
	default:
		ret = SR_ERR_NA;
	}

	return ret;
}

static int config_list(uint32_t key, GVariant **data, const struct sr_dev_inst *sdi,
		const struct sr_channel_group *cg)
{
	GVariant *gvar;
	GVariantBuilder gvb;

	(void)cg;

	switch (key) {
	case SR_CONF_DEVICE_OPTIONS:
		if (!sdi)
			*data = g_variant_new_fixed_array(G_VARIANT_TYPE_UINT32,
					drvopts, ARRAY_SIZE(drvopts), sizeof(uint32_t));
		else
			*data = g_variant_new_fixed_array(G_VARIANT_TYPE_UINT32,
					devopts, ARRAY_SIZE(devopts), sizeof(uint32_t));
		break;
	case SR_CONF_SAMPLERATE:
		g_variant_builder_init(&gvb, G_VARIANT_TYPE("a{sv}"));
		gvar = g_variant_new_fixed_array(G_VARIANT_TYPE("t"), samplerates,
				SAMPLERATES_COUNT, sizeof(uint64_t));
		g_variant_builder_add(&gvb, "{sv}", "samplerates", gvar);
		*data = g_variant_builder_end(&gvb);
		break;
	case SR_CONF_TRIGGER_MATCH:
		*data = g_variant_new_fixed_array(G_VARIANT_TYPE_INT32,
				trigger_matches, ARRAY_SIZE(trigger_matches),
				sizeof(int32_t));
		break;
	default:
		return SR_ERR_NA;
	}

	return SR_OK;
}

static int dev_acquisition_start(const struct sr_dev_inst *sdi)
{
	struct dev_context *devc;
	struct clockselect_50 clockselect;
	int frac, triggerpin, ret;
	uint8_t triggerselect = 0;
	struct triggerinout triggerinout_conf;
	struct triggerlut lut;

	if (sdi->status != SR_ST_ACTIVE)
		return SR_ERR_DEV_CLOSED;

	devc = sdi->priv;

	if (sigma_convert_trigger(sdi) != SR_OK) {
		sr_err("Failed to configure triggers.");
		return SR_ERR;
	}

	/* If the samplerate has not been set, default to 200 kHz. */
	if (devc->cur_firmware == -1) {
		if ((ret = sigma_set_samplerate(sdi, SR_KHZ(200))) != SR_OK)
			return ret;
	}

	/* Enter trigger programming mode. */
	sigma_set_register(WRITE_TRIGGER_SELECT1, 0x20, devc);

	/* 100 and 200 MHz mode. */
	if (devc->cur_samplerate >= SR_MHZ(100)) {
		sigma_set_register(WRITE_TRIGGER_SELECT1, 0x81, devc);

		/* Find which pin to trigger on from mask. */
		for (triggerpin = 0; triggerpin < 8; triggerpin++)
			if ((devc->trigger.risingmask | devc->trigger.fallingmask) &
			    (1 << triggerpin))
				break;

		/* Set trigger pin and light LED on trigger. */
		triggerselect = (1 << LEDSEL1) | (triggerpin & 0x7);

		/* Default rising edge. */
		if (devc->trigger.fallingmask)
			triggerselect |= 1 << 3;

	/* All other modes. */
	} else if (devc->cur_samplerate <= SR_MHZ(50)) {
		sigma_build_basic_trigger(&lut, devc);

		sigma_write_trigger_lut(&lut, devc);

		triggerselect = (1 << LEDSEL1) | (1 << LEDSEL0);
	}

	/* Setup trigger in and out pins to default values. */
	memset(&triggerinout_conf, 0, sizeof(struct triggerinout));
	triggerinout_conf.trgout_bytrigger = 1;
	triggerinout_conf.trgout_enable = 1;

	sigma_write_register(WRITE_TRIGGER_OPTION,
			     (uint8_t *) &triggerinout_conf,
			     sizeof(struct triggerinout), devc);

	/* Go back to normal mode. */
	sigma_set_register(WRITE_TRIGGER_SELECT1, triggerselect, devc);

	/* Set clock select register. */
	if (devc->cur_samplerate == SR_MHZ(200))
		/* Enable 4 channels. */
		sigma_set_register(WRITE_CLOCK_SELECT, 0xf0, devc);
	else if (devc->cur_samplerate == SR_MHZ(100))
		/* Enable 8 channels. */
		sigma_set_register(WRITE_CLOCK_SELECT, 0x00, devc);
	else {
		/*
		 * 50 MHz mode (or fraction thereof). Any fraction down to
		 * 50 MHz / 256 can be used, but is not supported by sigrok API.
		 */
		frac = SR_MHZ(50) / devc->cur_samplerate - 1;

		clockselect.async = 0;
		clockselect.fraction = frac;
		clockselect.disabled_channels = 0;

		sigma_write_register(WRITE_CLOCK_SELECT,
				     (uint8_t *) &clockselect,
				     sizeof(clockselect), devc);
	}

	/* Setup maximum post trigger time. */
	sigma_set_register(WRITE_POST_TRIGGER,
			   (devc->capture_ratio * 255) / 100, devc);

	/* Start acqusition. */
	gettimeofday(&devc->start_tv, 0);
	sigma_set_register(WRITE_MODE, 0x0d, devc);

	std_session_send_df_header(sdi, LOG_PREFIX);

	/* Add capture source. */
	sr_session_source_add(sdi->session, -1, 0, 10, sigma_receive_data, (void *)sdi);

	devc->state.state = SIGMA_CAPTURE;

	return SR_OK;
}

static int dev_acquisition_stop(struct sr_dev_inst *sdi)
{
	struct dev_context *devc;

	devc = sdi->priv;
	devc->state.state = SIGMA_IDLE;

	sr_session_source_remove(sdi->session, -1);

	return SR_OK;
}

SR_PRIV struct sr_dev_driver asix_sigma_driver_info = {
	.name = "asix-sigma",
	.longname = "ASIX SIGMA/SIGMA2",
	.api_version = 1,
	.init = std_init,
	.cleanup = std_cleanup,
	.scan = scan,
	.dev_list = std_dev_list,
	.dev_clear = dev_clear,
	.config_get = config_get,
	.config_set = config_set,
	.config_list = config_list,
	.dev_open = dev_open,
	.dev_close = dev_close,
	.dev_acquisition_start = dev_acquisition_start,
	.dev_acquisition_stop = dev_acquisition_stop,
	.context = NULL,
};
