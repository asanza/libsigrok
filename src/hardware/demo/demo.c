/*
 * This file is part of the libsigrok project.
 *
 * Copyright (C) 2010 Uwe Hermann <uwe@hermann-uwe.de>
 * Copyright (C) 2011 Olivier Fauchon <olivier@aixmarseille.com>
 * Copyright (C) 2012 Alexandru Gagniuc <mr.nuke.me@gmail.com>
 * Copyright (C) 2015 Bartosz Golaszewski <bgolaszewski@baylibre.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301 USA
 */

#include <config.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <libsigrok/libsigrok.h>
#include "libsigrok-internal.h"

#define LOG_PREFIX "demo"

#define DEFAULT_NUM_LOGIC_CHANNELS     8
#define DEFAULT_NUM_ANALOG_CHANNELS    4

/* The size in bytes of chunks to send through the session bus. */
#define LOGIC_BUFSIZE        4096
/* Size of the analog pattern space per channel. */
#define ANALOG_BUFSIZE       4096

#define DEFAULT_ANALOG_AMPLITUDE 10
#define ANALOG_SAMPLES_PER_PERIOD 20

/* Logic patterns we can generate. */
enum {
	/**
	 * Spells "sigrok" across 8 channels using '0's (with '1's as
	 * "background") when displayed using the 'bits' output format.
	 * The pattern is repeated every 8 channels, shifted to the right
	 * in time by one bit.
	 */
	PATTERN_SIGROK,

	/** Pseudo-random values on all channels. */
	PATTERN_RANDOM,

	/**
	 * Incrementing number across 8 channels. The pattern is repeated
	 * every 8 channels, shifted to the right in time by one bit.
	 */
	PATTERN_INC,

	/** All channels have a low logic state. */
	PATTERN_ALL_LOW,

	/** All channels have a high logic state. */
	PATTERN_ALL_HIGH,
};

/* Analog patterns we can generate. */
enum {
	/**
	 * Square wave.
	 */
	PATTERN_SQUARE,
	PATTERN_SINE,
	PATTERN_TRIANGLE,
	PATTERN_SAWTOOTH,
};

static const char *logic_pattern_str[] = {
	"sigrok",
	"random",
	"incremental",
	"all-low",
	"all-high",
};

static const char *analog_pattern_str[] = {
	"square",
	"sine",
	"triangle",
	"sawtooth",
};

struct analog_gen {
	int pattern;
	float amplitude;
	float pattern_data[ANALOG_BUFSIZE];
	unsigned int num_samples;
	struct sr_datafeed_analog_old packet;
	float avg_val; /* Average value */
	unsigned num_avgs; /* Number of samples averaged */
};

/* Private, per-device-instance driver context. */
struct dev_context {
	uint64_t cur_samplerate;
	uint64_t limit_samples;
	uint64_t limit_msec;
	uint64_t sent_samples;
	int64_t start_us;
	int64_t spent_us;
	uint64_t step;
	/* Logic */
	int32_t num_logic_channels;
	unsigned int logic_unitsize;
	/* There is only ever one logic channel group, so its pattern goes here. */
	uint8_t logic_pattern;
	unsigned char logic_data[LOGIC_BUFSIZE];
	/* Analog */
	int32_t num_analog_channels;
	GHashTable *ch_ag;
	gboolean avg; /* True if averaging is enabled */
	uint64_t avg_samples;
};

static const uint32_t drvopts[] = {
	SR_CONF_DEMO_DEV,
	SR_CONF_LOGIC_ANALYZER,
	SR_CONF_OSCILLOSCOPE,
};

static const uint32_t scanopts[] = {
	SR_CONF_NUM_LOGIC_CHANNELS,
	SR_CONF_NUM_ANALOG_CHANNELS,
};

static const uint32_t devopts[] = {
	SR_CONF_CONTINUOUS,
	SR_CONF_LIMIT_SAMPLES | SR_CONF_GET | SR_CONF_SET,
	SR_CONF_LIMIT_MSEC | SR_CONF_GET | SR_CONF_SET,
	SR_CONF_SAMPLERATE | SR_CONF_GET | SR_CONF_SET | SR_CONF_LIST,
	SR_CONF_AVERAGING | SR_CONF_GET | SR_CONF_SET,
	SR_CONF_AVG_SAMPLES | SR_CONF_GET | SR_CONF_SET,
};

static const uint32_t devopts_cg_logic[] = {
	SR_CONF_PATTERN_MODE | SR_CONF_GET | SR_CONF_SET | SR_CONF_LIST,
};

static const uint32_t devopts_cg_analog_group[] = {
	SR_CONF_AMPLITUDE | SR_CONF_GET | SR_CONF_SET,
};

static const uint32_t devopts_cg_analog_channel[] = {
	SR_CONF_PATTERN_MODE | SR_CONF_GET | SR_CONF_SET | SR_CONF_LIST,
	SR_CONF_AMPLITUDE | SR_CONF_GET | SR_CONF_SET,
};

static const uint64_t samplerates[] = {
	SR_HZ(1),
	SR_GHZ(1),
	SR_HZ(1),
};

static const uint8_t pattern_sigrok[] = {
	0x4c, 0x92, 0x92, 0x92, 0x64, 0x00, 0x00, 0x00,
	0x82, 0xfe, 0xfe, 0x82, 0x00, 0x00, 0x00, 0x00,
	0x7c, 0x82, 0x82, 0x92, 0x74, 0x00, 0x00, 0x00,
	0xfe, 0x12, 0x12, 0x32, 0xcc, 0x00, 0x00, 0x00,
	0x7c, 0x82, 0x82, 0x82, 0x7c, 0x00, 0x00, 0x00,
	0xfe, 0x10, 0x28, 0x44, 0x82, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0xbe, 0xbe, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

SR_PRIV struct sr_dev_driver demo_driver_info;

static int dev_acquisition_stop(struct sr_dev_inst *sdi);

static void generate_analog_pattern(struct analog_gen *ag, uint64_t sample_rate)
{
	double t, frequency;
	float value;
	unsigned int num_samples, i;
	int last_end;

	sr_dbg("Generating %s pattern.", analog_pattern_str[ag->pattern]);

	num_samples = ANALOG_BUFSIZE / sizeof(float);

	switch (ag->pattern) {
	case PATTERN_SQUARE:
		value = ag->amplitude;
		last_end = 0;
		for (i = 0; i < num_samples; i++) {
			if (i % 5 == 0)
				value = -value;
			if (i % 10 == 0)
				last_end = i;
			ag->pattern_data[i] = value;
		}
		ag->num_samples = last_end;
		break;
	case PATTERN_SINE:
		frequency = (double) sample_rate / ANALOG_SAMPLES_PER_PERIOD;

		/* Make sure the number of samples we put out is an integer
		 * multiple of our period size */
		/* FIXME we actually need only one period. A ringbuffer would be
		 * useful here. */
		while (num_samples % ANALOG_SAMPLES_PER_PERIOD != 0)
			num_samples--;

		for (i = 0; i < num_samples; i++) {
			t = (double) i / (double) sample_rate;
			ag->pattern_data[i] = ag->amplitude *
						sin(2 * G_PI * frequency * t);
		}

		ag->num_samples = num_samples;
		break;
	case PATTERN_TRIANGLE:
		frequency = (double) sample_rate / ANALOG_SAMPLES_PER_PERIOD;

		while (num_samples % ANALOG_SAMPLES_PER_PERIOD != 0)
			num_samples--;

		for (i = 0; i < num_samples; i++) {
			t = (double) i / (double) sample_rate;
			ag->pattern_data[i] = (2 * ag->amplitude / G_PI) *
						asin(sin(2 * G_PI * frequency * t));
		}

		ag->num_samples = num_samples;
		break;
	case PATTERN_SAWTOOTH:
		frequency = (double) sample_rate / ANALOG_SAMPLES_PER_PERIOD;

		while (num_samples % ANALOG_SAMPLES_PER_PERIOD != 0)
			num_samples--;

		for (i = 0; i < num_samples; i++) {
			t = (double) i / (double) sample_rate;
			ag->pattern_data[i] = 2 * ag->amplitude *
						((t * frequency) - floor(0.5f + t * frequency));
		}

		ag->num_samples = num_samples;
		break;
	}
}

static GSList *scan(struct sr_dev_driver *di, GSList *options)
{
	struct drv_context *drvc;
	struct dev_context *devc;
	struct sr_dev_inst *sdi;
	struct sr_channel *ch;
	struct sr_channel_group *cg, *acg;
	struct sr_config *src;
	struct analog_gen *ag;
	GSList *devices, *l;
	int num_logic_channels, num_analog_channels, pattern, i;
	char channel_name[16];

	drvc = di->context;

	num_logic_channels = DEFAULT_NUM_LOGIC_CHANNELS;
	num_analog_channels = DEFAULT_NUM_ANALOG_CHANNELS;
	for (l = options; l; l = l->next) {
		src = l->data;
		switch (src->key) {
		case SR_CONF_NUM_LOGIC_CHANNELS:
			num_logic_channels = g_variant_get_int32(src->data);
			break;
		case SR_CONF_NUM_ANALOG_CHANNELS:
			num_analog_channels = g_variant_get_int32(src->data);
			break;
		}
	}

	devices = NULL;

	sdi = g_malloc0(sizeof(struct sr_dev_inst));
	sdi->status = SR_ST_INACTIVE;
	sdi->model = g_strdup("Demo device");
	sdi->driver = di;

	devc = g_malloc0(sizeof(struct dev_context));
	devc->cur_samplerate = SR_KHZ(200);
	devc->num_logic_channels = num_logic_channels;
	devc->logic_unitsize = (devc->num_logic_channels + 7) / 8;
	devc->logic_pattern = PATTERN_SIGROK;
	devc->num_analog_channels = num_analog_channels;

	if (num_logic_channels > 0) {
		/* Logic channels, all in one channel group. */
		cg = g_malloc0(sizeof(struct sr_channel_group));
		cg->name = g_strdup("Logic");
		for (i = 0; i < num_logic_channels; i++) {
			sprintf(channel_name, "D%d", i);
			ch = sr_channel_new(sdi, i, SR_CHANNEL_LOGIC, TRUE, channel_name);
			cg->channels = g_slist_append(cg->channels, ch);
		}
		sdi->channel_groups = g_slist_append(NULL, cg);
	}

	/* Analog channels, channel groups and pattern generators. */
	if (num_analog_channels > 0) {
		pattern = 0;
		/* An "Analog" channel group with all analog channels in it. */
		acg = g_malloc0(sizeof(struct sr_channel_group));
		acg->name = g_strdup("Analog");
		sdi->channel_groups = g_slist_append(sdi->channel_groups, acg);

		devc->ch_ag = g_hash_table_new(g_direct_hash, g_direct_equal);
		for (i = 0; i < num_analog_channels; i++) {
			snprintf(channel_name, 16, "A%d", i);
			ch = sr_channel_new(sdi, i + num_logic_channels, SR_CHANNEL_ANALOG,
					TRUE, channel_name);
			acg->channels = g_slist_append(acg->channels, ch);

			/* Every analog channel gets its own channel group as well. */
			cg = g_malloc0(sizeof(struct sr_channel_group));
			cg->name = g_strdup(channel_name);
			cg->channels = g_slist_append(NULL, ch);
			sdi->channel_groups = g_slist_append(sdi->channel_groups, cg);

			/* Every channel gets a generator struct. */
			ag = g_malloc(sizeof(struct analog_gen));
			ag->amplitude = DEFAULT_ANALOG_AMPLITUDE;
			ag->packet.channels = cg->channels;
			ag->packet.mq = 0;
			ag->packet.mqflags = 0;
			ag->packet.unit = SR_UNIT_VOLT;
			ag->packet.data = ag->pattern_data;
			ag->pattern = pattern;
			ag->avg_val = 0.0f;
			ag->num_avgs = 0;
			g_hash_table_insert(devc->ch_ag, ch, ag);

			if (++pattern == ARRAY_SIZE(analog_pattern_str))
				pattern = 0;
		}
	}

	sdi->priv = devc;
	devices = g_slist_append(devices, sdi);
	drvc->instances = g_slist_append(drvc->instances, sdi);

	return devices;
}

static int dev_open(struct sr_dev_inst *sdi)
{
	sdi->status = SR_ST_ACTIVE;

	return SR_OK;
}

static int dev_close(struct sr_dev_inst *sdi)
{
	sdi->status = SR_ST_INACTIVE;

	return SR_OK;
}

static void clear_helper(void *priv)
{
	struct dev_context *devc;
	GHashTableIter iter;
	void *value;

	devc = priv;

	/* Analog generators. */
	g_hash_table_iter_init(&iter, devc->ch_ag);
	while (g_hash_table_iter_next(&iter, NULL, &value))
		g_free(value);
	g_hash_table_unref(devc->ch_ag);
	g_free(devc);
}

static int dev_clear(const struct sr_dev_driver *di)
{
	return std_dev_clear(di, clear_helper);
}

static int config_get(uint32_t key, GVariant **data, const struct sr_dev_inst *sdi,
		const struct sr_channel_group *cg)
{
	struct dev_context *devc;
	struct sr_channel *ch;
	struct analog_gen *ag;
	int pattern;

	if (!sdi)
		return SR_ERR_ARG;

	devc = sdi->priv;
	switch (key) {
	case SR_CONF_SAMPLERATE:
		*data = g_variant_new_uint64(devc->cur_samplerate);
		break;
	case SR_CONF_LIMIT_SAMPLES:
		*data = g_variant_new_uint64(devc->limit_samples);
		break;
	case SR_CONF_LIMIT_MSEC:
		*data = g_variant_new_uint64(devc->limit_msec);
		break;
	case SR_CONF_AVERAGING:
		*data = g_variant_new_boolean(devc->avg);
		break;
	case SR_CONF_AVG_SAMPLES:
		*data = g_variant_new_uint64(devc->avg_samples);
		break;
	case SR_CONF_PATTERN_MODE:
		if (!cg)
			return SR_ERR_CHANNEL_GROUP;
		/* Any channel in the group will do. */
		ch = cg->channels->data;
		if (ch->type == SR_CHANNEL_LOGIC) {
			pattern = devc->logic_pattern;
			*data = g_variant_new_string(logic_pattern_str[pattern]);
		} else if (ch->type == SR_CHANNEL_ANALOG) {
			ag = g_hash_table_lookup(devc->ch_ag, ch);
			pattern = ag->pattern;
			*data = g_variant_new_string(analog_pattern_str[pattern]);
		} else
			return SR_ERR_BUG;
		break;
	case SR_CONF_AMPLITUDE:
		if (!cg)
			return SR_ERR_CHANNEL_GROUP;
		/* Any channel in the group will do. */
		ch = cg->channels->data;
		if (ch->type != SR_CHANNEL_ANALOG)
			return SR_ERR_ARG;
		ag = g_hash_table_lookup(devc->ch_ag, ch);
		*data = g_variant_new_double(ag->amplitude);
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
	struct analog_gen *ag;
	struct sr_channel *ch;
	GSList *l;
	int logic_pattern, analog_pattern, ret;
	unsigned int i;
	const char *stropt;

	devc = sdi->priv;

	if (sdi->status != SR_ST_ACTIVE)
		return SR_ERR_DEV_CLOSED;

	ret = SR_OK;
	switch (key) {
	case SR_CONF_SAMPLERATE:
		devc->cur_samplerate = g_variant_get_uint64(data);
		break;
	case SR_CONF_LIMIT_SAMPLES:
		devc->limit_msec = 0;
		devc->limit_samples = g_variant_get_uint64(data);
		break;
	case SR_CONF_LIMIT_MSEC:
		devc->limit_msec = g_variant_get_uint64(data);
		devc->limit_samples = 0;
		break;
	case SR_CONF_AVERAGING:
		devc->avg = g_variant_get_boolean(data);
		sr_dbg("%s averaging", devc->avg ? "Enabling" : "Disabling");
		break;
	case SR_CONF_AVG_SAMPLES:
		devc->avg_samples = g_variant_get_uint64(data);
		sr_dbg("Setting averaging rate to %" PRIu64, devc->avg_samples);
		break;
	case SR_CONF_PATTERN_MODE:
		if (!cg)
			return SR_ERR_CHANNEL_GROUP;
		stropt = g_variant_get_string(data, NULL);
		logic_pattern = analog_pattern = -1;
		for (i = 0; i < ARRAY_SIZE(logic_pattern_str); i++) {
			if (!strcmp(stropt, logic_pattern_str[i])) {
				logic_pattern = i;
				break;
			}
		}
		for (i = 0; i < ARRAY_SIZE(analog_pattern_str); i++) {
			if (!strcmp(stropt, analog_pattern_str[i])) {
				analog_pattern = i;
				break;
			}
		}
		if (logic_pattern == -1 && analog_pattern == -1)
			return SR_ERR_ARG;
		for (l = cg->channels; l; l = l->next) {
			ch = l->data;
			if (ch->type == SR_CHANNEL_LOGIC) {
				if (logic_pattern == -1)
					return SR_ERR_ARG;
				sr_dbg("Setting logic pattern to %s",
						logic_pattern_str[logic_pattern]);
				devc->logic_pattern = logic_pattern;
				/* Might as well do this now, these are static. */
				if (logic_pattern == PATTERN_ALL_LOW)
					memset(devc->logic_data, 0x00, LOGIC_BUFSIZE);
				else if (logic_pattern == PATTERN_ALL_HIGH)
					memset(devc->logic_data, 0xff, LOGIC_BUFSIZE);
			} else if (ch->type == SR_CHANNEL_ANALOG) {
				if (analog_pattern == -1)
					return SR_ERR_ARG;
				sr_dbg("Setting analog pattern for channel %s to %s",
						ch->name, analog_pattern_str[analog_pattern]);
				ag = g_hash_table_lookup(devc->ch_ag, ch);
				ag->pattern = analog_pattern;
			} else
				return SR_ERR_BUG;
		}
		break;
	case SR_CONF_AMPLITUDE:
		if (!cg)
			return SR_ERR_CHANNEL_GROUP;
		for (l = cg->channels; l; l = l->next) {
			ch = l->data;
			if (ch->type != SR_CHANNEL_ANALOG)
				return SR_ERR_ARG;
			ag = g_hash_table_lookup(devc->ch_ag, ch);
			ag->amplitude = g_variant_get_double(data);
		}
		break;
	default:
		ret = SR_ERR_NA;
	}

	return ret;
}

static int config_list(uint32_t key, GVariant **data, const struct sr_dev_inst *sdi,
		const struct sr_channel_group *cg)
{
	struct sr_channel *ch;
	GVariant *gvar;
	GVariantBuilder gvb;

	if (key == SR_CONF_SCAN_OPTIONS) {
		*data = g_variant_new_fixed_array(G_VARIANT_TYPE_UINT32,
				scanopts, ARRAY_SIZE(scanopts), sizeof(uint32_t));
		return SR_OK;
	}

	if (key == SR_CONF_DEVICE_OPTIONS && !sdi) {
		*data = g_variant_new_fixed_array(G_VARIANT_TYPE_UINT32,
				drvopts, ARRAY_SIZE(drvopts), sizeof(uint32_t));
		return SR_OK;
	}

	if (!sdi)
		return SR_ERR_ARG;

	if (!cg) {
		switch (key) {
		case SR_CONF_DEVICE_OPTIONS:
			*data = g_variant_new_fixed_array(G_VARIANT_TYPE_UINT32,
					devopts, ARRAY_SIZE(devopts), sizeof(uint32_t));
			break;
		case SR_CONF_SAMPLERATE:
			g_variant_builder_init(&gvb, G_VARIANT_TYPE("a{sv}"));
			gvar = g_variant_new_fixed_array(G_VARIANT_TYPE("t"), samplerates,
					ARRAY_SIZE(samplerates), sizeof(uint64_t));
			g_variant_builder_add(&gvb, "{sv}", "samplerate-steps", gvar);
			*data = g_variant_builder_end(&gvb);
			break;
		default:
			return SR_ERR_NA;
		}
	} else {
		ch = cg->channels->data;
		switch (key) {
		case SR_CONF_DEVICE_OPTIONS:
			if (ch->type == SR_CHANNEL_LOGIC)
				*data = g_variant_new_fixed_array(G_VARIANT_TYPE_UINT32,
						devopts_cg_logic, ARRAY_SIZE(devopts_cg_logic),
						sizeof(uint32_t));
			else if (ch->type == SR_CHANNEL_ANALOG) {
				if (strcmp(cg->name, "Analog") == 0)
					*data = g_variant_new_fixed_array(G_VARIANT_TYPE_UINT32,
							devopts_cg_analog_group, ARRAY_SIZE(devopts_cg_analog_group),
							sizeof(uint32_t));
				else
					*data = g_variant_new_fixed_array(G_VARIANT_TYPE_UINT32,
							devopts_cg_analog_channel, ARRAY_SIZE(devopts_cg_analog_channel),
							sizeof(uint32_t));
			}
			else
				return SR_ERR_BUG;
			break;
		case SR_CONF_PATTERN_MODE:
			/* The analog group (with all 4 channels) shall not have a pattern property. */
			if (strcmp(cg->name, "Analog") == 0)
				return SR_ERR_NA;

			if (ch->type == SR_CHANNEL_LOGIC)
				*data = g_variant_new_strv(logic_pattern_str,
						ARRAY_SIZE(logic_pattern_str));
			else if (ch->type == SR_CHANNEL_ANALOG)
				*data = g_variant_new_strv(analog_pattern_str,
						ARRAY_SIZE(analog_pattern_str));
			else
				return SR_ERR_BUG;
			break;
		default:
			return SR_ERR_NA;
		}
	}

	return SR_OK;
}

static void logic_generator(struct sr_dev_inst *sdi, uint64_t size)
{
	struct dev_context *devc;
	uint64_t i, j;
	uint8_t pat;

	devc = sdi->priv;

	switch (devc->logic_pattern) {
	case PATTERN_SIGROK:
		memset(devc->logic_data, 0x00, size);
		for (i = 0; i < size; i += devc->logic_unitsize) {
			for (j = 0; j < devc->logic_unitsize; j++) {
				pat = pattern_sigrok[(devc->step + j) % sizeof(pattern_sigrok)] >> 1;
				devc->logic_data[i + j] = ~pat;
			}
			devc->step++;
		}
		break;
	case PATTERN_RANDOM:
		for (i = 0; i < size; i++)
			devc->logic_data[i] = (uint8_t)(rand() & 0xff);
		break;
	case PATTERN_INC:
		for (i = 0; i < size; i++) {
			for (j = 0; j < devc->logic_unitsize; j++) {
				devc->logic_data[i + j] = devc->step;
			}
			devc->step++;
		}
		break;
	case PATTERN_ALL_LOW:
	case PATTERN_ALL_HIGH:
		/* These were set when the pattern mode was selected. */
		break;
	default:
		sr_err("Unknown pattern: %d.", devc->logic_pattern);
		break;
	}
}

static void send_analog_packet(struct analog_gen *ag,
			       struct sr_dev_inst *sdi,
			       uint64_t *analog_sent,
			       uint64_t analog_pos,
			       uint64_t analog_todo)
{
	struct sr_datafeed_packet packet;
	struct dev_context *devc;
	uint64_t sending_now, to_avg;
	int ag_pattern_pos;
	unsigned int i;

	devc = sdi->priv;
	packet.type = SR_DF_ANALOG_OLD;
	packet.payload = &ag->packet;

	if (!devc->avg) {
		ag_pattern_pos = analog_pos % ag->num_samples;
		sending_now = MIN(analog_todo, ag->num_samples-ag_pattern_pos);
		ag->packet.data = ag->pattern_data + ag_pattern_pos;
		ag->packet.num_samples = sending_now;
		sr_session_send(sdi, &packet);

		/* Whichever channel group gets there first. */
		*analog_sent = MAX(*analog_sent, sending_now);
	} else {
		ag_pattern_pos = analog_pos % ag->num_samples;
		to_avg = MIN(analog_todo, ag->num_samples-ag_pattern_pos);

		for (i = 0; i < to_avg; i++) {
			ag->avg_val = (ag->avg_val +
					*(ag->pattern_data +
					  ag_pattern_pos + i)) / 2;
			ag->num_avgs++;
			/* Time to send averaged data? */
			if (devc->avg_samples > 0 &&
			    ag->num_avgs >= devc->avg_samples)
				goto do_send;
		}

		if (devc->avg_samples == 0) {
			/* We're averaging all the samples, so wait with
			 * sending until the very end.
			 */
			*analog_sent = ag->num_avgs;
			return;
		}

do_send:
		ag->packet.data = &ag->avg_val;
		ag->packet.num_samples = 1;

		sr_session_send(sdi, &packet);
		*analog_sent = ag->num_avgs;

		ag->num_avgs = 0;
		ag->avg_val = 0.0f;
	}
}

/* Callback handling data */
static int prepare_data(int fd, int revents, void *cb_data)
{
	struct sr_dev_inst *sdi;
	struct dev_context *devc;
	struct sr_datafeed_packet packet;
	struct sr_datafeed_logic logic;
	struct analog_gen *ag;
	GHashTableIter iter;
	void *value;
	uint64_t samples_todo, logic_done, analog_done, analog_sent, sending_now;
	int64_t elapsed_us, limit_us, todo_us;

	(void)fd;
	(void)revents;

	sdi = cb_data;
	devc = sdi->priv;

	/* Just in case. */
	if (devc->cur_samplerate <= 0
			|| (devc->num_logic_channels <= 0
			&& devc->num_analog_channels <= 0)) {
		dev_acquisition_stop(sdi);
		return G_SOURCE_CONTINUE;
	}

	/* What time span should we send samples for? */
	elapsed_us = g_get_monotonic_time() - devc->start_us;
	limit_us = 1000 * devc->limit_msec;
	if (limit_us > 0 && limit_us < elapsed_us)
		todo_us = MAX(0, limit_us - devc->spent_us);
	else
		todo_us = MAX(0, elapsed_us - devc->spent_us);

	/* How many samples are outstanding since the last round? */
	samples_todo = (todo_us * devc->cur_samplerate + G_USEC_PER_SEC - 1)
			/ G_USEC_PER_SEC;
	if (devc->limit_samples > 0) {
		if (devc->limit_samples < devc->sent_samples)
			samples_todo = 0;
		else if (devc->limit_samples - devc->sent_samples < samples_todo)
			samples_todo = devc->limit_samples - devc->sent_samples;
	}
	/* Calculate the actual time covered by this run back from the sample
	 * count, rounded towards zero. This avoids getting stuck on a too-low
	 * time delta with no samples being sent due to round-off.
	 */
	todo_us = samples_todo * G_USEC_PER_SEC / devc->cur_samplerate;

	logic_done  = devc->num_logic_channels  > 0 ? 0 : samples_todo;
	analog_done = devc->num_analog_channels > 0 ? 0 : samples_todo;

	while (logic_done < samples_todo || analog_done < samples_todo) {
		/* Logic */
		if (logic_done < samples_todo) {
			sending_now = MIN(samples_todo - logic_done,
					LOGIC_BUFSIZE / devc->logic_unitsize);
			logic_generator(sdi, sending_now * devc->logic_unitsize);
			packet.type = SR_DF_LOGIC;
			packet.payload = &logic;
			logic.length = sending_now * devc->logic_unitsize;
			logic.unitsize = devc->logic_unitsize;
			logic.data = devc->logic_data;
			sr_session_send(sdi, &packet);
			logic_done += sending_now;
		}

		/* Analog, one channel at a time */
		if (analog_done < samples_todo) {
			analog_sent = 0;

			g_hash_table_iter_init(&iter, devc->ch_ag);
			while (g_hash_table_iter_next(&iter, NULL, &value)) {
				send_analog_packet(value, sdi, &analog_sent,
						devc->sent_samples + analog_done,
						samples_todo - analog_done);
			}
			analog_done += analog_sent;
		}
	}
	/* At this point, both logic_done and analog_done should be
	 * exactly equal to samples_todo, or else.
	 */
	if (logic_done != samples_todo || analog_done != samples_todo) {
		sr_err("BUG: Sample count mismatch.");
		return G_SOURCE_REMOVE;
	}
	devc->sent_samples += samples_todo;
	devc->spent_us += todo_us;

	if ((devc->limit_samples > 0 && devc->sent_samples >= devc->limit_samples)
			|| (limit_us > 0 && devc->spent_us >= limit_us)) {

		/* If we're averaging everything - now is the time to send data */
		if (devc->avg_samples == 0) {
			g_hash_table_iter_init(&iter, devc->ch_ag);
			while (g_hash_table_iter_next(&iter, NULL, &value)) {
				ag = value;
				packet.type = SR_DF_ANALOG_OLD;
				packet.payload = &ag->packet;
				ag->packet.data = &ag->avg_val;
				ag->packet.num_samples = 1;
				sr_session_send(sdi, &packet);
			}
		}
		sr_dbg("Requested number of samples reached.");
		dev_acquisition_stop(sdi);
	}

	return G_SOURCE_CONTINUE;
}

static int dev_acquisition_start(const struct sr_dev_inst *sdi)
{
	struct dev_context *devc;
	GHashTableIter iter;
	void *value;

	if (sdi->status != SR_ST_ACTIVE)
		return SR_ERR_DEV_CLOSED;

	devc = sdi->priv;
	devc->sent_samples = 0;

	g_hash_table_iter_init(&iter, devc->ch_ag);
	while (g_hash_table_iter_next(&iter, NULL, &value))
		generate_analog_pattern(value, devc->cur_samplerate);

	sr_session_source_add(sdi->session, -1, 0, 100,
			prepare_data, (struct sr_dev_inst *)sdi);

	std_session_send_df_header(sdi, LOG_PREFIX);

	/* We use this timestamp to decide how many more samples to send. */
	devc->start_us = g_get_monotonic_time();
	devc->spent_us = 0;

	return SR_OK;
}

static int dev_acquisition_stop(struct sr_dev_inst *sdi)
{
	sr_dbg("Stopping acquisition.");
	sr_session_source_remove(sdi->session, -1);
	std_session_send_df_end(sdi, LOG_PREFIX);

	return SR_OK;
}

SR_PRIV struct sr_dev_driver demo_driver_info = {
	.name = "demo",
	.longname = "Demo driver and pattern generator",
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
