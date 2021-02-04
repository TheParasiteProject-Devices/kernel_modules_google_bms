/*
 * Fuel gauge driver for Maxim 17201/17205
 *
 * Copyright (C) 2018 Google Inc.
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
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": %s " fmt, __func__

#include <linux/err.h>
#include <linux/i2c.h>
#include <linux/iio/consumer.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_gpio.h>
#include <linux/pm_runtime.h>
#include <linux/regmap.h>
#include <linux/slab.h>
#include <linux/time.h>

#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/fs.h> /* register_chrdev, unregister_chrdev */
#include <linux/seq_file.h> /* seq_read, seq_lseek, single_release */
#include "gbms_power_supply.h"
#include "google_bms.h"
#include <misc/logbuffer.h>
#include "max1720x_battery.h"

#include <linux/debugfs.h>

#define MAX17X0X_TPOR_MS 150

#define MAX1720X_TRECALL_MS 5
#define MAX1730X_TRECALL_MS 5
#define MAX1720X_TICLR_MS 500
#define MAX1720X_I2C_DRIVER_NAME "max_fg_irq"
#define MAX1720X_DELAY_INIT_MS 1000
#define FULLCAPNOM_STABILIZE_CYCLES 5
#define CYCLE_BUCKET_SIZE 200
#define TEMP_BUCKET_SIZE 5		/* unit is 0.1 degree C */
#define NB_CYCLE_BUCKETS 4

/* capacity drift */
#define BATTERY_DEFAULT_CYCLE_STABLE	0
#define BATTERY_DEFAULT_CYCLE_FADE	0
#define BATTERY_DEFAULT_CYCLE_BAND	10
#define BATTERY_MAX_CYCLE_BAND		20

#define HISTORY_DEVICENAME "maxfg_history"

#include "max1720x.h"
#include "max1730x.h"
#include "max_m5.h"

enum max17xxx_register {
	MAX17XXX_COMMAND	= MAX1720X_COMMAND,
};

enum max17xxx_nvram {
	MAX17XXX_QHCA = MAX1720X_NUSER18C,
	MAX17XXX_QHQH = MAX1720X_NUSER18D,
};

enum max17xxx_command_bits {
	MAX17XXX_COMMAND_NV_RECALL	  = 0xE001,
};

/* Capacity Estimation */
struct gbatt_capacity_estimation {
	const struct max17x0x_reg *bcea;
	struct mutex batt_ce_lock;
	struct delayed_work settle_timer;
	int cap_tsettle;
	int cap_filt_length;
	int estimate_state;
	bool cable_in;
	int delta_cc_sum;
	int delta_vfsoc_sum;
	int cap_filter_count;
	int start_cc;
	int start_vfsoc;
};

#define DEFAULT_BATTERY_ID		-1
#define DEFAULT_BATTERY_ID_RETRIES	5

#define DEFAULT_CAP_SETTLE_INTERVAL	3
#define DEFAULT_CAP_FILTER_LENGTH	12

#define ESTIMATE_DONE		2
#define ESTIMATE_PENDING	1
#define ESTIMATE_NONE		0

#define CE_CAP_FILTER_COUNT	0
#define CE_DELTA_CC_SUM_REG	1
#define CE_DELTA_VFSOC_SUM_REG	2

#define CE_FILTER_COUNT_MAX	15


struct max1720x_history {
	int page_size;

	loff_t history_index;
	int history_count;
	bool *page_status;
	u16 *history;
};

struct max1720x_chip {
	struct device *dev;
	bool irq_shared;
	struct i2c_client *primary;
	struct i2c_client *secondary;

	int gauge_type;	/* -1 not present, 0=max1720x, 1=max1730x */
	struct max17x0x_regmap regmap;
	struct max17x0x_regmap regmap_nvram;

	struct power_supply *psy;
	struct delayed_work init_work;
	struct device_node *batt_node;

	u16 devname;
	struct max17x0x_cache_data nRAM_por;
	bool needs_reset;
	int (*fixups_fn)(struct max1720x_chip *chip);

	/* config */
	void *model_data;
	struct mutex model_lock;
	struct delayed_work model_work;
	int model_next_update;
	/* also used to restore model state from permanent storage */
	u16 reg_prop_capacity_raw;
	bool model_state_valid;	/* state read from persistent */
	int model_reload;
	bool model_ok;		/* model is running */

	/* history */
	struct mutex history_lock;
	int hcmajor;
	struct cdev hcdev;
	struct class *hcclass;
	bool history_available;
	bool history_added;
	int history_page_size;
	int nb_history_pages;
	int nb_history_flag_reg;

	/* for storage interface */
	struct max1720x_history history_storage;

	u16 RSense;
	u16 RConfig;

	int batt_id;
	int batt_id_defer_cnt;
	int cycle_count;

	bool init_complete;
	bool resume_complete;
	u16 health_status;
	int fake_capacity;
	int previous_qh;
	int current_capacity;
	int prev_charge_status;
	char serial_number[30];
	bool offmode_charger;
	s32 convgcfg_hysteresis;
	int nb_convgcfg;
	int curr_convgcfg_idx;
	s16 *temp_convgcfg;
	u16 *convgcfg_values;
	struct mutex convgcfg_lock;
	bool shadow_override;
	int nb_empty_voltage;
	u16 *empty_voltage;

	unsigned int debug_irq_none_cnt;
	unsigned long icnt;
	int zero_irq;

	/* fix capacity drift */
	struct max1720x_drift_data drift_data;
	int comp_update_count;
	int dxacc_update_count;

	/* Capacity Estimation */
	struct gbatt_capacity_estimation cap_estimate;
	struct logbuffer *ce_log;

	/* debug interface, register to read or write */
	u32 debug_reg_address;

	struct power_supply_desc max1720x_psy_desc;
};

#define MAX1720_EMPTY_VOLTAGE(profile, temp, cycle) \
	profile->empty_voltage[temp * NB_CYCLE_BUCKETS + cycle]


static bool max17x0x_reglog_init(struct max1720x_chip *chip)
{
	chip->regmap.reglog =
		devm_kzalloc(chip->dev, sizeof(*chip->regmap.reglog),
			     GFP_KERNEL);
	chip->regmap_nvram.reglog =
		devm_kzalloc(chip->dev, sizeof(*chip->regmap.reglog),
			     GFP_KERNEL);

	return chip->regmap.reglog && chip->regmap_nvram.reglog;
}

/* ------------------------------------------------------------------------- */


/* TODO: split between NV and Volatile? */


static const struct max17x0x_reg *
max17x0x_find_by_index(struct max17x0x_regtags *tags, int index)
{
	if (index < 0 || !tags || index >= tags->max)
		return NULL;

	return &tags->map[index];
}

static const struct max17x0x_reg *
max17x0x_find_by_tag(struct max17x0x_regmap *map, enum max17x0x_reg_tags tag)
{
	return max17x0x_find_by_index(&map->regtags, tag);
}

static inline int max17x0x_reg_read(struct max17x0x_regmap *map,
				    enum max17x0x_reg_tags tag,
				    u16 *val)
{
	const struct max17x0x_reg *reg;
	unsigned int tmp;
	int rtn;

	reg = max17x0x_find_by_tag(map, tag);
	if (!reg)
		return -EINVAL;

	rtn = regmap_read(map->regmap, reg->reg, &tmp);
	if (rtn)
		pr_err("Failed to read %x\n", reg->reg);
	else
		*val = tmp;

	return rtn;
}

/* ------------------------------------------------------------------------- */

/*
 * offset of the register in this atom.
 * NOTE: this is the byte offset regardless of the size of the register
 */
static int max17x0x_reg_offset_of(const struct max17x0x_reg *a,
				  unsigned int reg)
{
	int i;

	switch (a->type) {
	case GBMS_ATOM_TYPE_REG:
		return (reg == a->reg) ? 0 : -EINVAL;
	case GBMS_ATOM_TYPE_ZONE:
		if (reg >= a->base && reg < a->base + a->size)
			return (reg - a->base) * 2;
		break;
	case GBMS_ATOM_TYPE_MAP:
		for (i = 0 ; i < a->size ; i++)
			if (a->map[i] == reg)
				return i * 2;
		break;
	}

	return -ERANGE;
}

static int max17x0x_reg_store_sz(struct max17x0x_regmap *map,
				 const struct max17x0x_reg *a,
				 const void *data,
				 int size)
{
	int i, ret;

	if (size > a->size)
		size = a->size;

	if (a->type == GBMS_ATOM_TYPE_MAP) {
		const u16 *b = (u16 *)data;

		if (size % 2)
			return -ERANGE;

		for (i = 0; i < size / 2 ; i++) {
			ret = regmap_write(map->regmap, a->map[i], b[i]);
			if (ret < 0)
				break;

			max17x0x_reglog_log(map->reglog, a->map[i], b[i], ret);
		}
	} else if (a->type == GBMS_ATOM_TYPE_SET) {
		ret = -EINVAL;
	} else {
		ret = regmap_raw_write(map->regmap, a->base, data, size);

		if (map->reglog) {
			const u16 *b = (u16 *)data;

			for (i = 0; i < size ; i += 2)
				max17x0x_reglog_log(map->reglog, a->base + i,
						    b[i], ret);
		}
	}

	return ret;
}

static int max17x0x_reg_load_sz(struct max17x0x_regmap *map,
				const struct max17x0x_reg *a,
				void *data,
				int size)
{
	int ret;

	if (size > a->size)
		size = a->size;

	if (a->type == GBMS_ATOM_TYPE_MAP) {
		int i;
		unsigned int tmp;
		u16 *b = (u16 *)data;

		if (size % 2)
			return -ERANGE;

		for (i = 0; i < size / 2 ; i++) {
			ret = regmap_read(map->regmap,
					  (unsigned int)a->map[i],
					  &tmp);
			if (ret < 0)
				break;
			b[i] = tmp;
		}
	} else if (a->type == GBMS_ATOM_TYPE_SET) {
		ret = -EINVAL;
	} else {
		ret = regmap_raw_read(map->regmap, a->base, data, size);
	}

	return ret;
}

#define max17x0x_reg_store(map, a, data) \
	max17x0x_reg_store_sz(map, a, data, (a)->size)

#define max17x0x_reg_load(map, a, data) \
	max17x0x_reg_load_sz(map, a, data, (a)->size)


static u16 *batt_alloc_array(int count, int size)
{
	return (u16 *)kmalloc_array(count, size, GFP_KERNEL);
}

/* CACHE ----------------------------------------------------------------- */

static int max17x0x_cache_index_of(const struct max17x0x_cache_data *cache,
				   unsigned int reg)
{
	const int offset = max17x0x_reg_offset_of(&cache->atom, reg);

	return (offset < 0) ? offset : offset / 2;
}

#define max17x0x_cache_store(cache, regmap) \
	max17x0x_reg_store(regmap, &(cache)->atom, (cache)->cache_data)

#define max17x0x_cache_load(cache, regmap) \
	max17x0x_reg_load(regmap, &(cache)->atom, (cache)->cache_data)

#define max17x0x_cache_memcmp(src, dst) \
	memcmp((src)->cache_data, (dst)->cache_data, (src)->atom.size)

static void max17x0x_cache_free(struct max17x0x_cache_data *cache)
{
	kfree(cache->cache_data);
	cache->cache_data = NULL;
}

static int max17x0x_cache_dup(struct max17x0x_cache_data *dst,
			      const struct max17x0x_cache_data *src)
{
	memcpy(dst, src, sizeof(*dst));

	dst->cache_data = (u16 *)kmalloc(src->atom.size, GFP_KERNEL);
	if (!dst->cache_data)
		return -ENOMEM;

	memcpy(dst->cache_data, src->cache_data, src->atom.size);
	return 0;
}

static int max17x0x_cache_init(struct max17x0x_cache_data *cache,
				     u16 start, int end)
{
	const int count = end - start + 1; /* includes end */

	memset(cache, 0, sizeof(*cache));

	cache->cache_data = batt_alloc_array(count, sizeof(u16));
	if (!cache->cache_data)
		return -ENOMEM;

	cache->atom.type = GBMS_ATOM_TYPE_ZONE;
	cache->atom.size = count * sizeof(u16);
	cache->atom.base = start;

	return 0;
}

static int max17x0x_nvram_cache_init(struct max17x0x_cache_data *cache,
				     int gauge_type)
{
	int ret;

	if (gauge_type == MAX1730X_GAUGE_TYPE) {
		ret = max17x0x_cache_init(cache,
					  MAX1730X_NVRAM_START,
					  MAX1730X_NVRAM_END);
	} else {
		ret = max17x0x_cache_init(cache,
					  MAX1720X_NVRAM_START,
					  MAX1720X_NVRAM_END);
	}

	return ret;
}

/* ------------------------------------------------------------------------- */

static inline int reg_to_percentage(u16 val)
{
	/* LSB: 1/256% */
	return val >> 8;
}

static inline int reg_to_twos_comp_int(u16 val)
{
	/* Convert u16 to twos complement  */
	return -(val & 0x8000) + (val & 0x7FFF);
}

static inline int reg_to_micro_amp(s16 val, u16 rsense)
{
	/* LSB: 1.5625μV/RSENSE ; Rsense LSB is 10μΩ */
	return div_s64((s64) val * 156250, rsense);
}

static inline int reg_to_deci_deg_cel(s16 val)
{
	/* LSB: 1/256°C */
	return div_s64((s64) val * 10, 256);
}

static inline int reg_to_resistance_micro_ohms(s16 val, u16 rsense)
{
	/* LSB: 1/4096 Ohm */
	return div_s64((s64) val * 1000 * rsense, 4096);
}

static inline int reg_to_cycles(s16 val)
{
	/* LSB: 16% of one cycle */
	return DIV_ROUND_CLOSEST((int) val * 16, 100);
}

static inline int reg_to_seconds(s16 val)
{
	/* LSB: 5.625 seconds */
	return DIV_ROUND_CLOSEST((int) val * 5625, 1000);
}

static inline int reg_to_vempty(u16 val)
{
	return ((val >> 7) & 0x1FF) * 10;
}

static inline int reg_to_vrecovery(u16 val)
{
	return (val & 0x7F) * 40;
}

/* b/177099997 TaskPeriod ----------------------------------------------- */

static inline int reg_to_capacity_uah(u16 val, struct max1720x_chip *chip)
{
	const int lsb = max_m5_cap_lsb(chip->model_data);

	return reg_to_micro_amp_h(val, chip->RSense, lsb);
}

#if 0
/* TODO: will need in outliers */
static inline int capacity_uah_to_reg(int capacity, struct max1720x_chip *chip)
{
	const int lsb = max_m5_cap_lsb(chip->model_data);

	return micro_amp_h_to_reg(capacity / lsb, chip->RSense);
}
#endif

/* log ----------------------------------------------------------------- */

static void max1730x_read_log_write_status(struct max1720x_chip *chip,
					   u16 *buffer)
{
	u16 i;
	u16 data = 0;
	const struct max17x0x_reg *hsty;

	hsty = max17x0x_find_by_tag(&chip->regmap_nvram, MAX17X0X_TAG_HSTY);
	if (!hsty)
		return;

	REGMAP_WRITE(&chip->regmap, MAX17XXX_COMMAND,
		     MAX1730X_COMMAND_HISTORY_RECALL_WRITE_0);
	msleep(MAX1730X_TRECALL_MS);
	for (i = hsty->map[1]; i <= hsty->map[3]; i++) {
		(void)REGMAP_READ(&chip->regmap_nvram, i, &data);
		*buffer++ = data;
	}
}

static void max1730x_read_log_valid_status(struct max1720x_chip *chip,
					   u16 *buffer)
{
	u16 i;
	u16 data = 0;
	const struct max17x0x_reg *hsty;

	hsty = max17x0x_find_by_tag(&chip->regmap_nvram, MAX17X0X_TAG_HSTY);

	if (!hsty)
		return;

	REGMAP_WRITE(&chip->regmap, MAX17XXX_COMMAND,
		     MAX1730X_COMMAND_HISTORY_RECALL_VALID_0);
	msleep(MAX1730X_TRECALL_MS);
	(void)REGMAP_READ(&chip->regmap_nvram, hsty->map[4], &data);
	*buffer++ = data;

	REGMAP_WRITE(&chip->regmap, MAX17XXX_COMMAND,
		     MAX1730X_COMMAND_HISTORY_RECALL_VALID_1);
	msleep(MAX1730X_TRECALL_MS);
	for (i = hsty->map[0]; i <= hsty->map[2]; i++) {
		(void)REGMAP_READ(&chip->regmap_nvram, i, &data);
		*buffer++ = data;
	}
}

static void max1720x_read_log_write_status(struct max1720x_chip *chip,
					   u16 *buffer)
{
	int i;
	u16 data = 0;

	REGMAP_WRITE(&chip->regmap, MAX17XXX_COMMAND,
		     MAX1720X_COMMAND_HISTORY_RECALL_WRITE_0);
	msleep(MAX1720X_TRECALL_MS);
	for (i = MAX1720X_NVRAM_HISTORY_WRITE_STATUS_START;
	     i <= MAX1720X_NVRAM_HISTORY_END; i++) {
		(void)REGMAP_READ(&chip->regmap_nvram, i, &data);
		*buffer++ = data;
	}
	REGMAP_WRITE(&chip->regmap, MAX17XXX_COMMAND,
		     MAX1720X_COMMAND_HISTORY_RECALL_WRITE_1);
	msleep(MAX1720X_TRECALL_MS);
	for (i = MAX1720X_HISTORY_START;
	     i <= MAX1720X_NVRAM_HISTORY_WRITE_STATUS_END; i++) {
		(void)REGMAP_READ(&chip->regmap_nvram, i, &data);
		*buffer++ = data;
	}
}

static void max1720x_read_log_valid_status(struct max1720x_chip *chip,
					   u16 *buffer)
{
	int i;
	u16 data = 0;

	REGMAP_WRITE(&chip->regmap, MAX17XXX_COMMAND,
		     MAX1720X_COMMAND_HISTORY_RECALL_VALID_0);
	msleep(MAX1720X_TRECALL_MS);
	for (i = MAX1720X_NVRAM_HISTORY_VALID_STATUS_START;
	     i <= MAX1720X_NVRAM_HISTORY_END; i++) {
		(void)REGMAP_READ(&chip->regmap_nvram, i, &data);
		*buffer++ = data;
	}
	REGMAP_WRITE(&chip->regmap, MAX17XXX_COMMAND,
		     MAX1720X_COMMAND_HISTORY_RECALL_VALID_1);
	msleep(MAX1720X_TRECALL_MS);
	for (i = MAX1720X_HISTORY_START;
	     i <= MAX1720X_NVRAM_HISTORY_END; i++) {
		(void)REGMAP_READ(&chip->regmap_nvram, i, &data);
		*buffer++ = data;
	}
	REGMAP_WRITE(&chip->regmap, MAX17XXX_COMMAND,
		     MAX1720X_COMMAND_HISTORY_RECALL_VALID_2);
	msleep(MAX1720X_TRECALL_MS);
	for (i = MAX1720X_HISTORY_START;
	     i <= MAX1720X_NVRAM_HISTORY_VALID_STATUS_END; i++) {
		(void)REGMAP_READ(&chip->regmap_nvram, i, &data);
		*buffer++ = data;
	}
}

/* @return the number of pages or negative for error */
static int get_battery_history_status(struct max1720x_chip *chip,
				      bool *page_status)
{
	u16 *write_status, *valid_status;
	int i, addr_offset, bit_offset, nb_history_pages;
	int valid_history_entry_count = 0;

	write_status = batt_alloc_array(chip->nb_history_flag_reg, sizeof(u16));
	if (!write_status)
		return -ENOMEM;

	valid_status = batt_alloc_array(chip->nb_history_flag_reg, sizeof(u16));
	if (!valid_status) {
		kfree(write_status);
		return -ENOMEM;
	}

	if (chip->gauge_type == MAX1730X_GAUGE_TYPE) {
		max1730x_read_log_write_status(chip, write_status);
		max1730x_read_log_valid_status(chip, valid_status);
		nb_history_pages = MAX1730X_N_OF_HISTORY_PAGES;
	} else {
		max1720x_read_log_write_status(chip, write_status);
		max1720x_read_log_valid_status(chip, valid_status);
		nb_history_pages = MAX1720X_N_OF_HISTORY_PAGES;
	}

	/* Figure out the pages with valid history entry */
	for (i = 0; i < nb_history_pages; i++) {
		addr_offset = i / 8;
		bit_offset = i % 8;
		page_status[i] =
		    ((write_status[addr_offset] & BIT(bit_offset)) ||
		     (write_status[addr_offset] & BIT(bit_offset + 8))) &&
		    ((valid_status[addr_offset] & BIT(bit_offset)) ||
		     (valid_status[addr_offset] & BIT(bit_offset + 8)));
		if (page_status[i])
			valid_history_entry_count++;
	}

	kfree(write_status);
	kfree(valid_status);

	return valid_history_entry_count;
}

static void get_battery_history(struct max1720x_chip *chip,
				bool *page_status, u16 *history)
{
	int i, j, index = 0;
	u16 data = 0;
	const struct max17x0x_reg *hsty;
	u16 command_base = (chip->gauge_type == MAX1730X_GAUGE_TYPE)
		? MAX1730X_READ_HISTORY_CMD_BASE
		: MAX1720X_READ_HISTORY_CMD_BASE;

	hsty = max17x0x_find_by_tag(&chip->regmap_nvram, MAX17X0X_TAG_HSTY);
	if (!hsty)
		return;

	for (i = 0; i < chip->nb_history_pages; i++) {
		if (!page_status[i])
			continue;
		REGMAP_WRITE(&chip->regmap, MAX17XXX_COMMAND,
			     command_base + i);
		msleep(MAX1720X_TRECALL_MS);
		for (j = 0; j < chip->history_page_size; j++) {
			(void)REGMAP_READ(&chip->regmap_nvram,
					  (unsigned int)hsty->map[0] + j,
					  &data);
			history[index * chip->history_page_size + j] = data;
		}
		index++;
	}
}

static int format_battery_history_entry(char *temp, int size,
					int page_size, u16 *line)
{
	int length = 0, i;

	for (i = 0; i < page_size; i++) {
		length += scnprintf(temp + length,
			size - length, "%04x ",
			line[i]);
	}

	if (length > 0)
		temp[--length] = 0;
	return length;
}

/* @return number of valid entries */
static int max1720x_history_read(struct max1720x_chip *chip,
				 struct max1720x_history *hi)
{
	memset(hi, 0, sizeof(*hi));

	hi->page_status = kcalloc(chip->nb_history_pages,
				sizeof(bool), GFP_KERNEL);
	if (!hi->page_status)
		return -ENOMEM;


	hi->history_count = get_battery_history_status(chip, hi->page_status);
	if (hi->history_count < 0) {
		goto error_exit;
	} else if (hi->history_count != 0) {
		const int size = hi->history_count * chip->history_page_size;

		hi->history = batt_alloc_array(size, sizeof(u16));
		if (!hi->history) {
			hi->history_count = -ENOMEM;
			goto error_exit;
		}

		get_battery_history(chip, hi->page_status, hi->history);
	}

	return hi->history_count;

error_exit:
	kfree(hi->page_status);
	hi->page_status = NULL;
	return hi->history_count;

}

static void max1720x_history_free(struct max1720x_history *hi)
{
	kfree(hi->page_status);
	kfree(hi->history);

	hi->history = NULL;
	hi->page_status = NULL;
	hi->history_count = -1;
	hi->history_index = 0;
}


/*
 * Removed the following properties:
 *   POWER_SUPPLY_PROP_TIME_TO_EMPTY_AVG
 *   POWER_SUPPLY_PROP_TIME_TO_FULL_AVG
 *   POWER_SUPPLY_PROP_VOLTAGE_MAX_DESIGN,
 *   POWER_SUPPLY_PROP_VOLTAGE_MIN_DESIGN,
 * Need to keep the number of properies under UEVENT_NUM_ENVP (minus # of
 * standard uevent variables).
 */
static enum power_supply_property max1720x_battery_props[] = {
	POWER_SUPPLY_PROP_STATUS,
	POWER_SUPPLY_PROP_HEALTH,
	POWER_SUPPLY_PROP_CAPACITY,		/* replace with _RAW */
	POWER_SUPPLY_PROP_CHARGE_COUNTER,
	POWER_SUPPLY_PROP_CHARGE_FULL,
	POWER_SUPPLY_PROP_CHARGE_FULL_DESIGN,	/* used from gbattery */
	POWER_SUPPLY_PROP_CURRENT_AVG,		/* candidate for tier switch */
	POWER_SUPPLY_PROP_CURRENT_NOW,
	POWER_SUPPLY_PROP_CYCLE_COUNT,
	POWER_SUPPLY_PROP_PRESENT,
	POWER_SUPPLY_PROP_TEMP,
	POWER_SUPPLY_PROP_VOLTAGE_AVG,
	POWER_SUPPLY_PROP_VOLTAGE_NOW,
	POWER_SUPPLY_PROP_VOLTAGE_OCV,
	POWER_SUPPLY_PROP_TECHNOLOGY,
	POWER_SUPPLY_PROP_SERIAL_NUMBER,
};

/* ------------------------------------------------------------------------- */

static ssize_t max1720x_get_offmode_charger(struct device *dev,
					    struct device_attribute *attr,
					    char *buf)
{
	struct power_supply *psy = container_of(dev, struct power_supply, dev);
	struct max1720x_chip *chip = power_supply_get_drvdata(psy);

	return scnprintf(buf, PAGE_SIZE, "%hhd\n", chip->offmode_charger);
}

static ssize_t max1720x_set_offmode_charger(struct device *dev,
					    struct device_attribute *attr,
					    const char *buf, size_t count)
{
	struct power_supply *psy = container_of(dev, struct power_supply, dev);
	struct max1720x_chip *chip = power_supply_get_drvdata(psy);

	if (kstrtobool(buf, &chip->offmode_charger))
		return -EINVAL;

	return count;
}

static DEVICE_ATTR(offmode_charger, 0660,
		   max1720x_get_offmode_charger,
		   max1720x_set_offmode_charger);


static ssize_t max1720x_model_show_state(struct device *dev,
					 struct device_attribute *attr,
					 char *buf)
{
	struct power_supply *psy = container_of(dev, struct power_supply, dev);
	struct max1720x_chip *chip = power_supply_get_drvdata(psy);
	ssize_t len = 0;

	if (!chip->model_data)
		return -EINVAL;

	mutex_lock(&chip->model_lock);
	len += scnprintf(&buf[len], PAGE_SIZE, "ModelNextUpdate: %d\n",
			 chip->model_next_update);
	len += max_m5_model_state_cstr(&buf[len], PAGE_SIZE - len,
				       chip->model_data);
	mutex_unlock(&chip->model_lock);

	return len;
}

/*
 * force is true when changing the model via debug props.
 * NOTE: call holding model_lock
 */
static void max1720x_model_reload(struct max1720x_chip *chip, bool force)
{
	const bool disabled = chip->model_reload == MAX_M5_LOAD_MODEL_DISABLED;
	const bool pending = chip->model_reload != MAX_M5_LOAD_MODEL_IDLE;

	pr_debug("model_reload=%d force=%d pending=%d disabled=%d\n",
		 chip->model_reload, force, pending, disabled);

	if (!force && (pending || disabled))
		return;

	/* REQUEST -> IDLE or set to the number of retries */
	dev_info(chip->dev, "Schedule Load FG Model, ID=%d, ver:%d->%d cap_lsb:%d->%d\n",
			chip->batt_id,
			max_m5_model_read_version(chip->model_data),
			max_m5_fg_model_version(chip->model_data),
			max_m5_model_get_cap_lsb(chip->model_data),
			max_m5_cap_lsb(chip->model_data));

	chip->model_reload = MAX_M5_LOAD_MODEL_REQUEST;
	chip->model_ok = false;
	mod_delayed_work(system_wq, &chip->model_work, 0);
}

static ssize_t max1720x_model_set_state(struct device *dev,
					struct device_attribute *attr,
					const char *buf, size_t count)
{
	struct power_supply *psy = container_of(dev, struct power_supply, dev);
	struct max1720x_chip *chip = power_supply_get_drvdata(psy);
	int ret;

	if (!chip->model_data)
		return -EINVAL;

	mutex_lock(&chip->model_lock);

	/* read current state from gauge */
	ret = max_m5_model_read_state(chip->model_data);
	if (ret < 0) {
		mutex_unlock(&chip->model_lock);
		return ret;
	}

	/* overwrite with userland, will commit at cycle count */
	ret = max_m5_model_state_sscan(chip->model_data, buf, count);
	if (ret == 0) {
		/* force model state (valid) */
		chip->model_state_valid = true;
		max1720x_model_reload(chip, true);
	}

	mutex_unlock(&chip->model_lock);
	return count;
}

static DEVICE_ATTR(m5_model_state, 0640, max1720x_model_show_state,
		   max1720x_model_set_state);


/* Was POWER_SUPPLY_PROP_RESISTANCE_ID */
static ssize_t resistance_id_show(struct device *dev,
				 struct device_attribute *attr,
				 char *buff)
{
	struct power_supply *psy = container_of(dev, struct power_supply, dev);
	struct max1720x_chip *chip = power_supply_get_drvdata(psy);

	return scnprintf(buff, PAGE_SIZE, "%d\n", chip->batt_id);
}

static const DEVICE_ATTR_RO(resistance_id);

/* Was POWER_SUPPLY_PROP_RESISTANCE */
static ssize_t resistance_show(struct device *dev,
				 struct device_attribute *attr,
				 char *buff)
{
	struct power_supply *psy = container_of(dev, struct power_supply, dev);
	struct max1720x_chip *chip = power_supply_get_drvdata(psy);
	struct max17x0x_regmap *map = &chip->regmap;
	u16 data = 0;
	int err;

	err = REGMAP_READ(map, MAX1720X_RCELL, &data);
	if (err < 0)
		return err;

	return scnprintf(buff, PAGE_SIZE, "%d\n",
			 reg_to_resistance_micro_ohms(data, chip->RSense));
}

static const DEVICE_ATTR_RO(resistance);


/* lsb 1/256, race with max1720x_model_work()  */
static int max1720x_get_capacity_raw(struct max1720x_chip *chip, u16 *data)
{
	return REGMAP_READ(&chip->regmap, chip->reg_prop_capacity_raw, data);
}

static int max1720x_get_battery_soc(struct max1720x_chip *chip)
{
	u16 data;
	int capacity, err;

	if (chip->fake_capacity >= 0 && chip->fake_capacity <= 100)
		return chip->fake_capacity;

	err = REGMAP_READ(&chip->regmap, MAX1720X_REPSOC, &data);
	if (err)
		return err;
	capacity = reg_to_percentage(data);

	if (capacity == 100 && chip->offmode_charger)
		chip->fake_capacity = 100;

	return capacity;
}

static int max1720x_get_battery_vfsoc(struct max1720x_chip *chip)
{
	u16 data;
	int capacity, err;


	err = max17x0x_reg_read(&chip->regmap, MAX17X0X_TAG_vfsoc, &data);
	if (err)
		return err;
	capacity = reg_to_percentage(data);

	return capacity;
}

/* TODO: factor with the one in google_bms.c */
static char *psy_status_str[] = {
	"Unknown", "Charging", "Discharging", "NotCharging", "Full"
};

static void max1720x_prime_battery_qh_capacity(struct max1720x_chip *chip,
					       int status)
{
	u16  mcap = 0, data = 0;

	(void)max17x0x_reg_read(&chip->regmap, MAX17X0X_TAG_mcap, &mcap);
	chip->current_capacity = mcap;

	(void)REGMAP_READ(&chip->regmap, MAX1720X_QH, &data);
	chip->previous_qh = reg_to_twos_comp_int(data);

	if (chip->regmap_nvram.regmap) {
		REGMAP_WRITE(&chip->regmap_nvram, MAX17XXX_QHCA, ~mcap);
		dev_info(chip->dev, "Capacity primed to %d on %s\n",
			mcap, psy_status_str[status]);

		REGMAP_WRITE(&chip->regmap_nvram, MAX17XXX_QHQH, data);
		dev_info(chip->dev, "QH primed to %d on %s\n",
			data, psy_status_str[status]);
	}
}

/* NOTE: the gauge doesn't know if we are current limited to */
static int max1720x_get_battery_status(struct max1720x_chip *chip)
{
	u16 data = 0;
	int current_now, current_avg, ichgterm, vfsoc, soc, fullsocthr;
	int status = POWER_SUPPLY_STATUS_UNKNOWN, err;

	err = max17x0x_reg_read(&chip->regmap, MAX17X0X_TAG_curr, &data);
	if (err)
		return -EIO;
	current_now = -reg_to_micro_amp(data, chip->RSense);

	err = max17x0x_reg_read(&chip->regmap, MAX17X0X_TAG_avgc, &data);
	if (err)
		return -EIO;
	current_avg = -reg_to_micro_amp(data, chip->RSense);

	err = REGMAP_READ(&chip->regmap, MAX1720X_ICHGTERM, &data);
	if (err)
		return -EIO;
	ichgterm = reg_to_micro_amp(data, chip->RSense);

	err = REGMAP_READ(&chip->regmap, MAX1720X_FULLSOCTHR, &data);
	if (err)
		return -EIO;
	fullsocthr = reg_to_percentage(data);

	soc = max1720x_get_battery_soc(chip);
	if (soc < 0)
		return -EIO;

	vfsoc = max1720x_get_battery_vfsoc(chip);
	if (vfsoc < 0)
		return -EIO;

	if (current_avg > -ichgterm && current_avg <= 0) {

		if (soc >= fullsocthr) {
			const bool needs_prime = (chip->prev_charge_status ==
						  POWER_SUPPLY_STATUS_CHARGING);

			status = POWER_SUPPLY_STATUS_FULL;
			if (needs_prime)
				max1720x_prime_battery_qh_capacity(chip,
								   status);
		} else {
			status = POWER_SUPPLY_STATUS_NOT_CHARGING;
		}

	} else if (current_now >= -ichgterm)  {
		status = POWER_SUPPLY_STATUS_DISCHARGING;
	} else {
		status = POWER_SUPPLY_STATUS_CHARGING;
		if (chip->prev_charge_status == POWER_SUPPLY_STATUS_DISCHARGING
		    && current_avg  < -ichgterm)
			max1720x_prime_battery_qh_capacity(chip, status);
	}

	if (status != chip->prev_charge_status)
		dev_info(chip->dev, "s=%d->%d c=%d avg_c=%d ichgt=%d vfsoc=%d soc=%d fullsocthr=%d\n",
				    chip->prev_charge_status,
				    status, current_now, current_avg,
				    ichgterm, vfsoc, soc, fullsocthr);

	chip->prev_charge_status = status;

	return status;
}

static int max1720x_get_battery_health(struct max1720x_chip *chip)
{
	/* For health report what ever was recently alerted and clear it */

	if (chip->health_status & MAX1720X_STATUS_VMX) {
		chip->health_status &= ~MAX1720X_STATUS_VMX;
		return POWER_SUPPLY_HEALTH_OVERVOLTAGE;
	}

	if (chip->health_status & MAX1720X_STATUS_TMN) {
		chip->health_status &= ~MAX1720X_STATUS_TMN;
		return POWER_SUPPLY_HEALTH_COLD;
	}

	if (chip->health_status & MAX1720X_STATUS_TMX) {
		chip->health_status &= ~MAX1720X_STATUS_TMX;
		return POWER_SUPPLY_HEALTH_OVERHEAT;
	}

	return POWER_SUPPLY_HEALTH_GOOD;
}

static int max1720x_update_battery_qh_based_capacity(struct max1720x_chip *chip)
{
	u16 data;
	int current_qh, err = 0;

	err = REGMAP_READ(&chip->regmap, MAX1720X_QH, &data);
	if (err)
		return err;

	current_qh = reg_to_twos_comp_int(data);

	/* QH value accumulates as battery charges */
	chip->current_capacity -= (chip->previous_qh - current_qh);
	chip->previous_qh = current_qh;

	return 0;
}

static void max1720x_restore_battery_qh_capacity(struct max1720x_chip *chip)
{
	int ret;
	int current_qh, nvram_qh;
	u16 data = 0, nvram_capacity;

	if (!chip->regmap_nvram.regmap) {
		max1720x_prime_battery_qh_capacity(chip,
						   POWER_SUPPLY_STATUS_UNKNOWN);
		return;
	}


	/* Capacity data is stored as complement so it will not be zero. Using
	 * zero case to detect new un-primed pack
	 */
	ret = REGMAP_READ(&chip->regmap_nvram, MAX17XXX_QHCA, &data);
	if (!ret && data == 0) {
		max1720x_prime_battery_qh_capacity(chip,
						   POWER_SUPPLY_STATUS_UNKNOWN);
		return;
	}

	nvram_capacity = ~data;

	ret = REGMAP_READ(&chip->regmap_nvram, MAX17XXX_QHQH, &data);
	if (ret) {
		max1720x_prime_battery_qh_capacity(chip,
						   POWER_SUPPLY_STATUS_UNKNOWN);
		return;
	}
	nvram_qh = reg_to_twos_comp_int(data);

	ret = REGMAP_READ(&chip->regmap, MAX1720X_QH, &data);
	if (ret) {
		max1720x_prime_battery_qh_capacity(chip,
						   POWER_SUPPLY_STATUS_UNKNOWN);
		return;
	}
	current_qh = reg_to_twos_comp_int(data);

	/* QH value accumulates as battery discharges */
	chip->current_capacity = (int) nvram_capacity - (nvram_qh - current_qh);
	dev_info(chip->dev, "Capacity restored to %d\n",
		 chip->current_capacity);
	chip->previous_qh = current_qh;
	dev_info(chip->dev, "QH value restored to %d\n",
		 chip->previous_qh);
}

static void max1720x_handle_update_nconvgcfg(struct max1720x_chip *chip,
					     int temp)
{
	int idx = -1, hysteresis_temp;

	if (chip->temp_convgcfg == NULL)
		return;

	if (temp <= chip->temp_convgcfg[0]) {
		idx = 0;
	} else if (temp > chip->temp_convgcfg[chip->nb_convgcfg - 1]) {
		idx = chip->nb_convgcfg - 1;
	} else {
		for (idx = 1 ; idx < chip->nb_convgcfg; idx++) {
			if (temp > chip->temp_convgcfg[idx - 1] &&
			    temp <= chip->temp_convgcfg[idx])
				break;
		}
	}
	mutex_lock(&chip->convgcfg_lock);
	/* We want to switch to higher slot only if above temp + hysteresis
	 * but when temperature drops, we want to change at the level
	 */
	hysteresis_temp = chip->temp_convgcfg[chip->curr_convgcfg_idx] +
			  chip->convgcfg_hysteresis;
	if ((idx != chip->curr_convgcfg_idx) &&
	    (chip->curr_convgcfg_idx == -1 || idx < chip->curr_convgcfg_idx ||
	     temp >= chip->temp_convgcfg[chip->curr_convgcfg_idx] +
	     chip->convgcfg_hysteresis)) {
		REGMAP_WRITE(&chip->regmap_nvram, MAX1720X_NCONVGCFG,
			     chip->convgcfg_values[idx]);
		chip->curr_convgcfg_idx = idx;
		dev_info(chip->dev, "updating nConvgcfg to 0x%04x as temp is %d (idx:%d)\n",
			 chip->convgcfg_values[idx], temp, idx);
	}
	mutex_unlock(&chip->convgcfg_lock);
}

#define MAXIM_CYCLE_COUNT_RESET 655
#define MAX17201_HIST_CYCLE_COUNT_OFFSET	0x4
#define MAX17201_HIST_TIME_OFFSET		0xf

/* WA for cycle count reset.
 * max17201 fuel gauge rolls over the cycle count to 0 and burns
 * an history entry with 0 cycles when the cycle count exceeds
 * 655. This code workaround the issue adding 655 to the cycle
 * count if the fuel gauge history has an entry with 0 cycles and
 * non 0 time-in-field.
 */
static int max1720x_get_cycle_count_offset(const struct max1720x_chip *chip)
{
	int offset = 0;

	/*
	 * uses history on devices that have it (max1720x), use EEPROM
	 * in others. it might be written in terms of storage.
	 */

	return offset;
}

static int max1720x_get_cycle_count(struct max1720x_chip *chip)
{
	int err, cycle_count;
	u16 temp;

	err = REGMAP_READ(&chip->regmap, MAX1720X_CYCLES, &temp);
	if (err < 0)
		return err;

	cycle_count = reg_to_cycles(temp);
	if (chip->cycle_count == -1 || cycle_count < chip->cycle_count)
		cycle_count += max1720x_get_cycle_count_offset(chip);

	chip->cycle_count = cycle_count;
	return cycle_count;
}

static void max1720x_handle_update_empty_voltage(struct max1720x_chip *chip,
						 int temp)
{
	int cycle, cycle_idx, temp_idx, chg_st, ret = 0;
	u16 empty_volt_cfg, reg, vempty = 0;

	if (chip->empty_voltage == NULL)
		return;

	chg_st = max1720x_get_battery_status(chip);
	if (chg_st < 0)
		return;

	cycle = max1720x_get_cycle_count(chip);
	if (cycle < 0)
		return;

	ret = REGMAP_READ(&chip->regmap, MAX1720X_VEMPTY, &vempty);
	if (ret < 0)
		return;

	cycle_idx = cycle / CYCLE_BUCKET_SIZE;
	if (cycle_idx > (NB_CYCLE_BUCKETS - 1))
		cycle_idx = NB_CYCLE_BUCKETS - 1;

	if (temp < 0) {
		temp_idx = 0;
	} else {
		const int idx = temp / TEMP_BUCKET_SIZE + 1;
		const int temp_buckets = chip->nb_empty_voltage /
					 NB_CYCLE_BUCKETS;

		temp_idx = idx < (temp_buckets - 1) ? idx : (temp_buckets - 1);
	}

	empty_volt_cfg = MAX1720_EMPTY_VOLTAGE(chip, temp_idx, cycle_idx);
	reg = (empty_volt_cfg / 10) << 7 | (vempty & 0x7F);
	if ((reg > vempty) ||
	    (reg < vempty && chg_st != POWER_SUPPLY_STATUS_DISCHARGING)) {
		REGMAP_WRITE(&chip->regmap, MAX1720X_VEMPTY, reg);

		pr_debug("updating empty_voltage to %d(0x%04X), temp:%d(%d), cycle:%d(%d)\n",
				empty_volt_cfg, reg,
				temp, temp_idx,
				cycle, cycle_idx);
	}
}

/* Capacity Estimation functions*/
static int batt_ce_regmap_read(struct max17x0x_regmap *map,
			       const struct max17x0x_reg *bcea,
			       u32 reg, u16 *data)
{
	int err;
	u16 val;

	if (!bcea)
		return -EINVAL;

	err = REGMAP_READ(map, bcea->map[reg], &val);
	if (err)
		return err;

	switch(reg) {
	case CE_DELTA_CC_SUM_REG:
	case CE_DELTA_VFSOC_SUM_REG:
		*data = val;
		break;
	case CE_CAP_FILTER_COUNT:
		val = val & 0x0F00;
		*data = val >> 8;
		break;
	default:
		break;
	}

	return err;
}

static int batt_ce_regmap_write(struct max17x0x_regmap *map,
				const struct max17x0x_reg *bcea,
				u32 reg, u16 data)
{
	int err = -EINVAL;
	u16 val;

	if (!bcea)
		return -EINVAL;

	switch(reg) {
	case CE_DELTA_CC_SUM_REG:
	case CE_DELTA_VFSOC_SUM_REG:
		err = REGMAP_WRITE(map, bcea->map[reg], data);
		break;
	case CE_CAP_FILTER_COUNT:
		err = REGMAP_READ(map, bcea->map[reg], &val);
		if (err)
			return err;
		val = val & 0xF0FF;
		if (data > CE_FILTER_COUNT_MAX)
			val = val | 0x0F00;
		else
			val = val | (data << 8);
		err = REGMAP_WRITE(map, bcea->map[reg], val);
		break;
	default:
		break;
	}

	return err;
}

static void batt_ce_dump_data(const struct gbatt_capacity_estimation *cap_esti,
			      struct logbuffer *log)
{
	logbuffer_log(log, "cap_filter_count: %d"
			    " start_cc: %d"
			    " start_vfsoc: %d"
			    " delta_cc_sum: %d"
			    " delta_vfsoc_sum: %d"
			    " state: %d"
			    " cable: %d\n",
			    cap_esti->cap_filter_count,
			    cap_esti->start_cc,
			    cap_esti->start_vfsoc,
			    cap_esti->delta_cc_sum,
			    cap_esti->delta_vfsoc_sum,
			    cap_esti->estimate_state,
			    cap_esti->cable_in);
}

static int batt_ce_load_data(struct max17x0x_regmap *map,
			     struct gbatt_capacity_estimation *cap_esti)
{
	u16 data;
	const struct max17x0x_reg *bcea = cap_esti->bcea;

	cap_esti->estimate_state = ESTIMATE_NONE;
	if (batt_ce_regmap_read(map, bcea, CE_DELTA_CC_SUM_REG, &data) == 0)
		cap_esti->delta_cc_sum = data;
	else
		cap_esti->delta_cc_sum = 0;

	if (batt_ce_regmap_read(map, bcea, CE_DELTA_VFSOC_SUM_REG, &data) == 0)
		cap_esti->delta_vfsoc_sum = data;
	else
		cap_esti->delta_vfsoc_sum = 0;

	if (batt_ce_regmap_read(map, bcea, CE_CAP_FILTER_COUNT, &data) == 0)
		cap_esti->cap_filter_count = data;
	else
		cap_esti->cap_filter_count = 0;
	return 0;
}

/* call holding &cap_esti->batt_ce_lock */
static void batt_ce_store_data(struct max17x0x_regmap *map,
			       struct gbatt_capacity_estimation *cap_esti)
{
	if (cap_esti->cap_filter_count <= CE_FILTER_COUNT_MAX) {
		batt_ce_regmap_write(map, cap_esti->bcea,
					  CE_CAP_FILTER_COUNT,
					  cap_esti->cap_filter_count);
	}

	batt_ce_regmap_write(map, cap_esti->bcea,
				  CE_DELTA_VFSOC_SUM_REG,
				  cap_esti->delta_vfsoc_sum);
	batt_ce_regmap_write(map, cap_esti->bcea,
				  CE_DELTA_CC_SUM_REG,
				  cap_esti->delta_cc_sum);
}

/* call holding &cap_esti->batt_ce_lock */
static void batt_ce_stop_estimation(struct gbatt_capacity_estimation *cap_esti,
				   int reason)
{
	cap_esti->estimate_state = reason;
	cap_esti->start_vfsoc = 0;
	cap_esti->start_cc = 0;
}

static int batt_ce_full_estimate(struct gbatt_capacity_estimation *ce)
{
	return (ce->cap_filter_count > 0) && (ce->delta_vfsoc_sum > 0) ?
		ce->delta_cc_sum / ce->delta_vfsoc_sum : -1;
}

/* Measure the deltaCC, deltaVFSOC and CapacityFiltered */
static void batt_ce_capacityfiltered_work(struct work_struct *work)
{
	struct max1720x_chip *chip = container_of(work, struct max1720x_chip,
					    cap_estimate.settle_timer.work);
	struct gbatt_capacity_estimation *cap_esti = &chip->cap_estimate;
	const int lsb = max_m5_cap_lsb(chip->model_data);
	int settle_cc = 0, settle_vfsoc = 0;
	int delta_cc = 0, delta_vfsoc = 0;
	int cc_sum = 0, vfsoc_sum = 0;
	bool valid_estimate = false;
	int rc = 0;
	u16 data;

	mutex_lock(&cap_esti->batt_ce_lock);

	/* race with disconnect */
	if (!cap_esti->cable_in ||
	    cap_esti->estimate_state != ESTIMATE_PENDING) {
		goto exit;
	}

	rc = max1720x_update_battery_qh_based_capacity(chip);
	if (rc < 0)
		goto ioerr;

	settle_cc = reg_to_micro_amp_h(chip->current_capacity, chip->RSense, lsb);

	data = max1720x_get_battery_vfsoc(chip);
	if (data < 0)
		goto ioerr;

	settle_vfsoc = data;
	settle_cc = settle_cc / 1000;
	delta_cc = settle_cc - cap_esti->start_cc;
	delta_vfsoc = settle_vfsoc - cap_esti->start_vfsoc;

	if ((delta_cc > 0) && (delta_vfsoc > 0)) {

		cc_sum = delta_cc + cap_esti->delta_cc_sum;
		vfsoc_sum = delta_vfsoc + cap_esti->delta_vfsoc_sum;

		if (cap_esti->cap_filter_count >= cap_esti->cap_filt_length) {
			const int filter_divisor = cap_esti->cap_filt_length;

			cc_sum -= cap_esti->delta_cc_sum/filter_divisor;
			vfsoc_sum -= cap_esti->delta_vfsoc_sum/filter_divisor;
		}

		cap_esti->cap_filter_count++;
		cap_esti->delta_cc_sum = cc_sum;
		cap_esti->delta_vfsoc_sum = vfsoc_sum;
		batt_ce_store_data(&chip->regmap_nvram, &chip->cap_estimate);

		valid_estimate = true;
	}

ioerr:
	batt_ce_stop_estimation(cap_esti, ESTIMATE_DONE);

exit:
	logbuffer_log(chip->ce_log,
		"valid=%d settle[cc=%d, vfsoc=%d], delta[cc=%d,vfsoc=%d] ce[%d]=%d\n",
		valid_estimate,
		settle_cc, settle_vfsoc, delta_cc, delta_vfsoc,
		cap_esti->cap_filter_count,
		batt_ce_full_estimate(cap_esti));

	mutex_unlock(&cap_esti->batt_ce_lock);

	/* force to update uevent to framework side. */
	if (valid_estimate)
		power_supply_changed(chip->psy);
}

/*
 * batt_ce_init(): estimate_state = ESTIMATE_NONE
 * batt_ce_start(): estimate_state = ESTIMATE_NONE -> ESTIMATE_PENDING
 * batt_ce_capacityfiltered_work(): ESTIMATE_PENDING->ESTIMATE_DONE
 */
static int batt_ce_start(struct gbatt_capacity_estimation *cap_esti,
			 int cap_tsettle_ms)
{
	mutex_lock(&cap_esti->batt_ce_lock);

	/* Still has cable and estimate is not pending or cancelled */
	if (!cap_esti->cable_in || cap_esti->estimate_state != ESTIMATE_NONE)
		goto done;

	pr_info("EOC: Start the settle timer\n");
	cap_esti->estimate_state = ESTIMATE_PENDING;
	schedule_delayed_work(&cap_esti->settle_timer,
		msecs_to_jiffies(cap_tsettle_ms));

done:
	mutex_unlock(&cap_esti->batt_ce_lock);
	return 0;
}

static int batt_ce_init(struct gbatt_capacity_estimation *cap_esti,
			struct max1720x_chip *chip)
{
	int rc;
	u16 vfsoc = 0;
	const int lsb = max_m5_cap_lsb(chip->model_data);

	rc = max1720x_update_battery_qh_based_capacity(chip);
	if (rc < 0)
		return -EIO;

	vfsoc = max1720x_get_battery_vfsoc(chip);
	if (vfsoc < 0)
		return -EIO;

	cap_esti->start_vfsoc = vfsoc;
	cap_esti->start_cc = reg_to_micro_amp_h(chip->current_capacity,
						chip->RSense, lsb) / 1000;
	/* Capacity Estimation starts only when the state is NONE */
	cap_esti->estimate_state = ESTIMATE_NONE;
	return 0;
}

/* ------------------------------------------------------------------------- */
#define SEL_RES_AVG		0
#define SEL_RES_FILTER_COUNT	1
static int batt_res_registers(struct max1720x_chip *chip, bool bread,
			      int isel, u16 *data)
{
	int err = -EINVAL;
	const struct max17x0x_reg *bres;
	u16 res_filtered, res_filt_count, val;

	bres = max17x0x_find_by_tag(&chip->regmap_nvram, MAX17X0X_TAG_BRES);
	if (!bres)
		return err;

	switch (isel) {
	case SEL_RES_AVG:
		if (bread) {
			err = REGMAP_READ(&chip->regmap_nvram, bres->map[0],
					  &res_filtered);
			if (err)
				return err;

			*data = res_filtered;
			return 0;
		}
		err = REGMAP_WRITE(&chip->regmap_nvram, bres->map[0], *data);
		break;
	case SEL_RES_FILTER_COUNT:
		err = REGMAP_READ(&chip->regmap_nvram, bres->map[1], &val);
		if (err)
			return err;

		if (bread) {
			res_filt_count = (val & 0xF000) >> 12;
			*data = res_filt_count;
			return 0;
		}

		res_filt_count = (val & 0x0FFF) | (*data << 12);
		err = REGMAP_WRITE(&chip->regmap_nvram, bres->map[1],
				   res_filt_count);
		break;
	default:
		break;
	}

	return err;
}

static int max1720x_get_property(struct power_supply *psy,
				 enum power_supply_property psp,
				 union power_supply_propval *val)
{
	struct max1720x_chip *chip = (struct max1720x_chip *)
					power_supply_get_drvdata(psy);
	struct max17x0x_regmap *map = &chip->regmap;
	int rc, err = 0;
	u16 data = 0;
	int idata;

	pm_runtime_get_sync(chip->dev);
	if (!chip->init_complete || !chip->resume_complete) {
		pm_runtime_put_sync(chip->dev);
		return -EAGAIN;
	}
	pm_runtime_put_sync(chip->dev);

	mutex_lock(&chip->model_lock);

	switch (psp) {
	case POWER_SUPPLY_PROP_STATUS:
		err = max1720x_get_battery_status(chip);
		if (err < 0)
			break;

		/*
		 * Capacity estimation must run only once.
		 * NOTE: this is a getter with a side effect
		 */
		val->intval = err;
		if (err == POWER_SUPPLY_STATUS_FULL)
			batt_ce_start(&chip->cap_estimate,
				      chip->cap_estimate.cap_tsettle);
		break;
	case POWER_SUPPLY_PROP_HEALTH:
		val->intval = max1720x_get_battery_health(chip);
		break;
	case GBMS_PROP_CAPACITY_RAW:
		err = max1720x_get_capacity_raw(chip, &data);
		if (err == 0)
			val->intval = (int)data;
		break;
	case POWER_SUPPLY_PROP_CAPACITY:
		idata = max1720x_get_battery_soc(chip);
		if (idata < 0) {
			err = idata;
			break;
		}

		val->intval = idata;
		break;
	case POWER_SUPPLY_PROP_CHARGE_COUNTER:
		err = max1720x_update_battery_qh_based_capacity(chip);
		if (err < 0)
			break;

		val->intval = reg_to_capacity_uah(chip->current_capacity, chip);
		break;
	case POWER_SUPPLY_PROP_CHARGE_FULL:
		/*
		 * Snap charge_full to DESIGNCAP during early charge cycles to
		 * prevent large fluctuations in FULLCAPNOM. MAX1720X_CYCLES LSB
		 * is 16%
		 */
		err = max1720x_get_cycle_count(chip);
		if (err < 0)
			break;

		/* err is cycle_count */
		if (err <= FULLCAPNOM_STABILIZE_CYCLES)
			err = REGMAP_READ(map, MAX1720X_DESIGNCAP, &data);
		else
			err = REGMAP_READ(map, MAX1720X_FULLCAPNOM, &data);

		if (err == 0)
			val->intval = reg_to_capacity_uah(data, chip);
		break;
	case POWER_SUPPLY_PROP_CHARGE_FULL_DESIGN:
		err = REGMAP_READ(map, MAX1720X_DESIGNCAP, &data);
		if (err == 0)
			val->intval = reg_to_capacity_uah(data, chip);
		break;
	/* current is positive value when flowing to device */
	case POWER_SUPPLY_PROP_CURRENT_AVG:
		err = max17x0x_reg_read(map, MAX17X0X_TAG_avgc, &data);
		if (err == 0)
			val->intval = -reg_to_micro_amp(data, chip->RSense);
		break;
	/* current is positive value when flowing to device */
	case POWER_SUPPLY_PROP_CURRENT_NOW:
		err = max17x0x_reg_read(map, MAX17X0X_TAG_curr, &data);
		if (err == 0)
			val->intval = -reg_to_micro_amp(data, chip->RSense);
		break;
	case POWER_SUPPLY_PROP_CYCLE_COUNT:
		err = max1720x_get_cycle_count(chip);
		if (err < 0)
			break;
		/* err is cycle_count */
		val->intval = err;
		break;
	case POWER_SUPPLY_PROP_PRESENT:
		if (chip->gauge_type == -1) {
			val->intval = 0;
		} else {
			err = REGMAP_READ(map, MAX1720X_STATUS, &data);
			if (err < 0)
				break;

			/* BST is 0 when the battery is present */
			val->intval = (((u16) data) & MAX1720X_STATUS_BST)
							? 0 : 1;
		}
		break;
	case POWER_SUPPLY_PROP_TEMP:
		err = max17x0x_reg_read(map, MAX17X0X_TAG_temp, &data);
		if (err < 0)
			break;

		val->intval = reg_to_deci_deg_cel(data);
		max1720x_handle_update_nconvgcfg(chip, val->intval);
		max1720x_handle_update_empty_voltage(chip, val->intval);
		break;
	case POWER_SUPPLY_PROP_TIME_TO_EMPTY_AVG:
		err = REGMAP_READ(map, MAX1720X_TTE, &data);
		if (err == 0)
			val->intval = reg_to_seconds(data);
		break;
	case POWER_SUPPLY_PROP_TIME_TO_FULL_AVG:
		err = REGMAP_READ(map, MAX1720X_TTF, &data);
		if (err == 0)
			val->intval = reg_to_seconds(data);
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_AVG:
		err = REGMAP_READ(map, MAX1720X_AVGVCELL, &data);
		if (err == 0)
			val->intval = reg_to_micro_volt(data);
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_MAX_DESIGN:
		/* LSB: 20mV */
		err = max17x0x_reg_read(map, MAX17X0X_TAG_mmdv, &data);
		if (err == 0)
			val->intval = ((data >> 8) & 0xFF) * 20000;
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_MIN_DESIGN:
		/* LSB: 20mV */
		err = max17x0x_reg_read(map, MAX17X0X_TAG_mmdv, &data);
		if (err == 0)
			val->intval = (data & 0xFF) * 20000;
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_NOW:
		err = max17x0x_reg_read(map, MAX17X0X_TAG_vcel, &data);
		if (err == 0)
			val->intval = reg_to_micro_volt(data);
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_OCV:
		rc = max17x0x_reg_read(map, MAX17X0X_TAG_vfocv, &data);
		if (rc == -EINVAL) {
			val->intval = -1;
			break;
		}
		val->intval = reg_to_micro_volt(data);
		break;
	case POWER_SUPPLY_PROP_TECHNOLOGY:
		val->intval = POWER_SUPPLY_TECHNOLOGY_LION;
		break;
	case POWER_SUPPLY_PROP_SERIAL_NUMBER:
		val->strval = chip->serial_number;
		break;
	default:
		err = -EINVAL;
		break;
	}

	if (err < 0)
		pr_debug("error %d reading prop %d\n", err, psp);

	mutex_unlock(&chip->model_lock);
	return err;
}

static void max1720x_fixup_capacity(struct max1720x_chip *chip, int plugged)
{
	struct max1720x_drift_data *ddata = &chip->drift_data;
	int ret, cycle_count, cap_lsb;
	u16 data16;

	/* do not execute when POR is set */
	ret = REGMAP_READ(&chip->regmap, MAX1720X_STATUS, &data16);
	if (ret < 0 || data16 & MAX1720X_STATUS_POR)
		return;

	/* capacity outliers: fix rcomp0, tempco */
	ret = max1720x_fixup_comp(ddata, &chip->regmap, plugged);
	if (ret > 0) {
		chip->comp_update_count += 1;

		data16 = chip->comp_update_count;
		ret = gbms_storage_write(GBMS_TAG_CMPC, &data16, sizeof(data16));
		if (ret < 0)
			dev_err(chip->dev, "update comp stats (%d)\n", ret);
	}

	cycle_count = max1720x_get_cycle_count(chip);
	if (cycle_count < 0) {
		dev_err(chip->dev, "cannot read cycle_count (%d)\n",
			cycle_count);
		return;
	}

	/* capacity outliers: fix capacity */
	cap_lsb = max_m5_cap_lsb(chip->model_data);
	ret = max1720x_fixup_dxacc(ddata, &chip->regmap, cycle_count, plugged, cap_lsb);
	if (ret > 0) {
		chip->dxacc_update_count += 1;

		data16 = chip->dxacc_update_count;
		ret = gbms_storage_write(GBMS_TAG_DXAC, &data16, sizeof(data16));
		if (ret < 0)
			dev_err(chip->dev, "update cap stats (%d)\n", ret);
	}

}

static int max1720x_set_property(struct power_supply *psy,
				 enum power_supply_property psp,
				 const union power_supply_propval *val)
{
	struct max1720x_chip *chip = (struct max1720x_chip *)
					power_supply_get_drvdata(psy);
	struct gbatt_capacity_estimation *ce = &chip->cap_estimate;
	int delay_ms = 0;
	int rc = 0;

	pm_runtime_get_sync(chip->dev);
	if (!chip->init_complete || !chip->resume_complete) {
		pm_runtime_put_sync(chip->dev);
		return -EAGAIN;
	}
	pm_runtime_put_sync(chip->dev);

	switch (psp) {
	case GBMS_PROP_BATT_CE_CTRL:

		mutex_lock(&ce->batt_ce_lock);
		if (val->intval) {

			if (!ce->cable_in) {
				rc = batt_ce_init(ce, chip);
				ce->cable_in = (rc == 0);
			}

		} else if (ce->cable_in) {
			if (ce->estimate_state == ESTIMATE_PENDING)
				cancel_delayed_work_sync(&ce->settle_timer);

			/* race with batt_ce_capacityfiltered_work() */
			batt_ce_stop_estimation(ce, ESTIMATE_NONE);
			batt_ce_dump_data(ce, chip->ce_log);
			ce->cable_in = false;
		}
		mutex_unlock(&ce->batt_ce_lock);


		/* check cycle count, save state, check drift if needed */
		delay_ms = max1720x_check_drift_delay(&chip->drift_data);
		mod_delayed_work(system_wq, &chip->model_work, delay_ms);

		break;
	default:
		return -EINVAL;
	}

	if (rc < 0)
		return rc;

	return 0;
}

static int max1720x_property_is_writeable(struct power_supply *psy,
					  enum power_supply_property psp)
{
	switch (psp) {
	case GBMS_PROP_BATT_CE_CTRL:
		return 1;
	default:
		break;
	}

	return 0;
}

/*
 * A fuel gauge reset resets only the fuel gauge operation without resetting IC
 * hardware. This is useful for testing different configurations without writing
 * nonvolatile memory.
 * TODO: add a lock around fg_reset to prevent SW from accessing the gauge until
 * the delay for volatile register access (rset->map[2]) expires. Need a lock
 * only if using this after _init()
 */
static int max17x0x_fg_reset(struct max1720x_chip *chip)
{
	const struct max17x0x_reg *rset;
	bool done = false;
	int err;

	rset = max17x0x_find_by_tag(&chip->regmap_nvram, MAX17X0X_TAG_rset);
	if (!rset)
		return -EINVAL;

	dev_info(chip->dev, "FG_RESET addr=%x value=%x delay=%d\n",
			    rset->map16[0], rset->map16[1], rset->map16[2]);

	err = REGMAP_WRITE(&chip->regmap, rset->map16[0], rset->map16[1]);
	if (err < 0) {
		dev_err(chip->dev, "FG_RESET error writing Config2 (%d)\n",
				   err);
	} else {
		int loops = 10; /* 10 * MAX17X0X_TPOR_MS = 1.5 secs */
		u16 cfg2 = 0;

		for ( ; loops ; loops--) {
			msleep(MAX17X0X_TPOR_MS);

			err = REGMAP_READ(&chip->regmap, rset->map16[0], &cfg2);
			done = (err == 0) && !(cfg2 & rset->map16[1]);
			if (done) {
				msleep(rset->map16[2]);
				break;
			}
		}

		if (!done)
			dev_err(chip->dev, "FG_RESET error rst not clearing\n");
		else
			dev_info(chip->dev, "FG_RESET cleared in %dms\n",
				loops * MAX17X0X_TPOR_MS + rset->map16[2]);

	}

	return 0;
}

/*
 * A full reset restores the ICs to their power-up state the same as if power
 * had been cycled.
 */
static int max1720x_full_reset(struct max1720x_chip *chip)
{
	if (chip->gauge_type == MAX1730X_GAUGE_TYPE) {
		/*
		 * a full (hw) reset on max1730x cause charge and discharge FET
		 * to toggle and the device will lose power. Will need to
		 * connect the device to a charger to get max1730x firmware to
		 * start and max1730x to close the FETs. Never send a HW reset
		 * to a 1730x while in system...
		 */
		dev_warn(chip->dev, "ignore full reset of fuel gauge\n");
		return 0;
	}

	REGMAP_WRITE(&chip->regmap, MAX17XXX_COMMAND,
		     MAX1720X_COMMAND_HARDWARE_RESET);

	msleep(MAX17X0X_TPOR_MS);

	return 0;
}

#define IRQ_STORM_TRIGGER_SECONDS		60
#define IRQ_STORM_TRIGGER_MAX_COUNTS		50
static bool max1720x_fg_irq_storm_check(struct max1720x_chip *chip)
{
	int now_time = 0, interval_time, irq_cnt;
	bool storm = false;
	static int stime;

	chip->icnt++;

	now_time = div_u64(ktime_to_ns(ktime_get_boottime()), NSEC_PER_SEC);
	if (now_time < IRQ_STORM_TRIGGER_SECONDS) {
		stime = now_time;
		chip->icnt = 0;
	}

	interval_time = now_time - stime;
	if (interval_time  > IRQ_STORM_TRIGGER_SECONDS) {
		irq_cnt = chip->icnt * 100;
		irq_cnt /= (interval_time * 100 / IRQ_STORM_TRIGGER_SECONDS);

		storm = irq_cnt > IRQ_STORM_TRIGGER_MAX_COUNTS;
		if (!storm) {
			stime = now_time;
			chip->icnt = 0;
		}
	}

	return storm;
}

static irqreturn_t max1720x_fg_irq_thread_fn(int irq, void *obj)
{
	struct max1720x_chip *chip = (struct max1720x_chip *)obj;
	u16 fg_status, fg_status_clr;
	bool storm = false;
	int err = 0;


	if (!chip || (irq != -1 && irq != chip->primary->irq)) {
		WARN_ON_ONCE(1);
		return IRQ_NONE;
	}

	if (chip->gauge_type == -1)
		return IRQ_NONE;

	pm_runtime_get_sync(chip->dev);
	if (!chip->init_complete || !chip->resume_complete) {
		pm_runtime_put_sync(chip->dev);
		return -EAGAIN;
	}

	pm_runtime_put_sync(chip->dev);

	err = REGMAP_READ(&chip->regmap, MAX1720X_STATUS, &fg_status);
	if (err)
		return IRQ_NONE;

	/* disable storm check and spurius with shared interrupts */
	if (!chip->irq_shared) {

		storm = max1720x_fg_irq_storm_check(chip);
		if (storm) {
			u16 fg_alarm = 0;

			if (chip->gauge_type != MAX_M5_GAUGE_TYPE)
				err = REGMAP_READ(&chip->regmap, MAX1720X_ALARM,
						&fg_alarm);

			dev_warn(chip->dev, "sts:%04x, alarm:%04x, cnt:%lu err=%d\n",
				fg_status, fg_alarm, chip->icnt, err);
		}

		if (fg_status == 0) {
			chip->debug_irq_none_cnt++;
			pr_debug("spurius: fg_status=0 cnt=%d\n",
				chip->debug_irq_none_cnt);
			/* rate limit spurius interrupts */
			msleep(MAX1720X_TICLR_MS);
			return IRQ_HANDLED;
		}
	} else if (fg_status == 0) {
		/*
		 * Disable rate limiting for when interrupt is shared.
		 * NOTE: this might need to be re-evaluated at some later point
		 */
		return IRQ_NONE;
	}

	/* only used to report health */
	chip->health_status |= fg_status;

	/*
	 * write 0 to clear will loose interrupts when we don't write 1 to the
	 * bits that are not set. Just inverting fg_status cause an interrupt
	 * storm, only setting the bits marked as "host must clear" in the DS
	 * seems to work eg:
	 *
	 * fg_status_clr = fg_status
	 * fg_status_clr |= MAX1720X_STATUS_POR | MAX1720X_STATUS_DSOCI
	 *                | MAX1720X_STATUS_BI;
	 *
	 * If the above logic is sound, we probably need to set also the bits
	 * that config mark as "host must clear". Maxim to confirm.
	 */
	fg_status_clr = fg_status;

	if (fg_status & MAX1720X_STATUS_POR) {
		pr_debug("POR is set\n");

		/* trigger model load */
		mutex_lock(&chip->model_lock);
		if (chip->gauge_type == MAX_M5_GAUGE_TYPE)
			max1720x_model_reload(chip, false);
		else
			fg_status_clr &= ~MAX1720X_STATUS_POR;
		mutex_unlock(&chip->model_lock);
	}

	if (fg_status & MAX1720X_STATUS_IMN)
		pr_debug("IMN is set\n");

	if (fg_status & MAX1720X_STATUS_BST)
		pr_debug("BST is set\n");

	if (fg_status & MAX1720X_STATUS_IMX)
		pr_debug("IMX is set\n");

	if (fg_status & MAX1720X_STATUS_DSOCI) {
		fg_status_clr &= ~MAX1720X_STATUS_DSOCI;
		pr_debug("DSOCI is set\n");
	}
	if (fg_status & MAX1720X_STATUS_VMN) {
		if (chip->RConfig & MAX1720X_CONFIG_VS)
			fg_status_clr &= ~MAX1720X_STATUS_VMN;
		pr_debug("VMN is set\n");
	}
	if (fg_status & MAX1720X_STATUS_TMN) {
		if (chip->RConfig & MAX1720X_CONFIG_TS)
			fg_status_clr &= ~MAX1720X_STATUS_TMN;
		pr_debug("TMN is set\n");
	}
	if (fg_status & MAX1720X_STATUS_SMN) {
		if (chip->RConfig & MAX1720X_CONFIG_SS)
			fg_status_clr &= ~MAX1720X_STATUS_SMN;
		pr_debug("SMN is set\n");
	}
	if (fg_status & MAX1720X_STATUS_BI)
		pr_debug("BI is set\n");

	if (fg_status & MAX1720X_STATUS_VMX) {
		if (chip->RConfig & MAX1720X_CONFIG_VS)
			fg_status_clr &= ~MAX1720X_STATUS_VMX;
		pr_debug("VMX is set\n");
	}
	if (fg_status & MAX1720X_STATUS_TMX) {
		if (chip->RConfig & MAX1720X_CONFIG_TS)
			fg_status_clr &= ~MAX1720X_STATUS_TMX;
		pr_debug("TMX is set\n");
	}
	if (fg_status & MAX1720X_STATUS_SMX) {
		if (chip->RConfig & MAX1720X_CONFIG_SS)
			fg_status_clr &= ~MAX1720X_STATUS_SMX;
		pr_debug("SMX is set\n");
	}

	if (fg_status & MAX1720X_STATUS_BR)
		pr_debug("BR is set\n");

	/* NOTE: should always clear everything even if we lose state */
	REGMAP_WRITE(&chip->regmap, MAX1720X_STATUS, fg_status_clr);

	/* SOC interrupts need to go through all the time */
	if (fg_status & MAX1720X_STATUS_DSOCI) {
		const bool plugged = chip->cap_estimate.cable_in;

		if (max1720x_check_drift_on_soc(&chip->drift_data))
			max1720x_fixup_capacity(chip, plugged);

		if (storm)
			pr_debug("Force power_supply_change in storm\n");
		storm = false;
	}

	if (chip->psy && !storm)
		power_supply_changed(chip->psy);

	/*
	 * oneshot w/o filter will unmask on return but gauge will take up
	 * to 351 ms to clear ALRM1.
	 * NOTE: can do this masking on gauge side (Config, 0x1D) and using a
	 * workthread to re-enable.
	 */
	if (irq != -1)
		msleep(MAX1720X_TICLR_MS);


	return IRQ_HANDLED;
}

/* used to find batt_node and chemistry dependent FG overrides */
static int max1720x_read_batt_id(int *batt_id, const struct max1720x_chip *chip)
{
	bool defer;
	int rc = 0;
	struct device_node *node = chip->dev->of_node;
	u32 temp_id = 0;

	/* force the value in kohm */
	rc = of_property_read_u32(node, "maxim,force-batt-id", &temp_id);
	if (rc == 0) {
		dev_warn(chip->dev, "forcing battery RID %d\n", temp_id);
		*batt_id = temp_id;
		return 0;
	}

	/* return the value in kohm */
	rc = gbms_storage_read(GBMS_TAG_BRID, &temp_id, sizeof(temp_id));
	defer = (rc == -EPROBE_DEFER) ||
		(rc == -EINVAL) ||
		((rc == 0) && (temp_id == -EINVAL));
	if (defer)
		return -EPROBE_DEFER;

	if (rc < 0) {
		dev_err(chip->dev, "failed to get batt-id rc=%d\n", rc);
		*batt_id = -1;
		return -EINVAL;
	}

	*batt_id = temp_id;
	return 0;
}

/*  */
static struct device_node *max1720x_find_batt_node(struct max1720x_chip *chip)
{
	const int batt_id = chip->batt_id;
	const struct device *dev = chip->dev;
	struct device_node *config_node, *child_node;
	u32 batt_id_range = 20, batt_id_kohm;
	int ret;

	config_node = of_find_node_by_name(dev->of_node, "maxim,config");
	if (!config_node) {
		dev_warn(dev, "Failed to find maxim,config setting\n");
		return NULL;
	}

	ret = of_property_read_u32(dev->of_node, "maxim,batt-id-range-pct",
				   &batt_id_range);
	if (ret && ret == -EINVAL)
		dev_warn(dev, "failed to read maxim,batt-id-range-pct\n");

	for_each_child_of_node(config_node, child_node) {
		ret = of_property_read_u32(child_node, "maxim,batt-id-kohm",
					   &batt_id_kohm);
		if (ret != 0)
			continue;

		/* only look for matching algo_ver if set */
		if (chip->drift_data.algo_ver != MAX1720X_DA_VER_NONE) {
			u32 algo_ver;

			ret = of_property_read_u32(child_node,
						   "maxim,algo-version",
						   &algo_ver);
			if (ret == 0 && chip->drift_data.algo_ver != algo_ver)
				continue;
		}

		if (!batt_id_range && batt_id == batt_id_kohm)
			return child_node;
		if ((batt_id < (batt_id_kohm * (100 + batt_id_range) / 100)) &&
		    (batt_id > (batt_id_kohm * (100 - batt_id_range) / 100)))
			return child_node;
	}

	return NULL;
}

static int max17x0x_apply_regval_shadow(struct max1720x_chip *chip,
					struct device_node *node,
					struct max17x0x_cache_data *nRAM,
					int nb)
{
	u16 *regs;
	int ret, i;
	const char *propname = (chip->gauge_type == MAX1730X_GAUGE_TYPE) ?
		 "maxim,n_regval_1730x" : "maxim,n_regval_1720x";

	if (!node || nb <= 0)
		return 0;

	if (nb & 1) {
		dev_warn(chip->dev, "%s %s u16 elems count is not even: %d\n",
			 node->name, propname, nb);
		return -EINVAL;
	}

	regs = batt_alloc_array(nb, sizeof(u16));
	if (!regs)
		return -ENOMEM;

	ret = of_property_read_u16_array(node, propname, regs, nb);
	if (ret) {
		dev_warn(chip->dev, "failed to read %s: %d\n", propname, ret);
		goto shadow_out;
	}

	for (i = 0; i < nb; i += 2) {
		const int idx = max17x0x_cache_index_of(nRAM, regs[i]);
		nRAM->cache_data[idx] = regs[i + 1];
	}

shadow_out:
	kfree(regs);
	return ret;
}

/* support for initial batch of ill configured max1720x packs */
static void max1720x_consistency_check(struct max17x0x_cache_data *cache)
{
	int nvcfg_idx = max17x0x_cache_index_of(cache, MAX1720X_NNVCFG0);
	int ncgain_idx = max17x0x_cache_index_of(cache, MAX1720X_NCGAIN);
	u16 *nRAM_updated = cache->cache_data;

	if ((nRAM_updated[nvcfg_idx] & MAX1720X_NNVCFG0_ENCG) &&
		((nRAM_updated[ncgain_idx] == 0) ||
		(nRAM_updated[ncgain_idx] == 0x0400)))
		nRAM_updated[ncgain_idx] = 0x4000;
}

static int max17x0x_read_dt_version(struct device_node *node,
				    int gauge_type, u8 *reg, u8 *val)
{
	int ret;
	const char *propname;
	u8 version[2];

	if (gauge_type == MAX1730X_GAUGE_TYPE) {
		propname = "maxim,n_regval_1730x_ver";
	} else if (gauge_type == MAX1720X_GAUGE_TYPE) {
		propname = "maxim,n_regval_1720x_ver";
	} else {
		return -ENOTSUPP;
	}

	ret = of_property_read_u8_array(node, propname,
					version,
					sizeof(version));
	if (ret < 0)
		return -ENODATA;

	*reg = version[0];
	*val = version[1];

	return 0;
}

static int max17x0x_read_dt_version_por(struct device_node *node,
					int gauge_type, u8 *reg, u8 *val)
{
	int ret;
	const char *propname;
	u8 version[2];

	if (gauge_type == MAX1730X_GAUGE_TYPE) {
		propname = "maxim,n_regval_1730x_ver_por";
	} else if (gauge_type == MAX1720X_GAUGE_TYPE) {
		propname = "maxim,n_regval_1720x_ver_por";
	} else {
		return -ENOTSUPP;
	}

	ret = of_property_read_u8_array(node, propname,
					version,
					sizeof(version));
	if (ret < 0)
		return -ENODATA;

	*reg = version[0];
	*val = version[1];

	return 0;
}

static int max17x0x_handle_dt_shadow_config(struct max1720x_chip *chip)
{
	int ret, rc, glob_cnt;
	const char *propname = NULL;
	struct max17x0x_cache_data nRAM_c;
	struct max17x0x_cache_data nRAM_u;
	int ver_idx = -1;
	u8 vreg, vval;

	/* for devices that don't support max1720x_fg_reset() */
	if (!chip->shadow_override || chip->gauge_type == -1)
		return 0;

	if (chip->gauge_type == MAX1730X_GAUGE_TYPE)
		propname = "maxim,n_regval_1730x";
	else
		propname = "maxim,n_regval_1720x";

	ret = max17x0x_nvram_cache_init(&nRAM_c, chip->gauge_type);
	if (ret < 0)
		return ret;

	ret = max17x0x_cache_load(&nRAM_c, &chip->regmap_nvram);
	if (ret < 0) {
		dev_err(chip->dev, "Failed to read config from shadow RAM\n");
		goto error_out;
	}

	ret = max17x0x_cache_dup(&nRAM_u, &nRAM_c);
	if (ret < 0)
		goto error_out;

	/* apply overrides */
	if (chip->batt_node) {
		int batt_cnt;

		batt_cnt = of_property_count_elems_of_size(chip->batt_node,
							   propname,
							   sizeof(u16));
		max17x0x_apply_regval_shadow(chip, chip->batt_node,
					     &nRAM_u,
					     batt_cnt);
	}

	glob_cnt = of_property_count_elems_of_size(chip->dev->of_node,
						   propname,
						   sizeof(u16));
	max17x0x_apply_regval_shadow(chip, chip->dev->of_node,
				     &nRAM_u,
				     glob_cnt);

	if (chip->gauge_type == MAX1720X_GAUGE_TYPE)
		max1720x_consistency_check(&nRAM_u);

	rc = max17x0x_read_dt_version(chip->dev->of_node,
				      chip->gauge_type, &vreg, &vval);
	if (rc == 0) {
		/*
		 * Versioning enforced: reset the gauge (and overwrite
		 * version) only if the version in device tree is
		 * greater than the version in the gauge.
		 */
		ver_idx = max17x0x_cache_index_of(&nRAM_u, vreg);
		if (ver_idx < 0) {
			dev_err(chip->dev, "version register %x is not mapped\n",
					vreg);
		} else if ((nRAM_u.cache_data[ver_idx] & 0xff) < vval) {
			/*
			 * force version in dt, will write (and reset fg)
			 * only when less than the version in nRAM_c
			 */
			dev_info(chip->dev,
				"DT version updated %d -> %d\n",
				nRAM_u.cache_data[ver_idx] & 0xff,
				vval);

			nRAM_u.cache_data[ver_idx] &= 0xff00;
			nRAM_u.cache_data[ver_idx] |= vval;
			chip->needs_reset = true;
		}
	}

	if (max17x0x_cache_memcmp(&nRAM_c, &nRAM_u)) {
		bool fg_reset = false;

		if (ver_idx < 0) {
			/*
			 * Versioning not enforced: nConvgCfg take effect
			 * without resetting the gauge
			 */
			const int idx = max17x0x_cache_index_of(&nRAM_u,
							MAX1720X_NCONVGCFG);
			nRAM_c.cache_data[idx] = nRAM_u.cache_data[idx];
			fg_reset = max17x0x_cache_memcmp(&nRAM_u, &nRAM_c) != 0;
		}

		ret = max17x0x_cache_store(&nRAM_u, &chip->regmap_nvram);
		if (ret < 0) {
			dev_err(chip->dev,
				"Failed to write config from shadow RAM\n");
			goto error_out;
		}

		/* different reason for reset */
		if (fg_reset) {
			chip->needs_reset = true;
			dev_info(chip->dev,
				"DT config differs from shadow, resetting\n");
		}
	}

error_out:
	max17x0x_cache_free(&nRAM_c);
	max17x0x_cache_free(&nRAM_u);

	return ret;
}

static int max17x0x_apply_regval_register(struct max1720x_chip *chip,
					struct device_node *node)
{
	int cnt, ret = 0, idx, err;
	u16 *regs, data;
	const char *propname;

	propname = (chip->gauge_type == MAX1730X_GAUGE_TYPE) ?
		 "maxim,r_regval_1730x" : "maxim,r_regval_1720x";

	cnt =  of_property_count_elems_of_size(node, propname, sizeof(u16));
	if (!node || cnt <= 0)
		return 0;

	if (cnt & 1) {
		dev_warn(chip->dev, "%s %s u16 elems count is not even: %d\n",
			 node->name, propname, cnt);
		return -EINVAL;
	}

	regs = batt_alloc_array(cnt, sizeof(u16));
	if (!regs)
		return -ENOMEM;

	ret = of_property_read_u16_array(node, propname, regs, cnt);
	if (ret) {
		dev_warn(chip->dev, "failed to read %s %s: %d\n",
			 node->name, propname, ret);
		goto register_out;
	}

	for (idx = 0; idx < cnt; idx += 2) {
		if (max1720x_is_reg(chip->dev, regs[idx])) {
			err = REGMAP_READ(&chip->regmap, regs[idx], &data);
			if (!err && data != regs[idx + 1])
				REGMAP_WRITE(&chip->regmap, regs[idx],
					     regs[idx + 1]);
		}
	}
register_out:
	kfree(regs);
	return ret;
}

static int max17x0x_handle_dt_register_config(struct max1720x_chip *chip)
{
	int ret = 0;

	if (chip->batt_node)
		ret = max17x0x_apply_regval_register(chip, chip->batt_node);

	if (ret)
		return ret;

	ret = max17x0x_apply_regval_register(chip, chip->dev->of_node);

	return ret;
}

static int max1720x_handle_dt_nconvgcfg(struct max1720x_chip *chip)
{
	int ret = 0, i;
	struct device_node *node = chip->dev->of_node;

	chip->curr_convgcfg_idx = -1;
	mutex_init(&chip->convgcfg_lock);

	ret = of_property_read_u32(node, "google,cap-tsettle",
				   (u32 *)&chip->cap_estimate.cap_tsettle);
	if (ret < 0)
		chip->cap_estimate.cap_tsettle = DEFAULT_CAP_SETTLE_INTERVAL;
	chip->cap_estimate.cap_tsettle =
				chip->cap_estimate.cap_tsettle * 60 * 1000;

	ret = of_property_read_u32(node, "google,cap-filt-length",
				   (u32 *)&chip->cap_estimate.cap_filt_length);
	if (ret < 0)
		chip->cap_estimate.cap_filt_length = DEFAULT_CAP_FILTER_LENGTH;

	chip->nb_convgcfg =
	    of_property_count_elems_of_size(node, "maxim,nconvgcfg-temp-limits",
					    sizeof(s16));
	if (!chip->nb_convgcfg)
		return 0;

	ret = of_property_read_s32(node, "maxim,nconvgcfg-temp-hysteresis",
				   &chip->convgcfg_hysteresis);
	if (ret < 0)
		chip->convgcfg_hysteresis = 10;
	else if (chip->convgcfg_hysteresis < 0)
			chip->convgcfg_hysteresis = 10;
	if (ret == 0)
		dev_info(chip->dev, "%s maxim,nconvgcfg-temp-hysteresis = %d\n",
			 node->name, chip->convgcfg_hysteresis);

	if (chip->nb_convgcfg != of_property_count_elems_of_size(node,
						  "maxim,nconvgcfg-values",
						  sizeof(u16))) {
		dev_warn(chip->dev, "%s maxim,nconvgcfg-values and maxim,nconvgcfg-temp-limits are missmatching number of elements\n",
			 node->name);
		return -EINVAL;
	}
	chip->temp_convgcfg = (s16 *)devm_kmalloc_array(chip->dev,
							chip->nb_convgcfg,
							sizeof(s16),
								GFP_KERNEL);
	if (!chip->temp_convgcfg)
		return -ENOMEM;

	chip->convgcfg_values = (u16 *)devm_kmalloc_array(chip->dev,
							  chip->nb_convgcfg,
							  sizeof(u16),
							  GFP_KERNEL);
	if (!chip->convgcfg_values) {
		devm_kfree(chip->dev, chip->temp_convgcfg);
		chip->temp_convgcfg = NULL;
		return -ENOMEM;
	}

	ret = of_property_read_u16_array(node, "maxim,nconvgcfg-temp-limits",
					 (u16 *) chip->temp_convgcfg,
					 chip->nb_convgcfg);
	if (ret) {
		dev_warn(chip->dev, "failed to read maxim,nconvgcfg-temp-limits: %d\n",
			 ret);
		goto error;
	}

	ret = of_property_read_u16_array(node, "maxim,nconvgcfg-values",
					 chip->convgcfg_values,
					 chip->nb_convgcfg);
	if (ret) {
		dev_warn(chip->dev, "failed to read maxim,nconvgcfg-values: %d\n",
			 ret);
		goto error;
	}
	for (i = 1; i < chip->nb_convgcfg; i++) {
		if (chip->temp_convgcfg[i] < chip->temp_convgcfg[i-1]) {
			dev_warn(chip->dev, "nconvgcfg-temp-limits idx:%d < idx:%d\n",
				 i, i-1);
			goto error;
		}
		if ((chip->temp_convgcfg[i] - chip->temp_convgcfg[i-1])
		    <= chip->convgcfg_hysteresis) {
			dev_warn(chip->dev, "nconvgcfg-temp-hysteresis smaller than idx:%d, idx:%d\n",
				 i, i-1);
			goto error;
		}
	}

	chip->nb_empty_voltage = of_property_count_elems_of_size(node,
								 "maxim,empty-voltage",
								 sizeof(u16));
	if (chip->nb_empty_voltage > 0 &&
	    chip->nb_empty_voltage % NB_CYCLE_BUCKETS == 0) {
		chip->empty_voltage = (u16 *)devm_kmalloc_array(chip->dev,
							chip->nb_empty_voltage,
							sizeof(u16),
							GFP_KERNEL);
		if (!chip->empty_voltage)
			goto error;

		ret = of_property_read_u16_array(node, "maxim,empty-voltage",
						chip->empty_voltage,
						chip->nb_empty_voltage);
		if (ret) {
			dev_warn(chip->dev,
				 "failed to read maxim,empty-voltage: %d\n",
				 ret);
		}
	} else
		dev_warn(chip->dev,
			 "maxim,empty-voltage is missmatching the number of elements, nb = %d\n",
			 chip->nb_empty_voltage);
error:
	if (ret) {
		devm_kfree(chip->dev, chip->temp_convgcfg);
		devm_kfree(chip->dev, chip->convgcfg_values);
		chip->temp_convgcfg = NULL;
	}

	return ret;
}

static int get_irq_none_cnt(void *data, u64 *val)
{
	struct max1720x_chip *chip = (struct max1720x_chip *)data;

	*val = chip->debug_irq_none_cnt;
	return 0;
}

static int set_irq_none_cnt(void *data, u64 val)
{
	struct max1720x_chip *chip = (struct max1720x_chip *)data;

	if (val == 0)
		chip->debug_irq_none_cnt = 0;
	return 0;
}

DEFINE_SIMPLE_ATTRIBUTE(irq_none_cnt_fops, get_irq_none_cnt,
	set_irq_none_cnt, "%llu\n");


static int debug_fg_reset(void *data, u64 val)
{
	struct max1720x_chip *chip = data;
	int ret;

	if (val == 0)
		ret = max17x0x_fg_reset(chip);
	else if (val == 1)
		ret = max1720x_full_reset(chip);
	else
		ret = -EINVAL;

	return ret;
}

DEFINE_SIMPLE_ATTRIBUTE(debug_fg_reset_fops, NULL, debug_fg_reset, "%llu\n");

static int debug_ce_start(void *data, u64 val)
{
	struct max1720x_chip *chip = (struct max1720x_chip *)data;

	batt_ce_start(&chip->cap_estimate, val);
	return 0;
}

DEFINE_SIMPLE_ATTRIBUTE(debug_ce_start_fops, NULL, debug_ce_start, "%llu\n");

/* Model reload will be disabled if the node is not found */
static int max1720x_init_model(struct max1720x_chip *chip)
{
	void *model_data;

	/* ->batt_id negative for no lookup */
	if (chip->batt_id >= 0) {
		chip->batt_node = max1720x_find_batt_node(chip);
		pr_debug("node found=%d for ID=%d algo=%d\n",
			 !!chip->batt_node, chip->batt_id,
			 chip->drift_data.algo_ver);
	}

	/* TODO: split allocation and initialization */
	model_data = max_m5_init_data(chip->dev, chip->batt_node ?
				      chip->batt_node : chip->dev->of_node,
				      &chip->regmap);
	if (IS_ERR(model_data))
		return PTR_ERR(model_data);

	chip->model_data = model_data;

	if (!chip->batt_node) {
		dev_warn(chip->dev, "No child node for ID=%d, algo=%d\n",
			 chip->batt_id, chip->drift_data.algo_ver);
		chip->model_reload = MAX_M5_LOAD_MODEL_DISABLED;
	} else {
		u32 data32;
		int rc;

		/* align algo_ver for capacity drift to model */
		rc = of_property_read_u32(chip->batt_node, "maxim,algo-version",
					  &data32);
		if (rc == 0)
			chip->drift_data.algo_ver = data32;

		pr_debug("model_data ok for ID=%d, algo=%d\n",
			 chip->batt_id, chip->drift_data.algo_ver);
		chip->model_reload = MAX_M5_LOAD_MODEL_IDLE;
	}

	return 0;
}

/* change battery_id and cause reload of the FG model */
static int debug_batt_id_set(void *data, u64 val)
{
	struct max1720x_chip *chip = (struct max1720x_chip *)data;
	int ret;

	if (chip->gauge_type != MAX_M5_GAUGE_TYPE)
		return -EINVAL;

	mutex_lock(&chip->model_lock);

	/* reset state (if needed) */
	if (chip->model_data)
		max_m5_free_data(chip->model_data);
	chip->batt_id = val;

	/* re-init the model data (lookup in DT) */
	ret = max1720x_init_model(chip);
	if (ret == 0)
		max1720x_model_reload(chip, true);

	mutex_unlock(&chip->model_lock);

	dev_info(chip->dev, "Force model for batt_id=%llu (%d)\n", val, ret);
	return 0;
}

DEFINE_SIMPLE_ATTRIBUTE(debug_batt_id_fops, NULL, debug_batt_id_set, "%llu\n");


#define BATTERY_DEBUG_ATTRIBUTE(name, fn_read, fn_write) \
static const struct file_operations name = {	\
	.open	= simple_open,			\
	.llseek	= no_llseek,			\
	.read	= fn_read,			\
	.write	= fn_write,			\
}

/*
 * dump with "cat /d/max1720x/nvram_por | xxd"
 * NOTE: for testing add a setter that initialize chip->nRAM_por (if not
 * initialized) and use _load() to read NVRAM.
 */
static ssize_t debug_get_nvram_por(struct file *filp,
				   char __user *buf,
				   size_t count, loff_t *ppos)
{
	struct max1720x_chip *chip = (struct max1720x_chip *)filp->private_data;
	int size;

	if (!chip || !chip->nRAM_por.cache_data)
		return -ENODATA;

	size = chip->nRAM_por.atom.size > count ?
			count : chip->nRAM_por.atom.size;

	return simple_read_from_buffer(buf, count, ppos,
		chip->nRAM_por.cache_data,
		size);
}

BATTERY_DEBUG_ATTRIBUTE(debug_nvram_por_fops, debug_get_nvram_por, NULL);

static void max17x0x_reglog_dump(struct max17x0x_reglog *regs,
				 size_t size,
				 char *buff)
{
	int i, len = 0;

	for (i = 0; i < NB_REGMAP_MAX; i++) {
		if (size <= len)
			break;
		if (test_bit(i, regs->valid))
			len += scnprintf(&buff[len], size - len, "%02X:%04X\n",
					 i, regs->data[i]);
	}

	if (len == 0)
		scnprintf(buff, size, "No record\n");
}

static ssize_t debug_get_reglog_writes(struct file *filp,
				       char __user *buf,
				       size_t count, loff_t *ppos)
{
	char *buff;
	ssize_t rc = 0;
	struct max17x0x_reglog *reglog =
				(struct max17x0x_reglog *)filp->private_data;

	buff = kmalloc(count, GFP_KERNEL);
	if (!buff)
		return -ENOMEM;

	max17x0x_reglog_dump(reglog, count, buff);
	rc = simple_read_from_buffer(buf, count, ppos, buff, strlen(buff));

	kfree(buff);

	return rc;
}

BATTERY_DEBUG_ATTRIBUTE(debug_reglog_writes_fops,
			debug_get_reglog_writes, NULL);

static ssize_t max1720x_show_custom_model(struct file *filp, char __user *buf,
					  size_t count, loff_t *ppos)
{
	struct max1720x_chip *chip = (struct max1720x_chip *)filp->private_data;
	char *tmp;
	int len;

	if (!chip->model_data)
		return -EINVAL;

	tmp = kmalloc(PAGE_SIZE, GFP_KERNEL);
	if (!tmp)
		return -ENOMEM;

	mutex_lock(&chip->model_lock);
	len = max_m5_fg_model_cstr(tmp, PAGE_SIZE, chip->model_data);
	mutex_unlock(&chip->model_lock);

	if (len > 0)
		len = simple_read_from_buffer(buf, count,  ppos, tmp, len);

	kfree(tmp);

	return len;
}

static ssize_t max1720x_set_custom_model(struct file *filp,
					 const char __user *user_buf,
					 size_t count, loff_t *ppos)
{
	struct max1720x_chip *chip = (struct max1720x_chip *)filp->private_data;
	char *tmp;
	int ret;

	if (!chip->model_data)
		return -EINVAL;

	tmp = kmalloc(PAGE_SIZE, GFP_KERNEL);
	if (!tmp)
		return -ENOMEM;

	ret = simple_write_to_buffer(tmp, PAGE_SIZE, ppos, user_buf, count);
	if (!ret) {
		kfree(tmp);
		return -EFAULT;
	}

	mutex_lock(&chip->model_lock);
	ret = max_m5_fg_model_sscan(chip->model_data, tmp, count);
	if (ret < 0)
		count = ret;
	mutex_unlock(&chip->model_lock);

	kfree(tmp);

	return count;
}

BATTERY_DEBUG_ATTRIBUTE(debug_m5_custom_model_fops, max1720x_show_custom_model,
			max1720x_set_custom_model);


static int debug_sync_model(void *data, u64 val)
{
	struct max1720x_chip *chip = data;
	int ret;

	if (!chip->model_data)
		return -EINVAL;

	/* re-read new state from Fuel gauge, save to storage  */
	ret = max_m5_model_read_state(chip->model_data);
	if (ret == 0) {
		ret = max_m5_model_check_state(chip->model_data);
		if (ret < 0)
			pr_warn("%s: warning invalid state %d\n", __func__, ret);

		ret = max_m5_save_state_data(chip->model_data);
	}

	return ret;
}

DEFINE_SIMPLE_ATTRIBUTE(debug_sync_model_fops, NULL, debug_sync_model, "%llu\n");


static ssize_t max1720x_show_debug_data(struct file *filp, char __user *buf,
					size_t count, loff_t *ppos)
{
	struct max1720x_chip *chip = (struct max1720x_chip *)filp->private_data;
	char msg[8];
	u16 data;
	int ret;

	ret = REGMAP_READ(&chip->regmap, chip->debug_reg_address, &data);
	if (ret < 0)
		return ret;

	ret = scnprintf(msg, sizeof(msg), "%x\n", data);

	return simple_read_from_buffer(buf, count, ppos, msg, ret);
}

static ssize_t max1720x_set_debug_data(struct file *filp,
				       const char __user *user_buf,
				       size_t count, loff_t *ppos)
{
	struct max1720x_chip *chip = (struct max1720x_chip *)filp->private_data;
	char temp[8] = { };
	u16 data;
	int ret;

	ret = simple_write_to_buffer(temp, sizeof(temp) - 1, ppos, user_buf, count);
	if (!ret)
		return -EFAULT;

	ret = kstrtou16(temp, 16, &data);
	if (ret < 0)
		return ret;

	ret =  REGMAP_WRITE(&chip->regmap, chip->debug_reg_address, data);
	if (ret < 0)
		return ret;

	return count;
}

BATTERY_DEBUG_ATTRIBUTE(debug_reg_data_fops, max1720x_show_debug_data,
			max1720x_set_debug_data);


/*
 * TODO: add the building blocks of google capacity
 *
 * case POWER_SUPPLY_PROP_DELTA_CC_SUM:
 *	val->intval = chip->cap_estimate.delta_cc_sum;
 *	break;
 * case POWER_SUPPLY_PROP_DELTA_VFSOC_SUM:
 *	val->intval = chip->cap_estimate.delta_vfsoc_sum;
 *	break;
 */

static int max17x0x_init_debugfs(struct max1720x_chip *chip)
{
	struct dentry *de;

	de = debugfs_create_dir(chip->max1720x_psy_desc.name, 0);
	if (IS_ERR_OR_NULL(de))
		return -ENOENT;

	debugfs_create_file("irq_none_cnt", 0644, de, chip, &irq_none_cnt_fops);
	debugfs_create_file("nvram_por", 0440, de, chip, &debug_nvram_por_fops);
	debugfs_create_file("fg_reset", 0400, de, chip, &debug_fg_reset_fops);
	debugfs_create_file("ce_start", 0400, de, chip, &debug_ce_start_fops);
	debugfs_create_file("batt_id", 0600, de, chip, &debug_batt_id_fops);

	if (chip->regmap.reglog)
		debugfs_create_file("regmap_writes", 0440, de,
					chip->regmap.reglog,
					&debug_reglog_writes_fops);

	if (chip->regmap_nvram.reglog)
		debugfs_create_file("regmap_nvram_writes", 0440, de,
					chip->regmap_nvram.reglog,
					&debug_reglog_writes_fops);

	if (chip->gauge_type == MAX_M5_GAUGE_TYPE)
		debugfs_create_file("fg_model", 0440, de, chip,
				    &debug_m5_custom_model_fops);
	debugfs_create_bool("model_ok", 0444, de, &chip->model_ok);
	debugfs_create_file("sync_model", 0400, de, chip,
			    &debug_sync_model_fops);

	/* capacity drift fixup, one of MAX1720X_DA_VER_* */
	debugfs_create_u32("algo_ver", 0644, de, &chip->drift_data.algo_ver);

	/* new debug interface */
	debugfs_create_u32("address", 0600, de, &chip->debug_reg_address);
	debugfs_create_file("data", 0600, de, chip, &debug_reg_data_fops);

	return 0;
}

static u16 max1720x_read_rsense(const struct max1720x_chip *chip)
{
	u32 rsense_default = 500;
	int dt_rsense, ret;
	u16 rsense = 0;

	ret = of_property_read_u32(chip->dev->of_node, "maxim,rsense-default",
				   &rsense_default);
	dt_rsense = ret == 0;

	/* read from NVRAM if present */
	if (chip->regmap_nvram.regmap) {

		ret = REGMAP_READ(&chip->regmap_nvram, MAX1720X_NRSENSE, &rsense);
		if (ret == 0 && dt_rsense && rsense != rsense_default) {
			dev_warn(chip->dev, "RSense %d, forcing to %d uOhm\n",
				 rsense * 10, rsense_default * 10);

			rsense = rsense_default;
		}
	}

	if (!rsense)
		rsense = rsense_default;

	return rsense;
}

/*
 * The BCNT, QHQH and QHCA for 17202 modify these common registers
 * 0x18C, 0x18D, 0x18E, 0x18F, 0x1B2, 0x1B4 which will default to 0 after
 * the recovery procedure. The map changes the content of 0x1C4, 0x1C5 which
 * are not used (and should be 0 unless used by a future version of SW) as well
 * as 0x1CD, 0x1CE used for nManfctrName1/2 which might change. 0x1DF is the
 * INI checksum that will change with updates and cannot be used for this test.
 * The only register left is 0x1D7 (MAX1730X_NPROTCFG), that is the register
 * that the code will used to determine the corruption.
 */
static int max1730x_check_prot(struct max1720x_chip *chip, u16 devname)
{
	int needs_recall = 0;
	u16 nprotcfg;
	int ret;

	/* check protection registers */
	ret = REGMAP_READ(&chip->regmap_nvram, MAX1730X_NPROTCFG, &nprotcfg);
	if (ret < 0)
		return -EIO;

	if (devname == MAX1730X_GAUGE_PASS1)
		needs_recall = (nprotcfg != MAX1730X_NPROTCFG_PASS1);
	else /* pass2 */
		needs_recall = (nprotcfg != MAX1730X_NPROTCFG_PASS2);

	return needs_recall;
}

static int max17x0x_nvram_recall(struct max1720x_chip *chip)
{
	REGMAP_WRITE(&chip->regmap,
			MAX17XXX_COMMAND,
			MAX17XXX_COMMAND_NV_RECALL);
	msleep(MAX17X0X_TPOR_MS);
	return 0;
}

/* TODO: move to google/max1730x_battery.c, enable with build config */
static int max1730x_fixups(struct max1720x_chip *chip)
{
	int ret;

	struct device_node *node = chip->dev->of_node;
	bool write_back = false, write_ndata = false;
	const u16 val = 0x280B;
	u16 ndata = 0, bak = 0;

	/* b/123026365 */
	if (of_property_read_bool(node, "maxim,enable-nv-check")) {
		const u16 devname = (chip->devname >> 4);
		int needs_recall;

		needs_recall = max1730x_check_prot(chip, devname);
		if (needs_recall < 0) {
			dev_err(chip->dev, "error reading fg NV configuration\n");
		} else if (needs_recall == 0) {
			/* all good.. */
		} else if (devname == MAX1730X_GAUGE_PASS1) {
			dev_err(chip->dev, "***********************************************\n");
			dev_err(chip->dev, "WARNING: need to restore FG NV configuration to\n");
			dev_err(chip->dev, "default values. THE DEVICE WILL LOOSE POWER.\n");
			dev_err(chip->dev, "*******************************************\n");
			msleep(MSEC_PER_SEC);
			REGMAP_WRITE(&chip->regmap, MAX17XXX_COMMAND,
				     MAX1720X_COMMAND_HARDWARE_RESET);
			msleep(MAX17X0X_TPOR_MS);
		} else {
			dev_err(chip->dev, "Restoring FG NV configuration to sane values\n");

			if (!chip->needs_reset) {
				ret = max17x0x_nvram_recall(chip);
				if (ret == 0)
					chip->needs_reset = true;
			}

			REGMAP_WRITE(&chip->regmap_nvram,
					MAX1730X_NPROTCFG,
					MAX1730X_NPROTCFG_PASS2);
		}
	} else {
		dev_err(chip->dev, "nv-check disabled\n");
	}

	/* b/122605202 */
	ret = REGMAP_READ(&chip->regmap_nvram, MAX1730X_NVPRTTH1, &ndata);
	if (ret == 0)
		ret = REGMAP_READ(&chip->regmap, MAX1730X_NVPRTTH1BAK, &bak);
	if (ret < 0)
		return -EIO;

	if (ndata == MAX1730X_NVPRTTH1_CHARGING)
		write_back = (bak != val);
	else
		write_ndata = (ndata != val);

	if (write_back || write_ndata) {
		ret = REGMAP_WRITE(&chip->regmap,
				   MAX1730X_NVPRTTH1BAK, val);
		if (ret == 0)
			ret = REGMAP_WRITE(&chip->regmap_nvram,
					   MAX1730X_NVPRTTH1, val);
		if (ret == 0)
			ret = REGMAP_WRITE(&chip->regmap,
					   MAX1730X_NVPRTTH1BAK, val);
	}

	if (ret < 0)
		dev_info(chip->dev, "failed to update 0x0D6=%x 0x1D0=%x to %x (%d)\n",
			 bak, ndata, val, ret);
	else if (write_back)
		dev_info(chip->dev, "0x0D6=%x 0x1D0=%x updated to %x (%d)\n",
			 bak, ndata, val, ret);
	else if (write_ndata)
		dev_info(chip->dev, "0x1D0=%x updated to %x (%d)\n",
			ndata, val, ret);

	return ret;
}

static int max17x0x_dump_param(struct max1720x_chip *chip)
{
	int ret;
	u16 data;

	ret = max17x0x_reg_read(&chip->regmap, MAX17X0X_TAG_cnfg,
				&chip->RConfig);
	if (ret < 0)
		return ret;

	dev_info(chip->dev, "Config: 0x%04x\n", chip->RConfig);

	ret = REGMAP_READ(&chip->regmap, MAX1720X_ICHGTERM, &data);
	if (ret < 0)
		return ret;

	dev_info(chip->dev, "IChgTerm: %d\n",
		 reg_to_micro_amp(data, chip->RSense));

	ret = REGMAP_READ(&chip->regmap, MAX1720X_VEMPTY, &data);
	if (ret < 0)
		return ret;

	dev_info(chip->dev, "VEmpty: VE=%dmV VR=%dmV\n",
		 reg_to_vempty(data), reg_to_vrecovery(data));

	return 0;
}

static int max1720x_clear_por(struct max1720x_chip *chip)
{
	u16 data;
	int ret;

	ret = REGMAP_READ(&chip->regmap, MAX1720X_STATUS, &data);
	if (ret < 0)
		return ret;

	if ((data & MAX1720X_STATUS_POR) == 0)
		return 0;

	return regmap_update_bits(chip->regmap.regmap,
				  MAX1720X_STATUS,
				  MAX1720X_STATUS_POR,
				  0x0);
}

/* read state from fg (if needed) and set the next update field */
static int max1720x_set_next_update(struct max1720x_chip *chip)
{
	int rc, cycle_count;

	cycle_count = max1720x_get_cycle_count(chip);
	if (cycle_count < 0)
		return cycle_count;

	if (chip->model_next_update && cycle_count < chip->model_next_update)
		return 0;

	/* read new state from Fuel gauge, save to storage if needed */
	rc = max_m5_model_read_state(chip->model_data);
	if (rc == 0) {
		rc = max_m5_model_check_state(chip->model_data);
		if (rc < 0) {
			pr_debug("%s: fg model state is corrupt rc=%d\n",
				 __func__, rc);
			return -EINVAL;
		}
	}

	if (rc == 0 && chip->model_next_update)
		rc = max_m5_save_state_data(chip->model_data);
	if (rc == 0)
		chip->model_next_update = (cycle_count + (1 << 6)) &
					  ~((1 << 6) - 1);

	pr_debug("%s: cycle_count=%d next_update=%d rc=%d\n", __func__,
		 cycle_count, chip->model_next_update, rc);

	return 0;
}

static int max1720x_model_load(struct max1720x_chip *chip)
{
	int ret;

	/* retrieve model state from permanent storage only on boot */
	if (!chip->model_state_valid) {

		/*
		 * retrieve state from storage: retry on -EAGAIN as long as
		 * model_reload > _IDLE
		 */
		ret = max_m5_load_state_data(chip->model_data);
		if (ret == -EAGAIN)
			return -EAGAIN;
		if (ret < 0)
			dev_warn(chip->dev, "Load Model Using Default State (%d)\n",
				ret);

		/* use the state from the DT when GMSR is invalid */
	}

	/* failure on the gauge: retry as long as model_reload > IDLE */
	ret = max_m5_load_gauge_model(chip->model_data);
	if (ret < 0) {
		dev_err(chip->dev, "Load Model Failed ret=%d\n", ret);
		return -EAGAIN;
	}

	/* fix capacity outliers algo */
	ret = max_m5_fixup_outliers(&chip->drift_data, chip->model_data);
	if (ret < 0)
		dev_err(chip->dev, "Load Model fixing drift data rc=%d\n", ret);

	/* The caller need to call max1720x_set_next_update() */

	/* mark model state as "safe" */
	chip->reg_prop_capacity_raw = MAX1720X_REPSOC;
	chip->model_state_valid = true;
	return 0;
}

static void max1720x_model_work(struct work_struct *work)
{
	struct max1720x_chip *chip = container_of(work, struct max1720x_chip,
						  model_work.work);
	bool new_model = false;
	int rc;

	if (!chip->model_data)
		return;

	mutex_lock(&chip->model_lock);

	/* set model_reload to the #attempts, might change cycle count */
	if (chip->model_reload >= MAX_M5_LOAD_MODEL_REQUEST) {

		rc = max1720x_model_load(chip);
		if (rc == 0) {
			rc = max1720x_clear_por(chip);

			dev_info(chip->dev, "Model OK, Clear Power-On Reset (%d)\n",
				 rc);

			/* TODO: keep trying to clear POR if the above fail */
			chip->model_reload = MAX_M5_LOAD_MODEL_IDLE;
			chip->model_ok = true;
			new_model = true;
		} else if (rc != -EAGAIN) {
			chip->model_reload = MAX_M5_LOAD_MODEL_DISABLED;
			chip->model_ok = false;
		} else if (chip->model_reload > MAX_M5_LOAD_MODEL_IDLE) {
			chip->model_reload -= 1;
		}
	}

	/* b/171741751, fix capacity drift (if POR is cleared) */
	if (max1720x_check_drift_enabled(&chip->drift_data))
		max1720x_fixup_capacity(chip, chip->cap_estimate.cable_in);

	/* save state only when model is running */
	if (chip->model_ok) {
		rc = max1720x_set_next_update(chip);
		if (rc < 0)
			dev_err(chip->dev, "%s cannot set next update (%d)\n",
				 __func__, rc);
	}

	if (chip->model_reload >= MAX_M5_LOAD_MODEL_REQUEST) {
		const unsigned long delay = msecs_to_jiffies(60 * 1000);

		schedule_delayed_work(&chip->model_work, delay);
	}

	if (new_model) {
		dev_info(chip->dev, "FG Model OK, ver=%d cap_lsb=%d next_update=%d\n",
			 max_m5_fg_model_version(chip->model_data),
			 max_m5_cap_lsb(chip->model_data),
			 chip->model_next_update);
		power_supply_changed(chip->psy);
	}

	mutex_unlock(&chip->model_lock);
}

static int read_chip_property_u32(const struct max1720x_chip *chip,
				  char *property, u32 *data32)
{
	int ret;

	if (chip->batt_node) {
		ret = of_property_read_u32(chip->batt_node, property, data32);
		if (ret == 0)
			return ret;
	}

	return of_property_read_u32(chip->dev->of_node, property, data32);
}

/* fix capacity drift after loading the model */
static int max17201_init_fix_capacity(struct max1720x_chip *chip)
{
	struct max1720x_drift_data *ddata = &chip->drift_data;
	u32 data32 = 0;
	u16 data16;
	int ret;

	ret = gbms_storage_read(GBMS_TAG_CMPC, &data16, sizeof(data16));
	if (ret == -EPROBE_DEFER)
		return -EPROBE_DEFER;
	if (ret == 0)
		chip->comp_update_count = data16;
	else
		chip->comp_update_count = 0;

	ret = gbms_storage_read(GBMS_TAG_DXAC, &data16, sizeof(data16));
	if (ret == -EPROBE_DEFER)
		return -EPROBE_DEFER;
	if (ret == 0)
		chip->dxacc_update_count = data16;
	else
		chip->dxacc_update_count = 0;

	/* device dependent values */
	ddata->rsense = chip->RSense;
	/* update design_capacity after loading the model if not set in dt */
	ret = of_property_read_u32(chip->dev->of_node, "maxim,capacity-design",
				   &data32);
	if (ret < 0) {
		ddata->design_capacity = -1;
	} else if (data32 != 0) {
		ddata->design_capacity = data32;
	} else if (chip->regmap_nvram.regmap) {
		/*
		 * TODO: read design capacity from NVRAM if available
		 * ret = REGMAP_READ(&chip->regmap_nvram, MAX1720X_NDESIGNCAP,
		 *		  &ddata->design_capacity);
		 */
		ret = REGMAP_READ(&chip->regmap, MAX1720X_DESIGNCAP,
				  &ddata->design_capacity);
		if (ret < 0)
			return -EPROBE_DEFER;

		/* add retries? */
	}

	/*
	 * chemistry dependent codes:
	 * NOTE: ->batt_node is initialized in *_handle_dt_shadow_config
	 */
	ret = read_chip_property_u32(chip, "maxim,capacity-rcomp0", &data32);
	if (ret < 0)
		ddata->ini_rcomp0 = -1;
	else
		ddata->ini_rcomp0 = data32;

	ret = read_chip_property_u32(chip, "maxim,capacity-tempco", &data32);
	if (ret < 0)
		ddata->ini_tempco = -1;
	else
		ddata->ini_tempco = data32;

	ret = of_property_read_u32(chip->dev->of_node, "maxim,capacity-stable",
				   &data32);
	if (ret < 0)
		ddata->cycle_stable = BATTERY_DEFAULT_CYCLE_STABLE;
	else
		ddata->cycle_stable = data32;

	ret = of_property_read_u32(chip->dev->of_node, "maxim,capacity-fade",
				   &data32);
	if (ret < 0)
		ddata->cycle_fade = BATTERY_DEFAULT_CYCLE_FADE;
	else
		ddata->cycle_fade = data32;

	ret = of_property_read_u32(chip->dev->of_node, "maxim,capacity-band",
				   &data32);
	if (ret < 0) {
		ddata->cycle_band = BATTERY_DEFAULT_CYCLE_BAND;
	} else {
		ddata->cycle_band = data32;
		if (ddata->cycle_band > BATTERY_MAX_CYCLE_BAND)
			ddata->cycle_band = BATTERY_MAX_CYCLE_BAND;
	}

	/*
	 * Set to force loading the model with corresponding algo-version.
	 * MW A0+ MW-A0 should use MAX1720X_DA_VER_ORIG while and MW-A1 should
	 * use MAX1720X_DA_VER_MWA1 for RC1 or MAX1720X_DA_VER_NONE for RC2.
	 * MW-A2 should use MAX1720X_DA_VER_NONE for RC1 and RC2.
	 * Not used for max1720x and max1730x.
	 */
	if (max_m5_check_devname(chip->devname) ) {
		ret = of_property_read_u32(chip->dev->of_node,
					   "maxim,algo-version",
					   &data32);
		if (ret < 0 || data32 > MAX1720X_DA_VER_MWA2)
			ddata->algo_ver = MAX1720X_DA_VER_NONE;
		else
			ddata->algo_ver = data32;
	} else {
		ddata->algo_ver = MAX1720X_DA_VER_ORIG;
	}

	ret = read_chip_property_u32(chip, "maxim,capacity-filtercfg", &data32);
	if (ret < 0)
		ddata->ini_filtercfg = -1;
	else
		ddata->ini_filtercfg = data32;

	if (ddata->ini_filtercfg != -1)
		dev_info(chip->dev, "ini_filtercfg=0x%x\n",
			 ddata->ini_filtercfg);

	return 0;
}

/* handle recovery of FG state */
static int max1720x_init_max_m5(struct max1720x_chip *chip)
{
	int ret;

	/* TODO add retries */
	ret = max_m5_model_read_state(chip->model_data);
	if (ret < 0) {
		dev_err(chip->dev, "FG Model Error (%d)\n", ret);
		return -EPROBE_DEFER;
	}

	ret = max_m5_model_check_state(chip->model_data);
	if (ret < 0) {
		ret = max1720x_full_reset(chip);
		if (ret == 0)
			ret = max_m5_model_read_state(chip->model_data);

		dev_err(chip->dev, "FG State Corrupt, Reset (%d), Will reload\n",
			ret);
		return 0;
	}

	if (!max_m5_fg_model_check_version(chip->model_data)) {
		ret = max1720x_full_reset(chip);
		if (ret == 0)
			ret = max_m5_model_read_state(chip->model_data);

		/* TODO: invalidate GSMR data when switching from RC2->RC1 */

		dev_warn(chip->dev, "FG Version Changed, Reset (%d), Will Reload\n",
			 ret);
		return 0;
	}

	ret = max1720x_set_next_update(chip);
	if (ret < 0)
		dev_warn(chip->dev, "Error on Next Update, Will retry\n");

	dev_info(chip->dev, "FG Model OK, ver=%d cap_lsb=%d next_update=%d\n",
			max_m5_model_read_version(chip->model_data),
			max_m5_cap_lsb(chip->model_data),
			chip->model_next_update);

	chip->reg_prop_capacity_raw = MAX1720X_REPSOC;
	chip->model_state_valid = true;
	chip->model_ok = true;
	return 0;
}


static int max1720x_init_chip(struct max1720x_chip *chip)
{
	int ret;
	u8 vreg, vpor;
	u16 data = 0, tmp;
	bool por = false, force_recall = false;

	if (of_property_read_bool(chip->dev->of_node, "maxim,force-hard-reset"))
		max1720x_full_reset(chip);

	ret = REGMAP_READ(&chip->regmap, MAX1720X_STATUS, &data);
	if (ret < 0)
		return -EPROBE_DEFER;
	por = (data & MAX1720X_STATUS_POR) != 0;
	if (por && chip->regmap_nvram.regmap) {
		dev_err(chip->dev, "Recall: POR bit is set\n");
		force_recall = true;
	}

	/* TODO: disable with maxim,fix-ignore-zero-rsense */
	chip->RSense = max1720x_read_rsense(chip);
	if (chip->RSense == 0) {
		dev_err(chip->dev, "Recall: RSense value 0 micro Ohm\n");
		force_recall = true;
	}

	/* read por force recall and reset when version is the por */
	ret = max17x0x_read_dt_version_por(chip->dev->of_node,
					   chip->gauge_type, &vreg, &vpor);
	if (ret == 0) {
		ret = REGMAP_READ(&chip->regmap_nvram, vreg, &tmp);
		if ((ret == 0) && (vpor == (tmp & 0x00ff))) {
			dev_err(chip->dev, "Recall: POR version %d\n", vpor);
			force_recall = true;
		}
	}

	/* b/129384855 fix mismatch between pack INI file and overrides */
	if (of_property_read_bool(chip->dev->of_node, "maxim,fix-vempty")) {
		ret = REGMAP_READ(&chip->regmap, MAX1720X_VEMPTY, &data);
		if ((ret == 0) && (reg_to_vrecovery(data) == 0)) {
			dev_err(chip->dev, "Recall: zero vrecovery\n");
			force_recall = true;
		}
	}

	if (force_recall && chip->regmap_nvram.regmap) {
		/* debug only */
		ret = max17x0x_nvram_cache_init(&chip->nRAM_por,
						chip->gauge_type);
		if (ret == 0)
			ret = max17x0x_cache_load(&chip->nRAM_por,
						  &chip->regmap_nvram);
		if (ret < 0) {
			dev_err(chip->dev, "POR: Failed to backup config\n");
			return -EPROBE_DEFER;
		}

		dev_info(chip->dev, "Recall Battery NVRAM\n");
		ret = max17x0x_nvram_recall(chip);
		if (ret == 0)
			chip->needs_reset = true;

		/* TODO: enable with maxim,fix-nagefccfg */
		if (chip->gauge_type == MAX1720X_GAUGE_TYPE)
			REGMAP_WRITE(&chip->regmap_nvram,
				     MAX1720X_NAGEFCCFG, 0);
	}

	/* device dependent fixups to the registers */
	if (chip->fixups_fn) {
		ret = chip->fixups_fn(chip);
		if (ret < 0) {
			dev_err(chip->dev, "Fixups failed (%d)\n", ret);
			return ret;
		}
	}

	/* set maxim,force-batt-id in DT to not delay the probe */
	ret = max1720x_read_batt_id(&chip->batt_id, chip);
	if (ret == -EPROBE_DEFER) {
		if (chip->batt_id_defer_cnt) {
			chip->batt_id_defer_cnt -= 1;
			return -EPROBE_DEFER;
		}

		chip->batt_id = DEFAULT_BATTERY_ID;
		dev_info(chip->dev, "default device battery ID = %d\n",
			 chip->batt_id);
	} else {
		dev_info(chip->dev, "device battery RID: %d kohm\n",
			 chip->batt_id);
	}

	/* fuel gauge model needs to know the batt_id */
	mutex_init(&chip->model_lock);

	/*
	 * The behavior of the drift workaround changes with the capacity
	 * learning algo used in the part. Integrated FG might have
	 * configurable capacity learning.
	 */
	ret = max17201_init_fix_capacity(chip);
	if (ret < 0)
		dev_err(chip->dev, "Capacity drift WAR not enabled(%d)\n", ret);
	/*
	 * FG model is ony used for integrated FG (MW). Loading a model might
	 * change the capacity drift WAR algo_ver and design_capacity.
	 * NOTE: design_capacity used for drift might be updated after loading
	 * a FG model.
	 */
	ret = max1720x_init_model(chip);
	if (ret < 0)
		dev_err(chip->dev, "Cannot init FG model (%d)\n", ret);

	/* dump capacity drift fixup configuration only when enabled */
	if (chip->drift_data.algo_ver != MAX1720X_DA_VER_NONE) {
		struct max1720x_drift_data *ddata = &chip->drift_data;

		dev_info(chip->dev, "ver=%d rsns=%d cnts=%d,%d dc=%d cap_sta=%d cap_fad=%d rcomp0=0x%x tempco=0x%x\n",
			 ddata->algo_ver, ddata->rsense,
			 chip->comp_update_count, chip->dxacc_update_count,
			 ddata->design_capacity, ddata->cycle_stable,
			 ddata->cycle_fade, ddata->ini_rcomp0,
			 ddata->ini_tempco);
	}

	/* not needed for FG with NVRAM */
	ret = max17x0x_handle_dt_shadow_config(chip);
	if (ret == -EPROBE_DEFER)
		return ret;

	ret = max17x0x_handle_dt_register_config(chip);
	if (ret == -EPROBE_DEFER)
		return ret;

	(void) max1720x_handle_dt_nconvgcfg(chip);

	/* recall, force & reset SW */
	if (chip->needs_reset) {
		max17x0x_fg_reset(chip);

		if (chip->RSense == 0)
			chip->RSense = max1720x_read_rsense(chip);
	}

	ret = max17x0x_dump_param(chip);
	if (ret < 0)
		return -EPROBE_DEFER;
	dev_info(chip->dev, "RSense value %d micro Ohm\n", chip->RSense * 10);

	ret = REGMAP_READ(&chip->regmap, MAX1720X_STATUS, &data);
	if (!ret && data & MAX1720X_STATUS_BR) {
		dev_info(chip->dev, "Clearing Battery Removal bit\n");
		regmap_update_bits(chip->regmap.regmap, MAX1720X_STATUS,
				   MAX1720X_STATUS_BR, 0x0);
	}
	if (!ret && data & MAX1720X_STATUS_BI) {
		dev_info(chip->dev, "Clearing Battery Insertion bit\n");
		regmap_update_bits(chip->regmap.regmap, MAX1720X_STATUS,
				   MAX1720X_STATUS_BI, 0x0);
	}

	/* max_m5 triggers loading of the model in the irq handler on POR */
	if (!por && chip->gauge_type == MAX_M5_GAUGE_TYPE) {
		ret = max1720x_init_max_m5(chip);
		if (ret < 0)
			return ret;
	} else if (por && chip->gauge_type != MAX_M5_GAUGE_TYPE) {
		ret = regmap_update_bits(chip->regmap.regmap,
					 MAX1720X_STATUS,
					 MAX1720X_STATUS_POR, 0x0);
		dev_info(chip->dev, "Clearing Power-On Reset bit (%d)\n", ret);
		chip->reg_prop_capacity_raw = MAX1720X_REPSOC;
	}


	max1720x_restore_battery_qh_capacity(chip);

	return 0;
}

static int max1730x_decode_sn(char *serial_number,
			      unsigned int max,
			      const u16 *data)
{
	int tmp, count = 0;

	if (data[0] != 0x4257)	/* "BW": DSY */
		return -EINVAL;

	count += scnprintf(serial_number + count, max - count, "%02X%02X%02X",
			   data[3] & 0xFF,
			   data[4] & 0xFF,
			   data[5] & 0xFF);

	tmp = (((((data[1] >> 9) & 0x3f) + 1980) * 10000) +
		((data[1] >> 5) & 0xf) * 100 + (data[1] & 0x1F));
	count += scnprintf(serial_number + count, max - count, "%d",
			   tmp);

	count += scnprintf(serial_number + count, max - count, "%c%c",
			   data[0] >> 8, data[0] & 0xFF);

	count += scnprintf(serial_number + count, max - count, "%c%c%c%c",
			   data[7] >> 8,
			   data[8] >> 8,
			   data[9] >> 8,
			   data[9] & 0xFF);

	count += scnprintf(serial_number + count, max - count, "%c",
			   data[2] & 0xFF);

	count += scnprintf(serial_number + count, max - count, "%c%c",
			   data[6] >> 8, data[6] & 0xFF);
	return 0;
}

static int max1720x_decode_sn(char *serial_number,
			      unsigned int max,
			      const u16 *data)
{
	int tmp, count = 0, shift;
	char cell_vendor;

	if (data[0] == 0x5357) /* "SW": SWD */
		shift = 0;
	else if (data[0] == 0x4257) /* "BW": DSY */
		shift = 8;
	else
		return -EINVAL;

	count += scnprintf(serial_number + count, max - count, "%02X%02X%02X",
			   data[1] >> shift,
			   data[2] >> shift,
			   data[3] >> shift);

	tmp = (((((data[4] >> 9) & 0x3f) + 1980) * 10000) +
		((data[4] >> 5) & 0xf) * 100 + (data[4] & 0x1F));
	count += scnprintf(serial_number + count, max - count, "%d",
			   tmp);

	count += scnprintf(serial_number + count, max - count, "%c%c",
			   data[0] >> 8,
			   data[0] & 0xFF);

	count += scnprintf(serial_number + count, max - count, "%c%c%c",
			   data[5] >> shift,
			   data[6] >> shift,
			   data[7] >> shift);

	tmp = data[8];
	if (tmp >> 8 == 0)
		tmp = ('?' << 8) | (tmp & 0xFF);
	if ((tmp & 0xFF) == 0)
		tmp = (tmp & 0xFF00) | '?';
	count += scnprintf(serial_number + count, max - count, "%c%c",
			   tmp >> 8,
			   tmp & 0xFF);

	cell_vendor = (shift == 8) ? (data[9] >> 8) : (data[9] & 0xFF);
	count += scnprintf(serial_number + count, max - count, "%c",
			   cell_vendor);

	if (shift == 8) {
		count += scnprintf(serial_number + count, max - count, "%02X",
				   data[10] >> 8);
	} else {
		count += scnprintf(serial_number + count, max - count, "%c%c",
				   data[10] >> 8, data[10] & 0xFF);
	}

	return 0;
}

static void *ct_seq_start(struct seq_file *s, loff_t *pos)
{
	struct max1720x_history *hi = (struct max1720x_history *)s->private;

	if (*pos >= hi->history_count)
		return NULL;
	hi->history_index = *pos;

	return &hi->history_index;
}

static void *ct_seq_next(struct seq_file *s, void *v, loff_t *pos)
{
	loff_t *spos = (loff_t *)v;
	struct max1720x_history *hi = (struct max1720x_history *)s->private;

	*pos = ++*spos;
	if (*pos >= hi->history_count)
		return NULL;

	return spos;
}

static void ct_seq_stop(struct seq_file *s, void *v)
{
	/* iterator in hi, no need to free */
}

static int ct_seq_show(struct seq_file *s, void *v)
{
	char temp[96];
	loff_t *spos = (loff_t *)v;
	struct max1720x_history *hi = (struct max1720x_history *)s->private;
	const size_t offset = *spos * hi->page_size;

	format_battery_history_entry(temp, sizeof(temp),
					hi->page_size, &hi->history[offset]);
	seq_printf(s, "%s\n", temp);

	return 0;
}

static const struct seq_operations ct_seq_ops = {
	.start = ct_seq_start,
	.next  = ct_seq_next,
	.stop  = ct_seq_stop,
	.show  = ct_seq_show
};

static int history_dev_open(struct inode *inode, struct file *file)
{
	struct max1720x_chip *chip =
		container_of(inode->i_cdev, struct max1720x_chip, hcdev);
	struct max1720x_history *hi;
	int history_count;

	hi = __seq_open_private(file, &ct_seq_ops, sizeof(*hi));
	if (!hi)
		return -ENOMEM;

	mutex_lock(&chip->history_lock);
	history_count = max1720x_history_read(chip, hi);
	if (history_count < 0) {
		mutex_unlock(&chip->history_lock);
		return history_count;
	} else if (history_count == 0) {
		dev_info(chip->dev, "No battery history has been recorded\n");
	}
	mutex_unlock(&chip->history_lock);

	return 0;
}

static int history_dev_release(struct inode *inode, struct file *file)
{
	struct max1720x_history *hi =
		((struct seq_file *)file->private_data)->private;

	if (hi) {
		max1720x_history_free(hi);
		seq_release_private(inode, file);
	}

	return 0;
}

static const struct file_operations hdev_fops = {
	.open = history_dev_open,
	.owner = THIS_MODULE,
	.read = seq_read,
	.release = history_dev_release,
};

static void max1720x_cleanup_history(struct max1720x_chip *chip)
{
	if (chip->history_added)
		cdev_del(&chip->hcdev);
	if (chip->history_available)
		device_destroy(chip->hcclass, chip->hcmajor);
	if (chip->hcclass)
		class_destroy(chip->hcclass);
	if (chip->hcmajor != -1)
		unregister_chrdev_region(chip->hcmajor, 1);
}

static int max1720x_init_history_device(struct max1720x_chip *chip)
{
	struct device *hcdev;

	mutex_init(&chip->history_lock);

	chip->hcmajor = -1;

	/* cat /proc/devices */
	if (alloc_chrdev_region(&chip->hcmajor, 0, 1, HISTORY_DEVICENAME) < 0)
		goto no_history;
	/* ls /sys/class */
	chip->hcclass = class_create(THIS_MODULE, HISTORY_DEVICENAME);
	if (chip->hcclass == NULL)
		goto no_history;
	/* ls /dev/ */
	hcdev = device_create(chip->hcclass, NULL, chip->hcmajor, NULL,
		HISTORY_DEVICENAME);
	if (hcdev == NULL)
		goto no_history;

	chip->history_available = true;
	cdev_init(&chip->hcdev, &hdev_fops);
	if (cdev_add(&chip->hcdev, chip->hcmajor, 1) == -1)
		goto no_history;

	chip->history_added = true;
	return 0;

no_history:
	max1720x_cleanup_history(chip);
	return -ENODEV;
}

static int max1720x_init_history(struct max1720x_chip *chip)
{
	if (chip->gauge_type == MAX1730X_GAUGE_TYPE) {
		chip->nb_history_pages = MAX1730X_N_OF_HISTORY_PAGES;
		chip->history_page_size = MAX1730X_HISTORY_PAGE_SIZE;
		chip->nb_history_flag_reg = MAX1730X_N_OF_HISTORY_FLAGS_REG;
	} else if (chip->gauge_type == MAX1720X_GAUGE_TYPE) {
		chip->nb_history_pages = MAX1720X_N_OF_HISTORY_PAGES;
		chip->history_page_size = MAX1720X_HISTORY_PAGE_SIZE;
		chip->nb_history_flag_reg = MAX1720X_N_OF_HISTORY_FLAGS_REG;
	} else {
		return -EINVAL;
	}

	return 0;
}

/* ------------------------------------------------------------------------- */

static int max17x0x_storage_info(gbms_tag_t tag, size_t *addr, size_t *count,
				 void *ptr)
{
	struct max1720x_chip *chip = (struct max1720x_chip *)ptr;

	if (!chip->history_available)
		return -ENOENT;

	*count = chip->history_page_size * 2; /* storage is in byte */
	*addr = -1;
	return 0;
}

/*
 * The standard device call this with !data && !size && index=0 on start and
 * !data && !size && index<0 on stop. The call on start free and reload the
 * history from the gauge potentially increasing the number of entries (note
 * clients will not see that until they call start). On close the code just
 * release the allocated memory and entries: this is not a problem for cliets
 * that might be open because the data will be reloaded on next access.
 * This might create some churn but it's ok since we should not have more than
 * one client for this.
 */
static int max17x0x_storage_history_read(void *buff, size_t size, int index,
					 struct max1720x_chip *chip)
{
	struct max1720x_history *hi = &chip->history_storage;

	/* (!buff || !size) -> free the memory
	 *	if index == INVALID -> return 0
	 *	if index < 0 -> return -EIVAL
	 *	if index >= 0 -> re-read history
	 */
	if (!buff || !size) {
		max1720x_history_free(hi);
		if (index == GBMS_STORAGE_INDEX_INVALID)
			return 0;
	}

	if (index < 0)
		return -EINVAL;

	/* read history if needed */
	if (hi->history_count < 0) {
		int ret;

		ret = max1720x_history_read(chip, hi);
		if (ret < 0)
			return ret;
	}

	/* index == 0 is ok here */
	if (index >= hi->history_count)
		return -ENODATA;

	/* !buff, !size to read iterator count */
	if (!size || !buff)
		return hi->history_count;

	memcpy(buff, &hi->history[index * chip->history_page_size], size);
	return size;
}

static int max17x0x_storage_read_data(gbms_tag_t tag, void *buff, size_t size,
				      int index, void *ptr)
{
	int ret;
	struct max1720x_chip *chip = (struct max1720x_chip *)ptr;

	switch (tag) {
	case GBMS_TAG_HIST:
		/* short reads are invalid */
		if (size && size != chip->history_page_size * 2)
			return -EINVAL;

		mutex_lock(&chip->history_lock);
		ret = max17x0x_storage_history_read(buff, size, index, chip);
		mutex_unlock(&chip->history_lock);
		return ret;
	default:
		ret = -ENOENT;
		break;
	}

	return ret;
}

static int max17x0x_storage_iter(int index, gbms_tag_t *tag, void *ptr)
{
	struct max1720x_chip *chip = (struct max1720x_chip *)ptr;
	static gbms_tag_t keys[] = {GBMS_TAG_SNUM, GBMS_TAG_BCNT,
				    GBMS_TAG_RAVG, GBMS_TAG_RFCN,
				    GBMS_TAG_CMPC, GBMS_TAG_DXAC};
	const int count = ARRAY_SIZE(keys);


	if (index >= 0 && index < count) {
		*tag = keys[index];
	} else if (chip->history_available && index == count) {
		*tag = GBMS_TAG_HIST;
	} else {
		return -ENOENT;
	}

	return 0;
}

static int max17x0x_storage_read(gbms_tag_t tag, void *buff, size_t size,
				 void *ptr)
{
	struct max1720x_chip *chip = (struct max1720x_chip *)ptr;
	const struct max17x0x_reg *reg;
	int ret;

	switch (tag) {
	case GBMS_TAG_SNUM:
		reg = max17x0x_find_by_tag(&chip->regmap_nvram,
					   MAX17X0X_TAG_SNUM);
		if (reg && reg->size > size)
			return -ERANGE;
		break;

	case GBMS_TAG_BCNT:
		reg = max17x0x_find_by_tag(&chip->regmap_nvram,
					   MAX17X0X_TAG_BCNT);
		if (reg && reg->size != size)
			return -ERANGE;
		break;

	/* Was POWER_SUPPLY_PROP_RESISTANCE_AVG */
	case GBMS_TAG_RAVG:
		if (size != sizeof(u16))
			return -ERANGE;

		/* TODO(167639130) use tags */
		ret = batt_res_registers(chip, true, SEL_RES_AVG, (u16 *)buff);
		if (ret == -EINVAL)
			*(u16 *)buff = -1;
		return 0;

	/* Was POWER_SUPPLY_PROP_RES_FILTER_COUNT */
	case GBMS_TAG_RFCN:
		if (size != sizeof(u16))
			return -ERANGE;

		/* TODO(167639130) use tags */
		ret = batt_res_registers(chip, true, SEL_RES_FILTER_COUNT,
					(u16 *)buff);
		if (ret == -EINVAL)
			*(u16 *)buff = -1;
		return 0;

	case GBMS_TAG_DXAC:
/*	MAX17201_DXACC_UPDATE_CNT = MAX1720X_NTALRTTH, */
	case GBMS_TAG_CMPC:
/*	MAX17201_COMP_UPDATE_CNT = MAX1720X_NVALRTTH, */
		reg = NULL;
		break;

	default:
		reg = NULL;
		break;
	}

	if (!reg)
		return -ENOENT;

	ret = max17x0x_reg_load(&chip->regmap_nvram, reg, buff);
	if (ret == 0)
		ret = reg->size;

	return ret;
}

static int max17x0x_storage_write(gbms_tag_t tag, const void *buff, size_t size,
				  void *ptr)
{
	int ret;
	const struct max17x0x_reg *reg;
	struct max1720x_chip *chip = (struct max1720x_chip *)ptr;

	switch (tag) {
	case GBMS_TAG_BCNT:
		reg = max17x0x_find_by_tag(&chip->regmap_nvram,
					   MAX17X0X_TAG_BCNT);
		if (reg && reg->size != size)
			return -ERANGE;
	break;

	/* Was POWER_SUPPLY_PROP_RESISTANCE_AVG */
	case GBMS_TAG_RAVG:
		if (size != sizeof(u16))
			return -ERANGE;
		return batt_res_registers(chip, false, SEL_RES_AVG,
					  (u16 *)buff);

	/* Was POWER_SUPPLY_PROP_RES_FILTER_COUNT */
	case GBMS_TAG_RFCN:
		if (size != sizeof(u16))
			return -ERANGE;
		return batt_res_registers(chip, false, SEL_RES_FILTER_COUNT,
					  (u16 *)buff);

	case GBMS_TAG_DXAC:
/*	MAX17201_DXACC_UPDATE_CNT = MAX1720X_NTALRTTH, */
	case GBMS_TAG_CMPC:
/*	MAX17201_COMP_UPDATE_CNT = MAX1720X_NVALRTTH, */
		reg = NULL;
		break;

	default:
		reg = NULL;
		break;
	}

	if (!reg)
		return -ENOENT;

	ret = max17x0x_reg_store(&chip->regmap_nvram, reg, buff);
	if (ret == 0)
		ret = reg->size;

	return ret;
}

static struct gbms_storage_desc max17x0x_storage_dsc = {
	.info = max17x0x_storage_info,
	.iter = max17x0x_storage_iter,
	.read = max17x0x_storage_read,
	.write = max17x0x_storage_write,
	.read_data = max17x0x_storage_read_data,
};

/* ------------------------------------------------------------------------- */

static int max17x0x_prop_iter(int index, gbms_tag_t *tag, void *ptr)
{
	static gbms_tag_t keys[] = {GBMS_TAG_BRES, GBMS_TAG_GCFE};
	const int count = ARRAY_SIZE(keys);

	if (index >= 0 && index < count) {
		*tag = keys[index];
		return 0;
	}

	return -ENOENT;
}

static int max17x0x_prop_read(gbms_tag_t tag, void *buff, size_t size,
			      void *ptr)
{
	struct max1720x_chip *chip = (struct max1720x_chip *)ptr;
	int ret = -ENOENT;
	u16 data;

	switch (tag) {
	/* Was POWER_SUPPLY_PROP_RESISTANCE */
	case GBMS_TAG_BRES:
		if (size != sizeof(u32))
			return -ERANGE;

		ret = REGMAP_READ(&chip->regmap, MAX1720X_RCELL, &data);
		if (ret < 0)
			return ret;

		*(u32 *)buff = reg_to_resistance_micro_ohms(data, chip->RSense);
		break;

	/* Was POWER_SUPPLY_PROP_CHARGE_FULL_ESTIMATE */
	case GBMS_TAG_GCFE:
		if (size != sizeof(u16))
			return -ERANGE;

		ret = batt_ce_full_estimate(&chip->cap_estimate);
		if (ret < 0)
			return ret;

		*(u16 *)buff = ret & 0xffff;
		break;

	default:
		break;
	}

	return ret;
}

static struct gbms_storage_desc max17x0x_prop_dsc = {
	.iter = max17x0x_prop_iter,
	.read = max17x0x_prop_read,
};

/* ------------------------------------------------------------------------- */

/* this must be not blocking */
static void max17x0x_read_serial_number(struct max1720x_chip *chip)
{
	char buff[32] = {0};
	int ret;

	ret = gbms_storage_read(GBMS_TAG_MINF, buff, GBMS_MINF_LEN);
	if (ret >= 0) {
		strncpy(chip->serial_number, buff, ret);
		return;
	}

	ret = gbms_storage_read(GBMS_TAG_SNUM, buff, sizeof(buff));
	if (ret < 0) {
		/* do nothing */
	} else if (chip->gauge_type == MAX1730X_GAUGE_TYPE) {
		ret = max1730x_decode_sn(chip->serial_number,
					 sizeof(chip->serial_number),
					 (u16 *)buff);
	} else if (chip->gauge_type == MAX1720X_GAUGE_TYPE) {
		ret = max1720x_decode_sn(chip->serial_number,
					 sizeof(chip->serial_number),
					 (u16 *)buff);
	} else {
		if (ret > sizeof(chip->serial_number))
			ret = sizeof(chip->serial_number);
		strncpy(chip->serial_number, buff, ret);
	}

	if (ret < 0)
		chip->serial_number[0] = '\0';
}

static void max1720x_init_work(struct work_struct *work)
{
	struct max1720x_chip *chip = container_of(work, struct max1720x_chip,
						  init_work.work);
	int ret = 0;

	if (chip->gauge_type != -1) {

		/* TODO: move to max1720x1 */
		if (chip->regmap_nvram.regmap) {
			ret = gbms_storage_register(&max17x0x_storage_dsc,
						    "maxfg_sr", chip);
			if (ret == -EBUSY)
				ret = 0;
		}

		/* these don't require nvm storage */
		ret = gbms_storage_register(&max17x0x_prop_dsc, "maxfg", chip);
		if (ret == -EBUSY)
			ret = 0;

		if (ret == 0)
			ret = max1720x_init_chip(chip);
		if (ret == -EPROBE_DEFER) {
			schedule_delayed_work(&chip->init_work,
				msecs_to_jiffies(MAX1720X_DELAY_INIT_MS));
			return;
		}
	}

	/* serial number might not be stored in the FG */
	max17x0x_read_serial_number(chip);

	mutex_init(&chip->cap_estimate.batt_ce_lock);
	chip->prev_charge_status = POWER_SUPPLY_STATUS_UNKNOWN;
	chip->fake_capacity = -EINVAL;
	chip->resume_complete = true;
	chip->init_complete = true;
	max17x0x_init_debugfs(chip);

	/*
	 * Handle any IRQ that might have been set before init
	 * NOTE: will clear the POR bit and trigger model load if needed
	 */
	max1720x_fg_irq_thread_fn(-1, chip);

	dev_info(chip->dev, "init_work done\n");
	if (chip->gauge_type == -1)
		return;

	/* Init History and Capacity Estimate only when gauge type is known. */
	ret = max1720x_init_history(chip);
	if (ret == 0)
		(void)max1720x_init_history_device(chip);

	ret = batt_ce_load_data(&chip->regmap_nvram, &chip->cap_estimate);
	if (ret == 0)
		batt_ce_dump_data(&chip->cap_estimate, chip->ce_log);
}

/* TODO: fix detection of 17301 for non samples looking at FW version too */
static int max17xxx_read_gauge_type(struct max1720x_chip *chip)
{
	u8 reg = MAX1720X_DEVNAME;
	struct i2c_msg xfer[2];
	uint8_t buf[2] = { };
	int ret, gauge_type;

	/* some maxim IF-PMIC corrupt reads w/o Rs b/152373060 */
	xfer[0].addr = chip->primary->addr;
	xfer[0].flags = 0;
	xfer[0].len = 1;
	xfer[0].buf = &reg;

	xfer[1].addr = chip->primary->addr;
	xfer[1].flags = I2C_M_RD;
	xfer[1].len = 2;
	xfer[1].buf = buf;

	ret = i2c_transfer(chip->primary->adapter, xfer, 2);
	if (ret != 2)
		return -EIO;

	/* it might need devname later */
	chip->devname = buf[1] << 8 | buf[0];

	ret = of_property_read_u32(chip->dev->of_node, "maxim,gauge-type",
				   &gauge_type);
	if (ret == 0) {
		dev_warn(chip->dev, "forced gauge type to %d\n", gauge_type);
		return gauge_type;
	}

	/* 0 not M5, !=0 M5 */
	ret = max_m5_check_devname(chip->devname);
	if (ret)
		return MAX_M5_GAUGE_TYPE;

	switch (chip->devname >> 4) {
	case 0x404: /* max1730x sample */
	case 0x405: /* max1730x pass2 silicon initial samples */
	case 0x406: /* max1730x pass2 silicon */
		gauge_type = MAX1730X_GAUGE_TYPE;
		break;
	default:
		gauge_type = MAX1720X_GAUGE_TYPE;
		dev_warn(chip->dev, "devname=%x defaults to %d\n",
			 chip->devname, gauge_type);
		break;
	}

	return gauge_type;
}


/* NOTE: NEED TO COME BEFORE REGISTER ACCESS */
static int max17x0x_regmap_init(struct max1720x_chip *chip)
{
	int secondary_address = 0xb;
	struct device *dev = chip->dev;

	if (chip->gauge_type == MAX1730X_GAUGE_TYPE) {
		/* redefine primary for max1730x */
		chip->regmap.regmap = devm_regmap_init_i2c(chip->primary,
						&max1730x_regmap_cfg);
		if (IS_ERR(chip->regmap.regmap)) {
			dev_err(chip->dev, "Failed to re-initialize regmap (%ld)\n",
				IS_ERR_VALUE(chip->regmap.regmap));
			return -EINVAL;
		}

		/* shadow override is not supported on early samples */
		chip->shadow_override = (chip->devname >> 4) != 0x404;

		chip->regmap.regtags.max = ARRAY_SIZE(max1730x);
		chip->regmap.regtags.map = max1730x;
		chip->fixups_fn = max1730x_fixups;
	} else if (chip->gauge_type == MAX_M5_GAUGE_TYPE) {
		int ret;

		ret = max_m5_regmap_init(&chip->regmap, chip->primary);
		if (ret < 0) {
			dev_err(chip->dev, "Failed to re-initialize regmap (%ld)\n",
				IS_ERR_VALUE(chip->regmap.regmap));
			return -EINVAL;
		}

		chip->shadow_override = false;
		secondary_address = 0;
	} else if (chip->gauge_type == MAX1720X_GAUGE_TYPE) {

		chip->regmap.regmap = devm_regmap_init_i2c(chip->primary,
							&max1720x_regmap_cfg);
		if (IS_ERR(chip->regmap.regmap)) {
			dev_err(chip->dev, "Failed to initialize primary regmap (%ld)\n",
				IS_ERR_VALUE(chip->regmap.regmap));
			return -EINVAL;
		}

		/* max1720x is default map */
		chip->regmap.regtags.max = ARRAY_SIZE(max1720x);
		chip->regmap.regtags.map = max1720x;
	}

	/* todo read secondary address from DT */
	if (!secondary_address) {
		dev_warn(chip->dev, "Device 0x%x has no permanent storage\n",
			chip->devname);
		return 0;
	}

	chip->secondary = i2c_new_ancillary_device(chip->primary,
						   "nvram",
						   secondary_address);
	if (chip->secondary == NULL) {
		dev_err(dev, "Failed to initialize secondary i2c device\n");
		return -ENODEV;
	}

	i2c_set_clientdata(chip->secondary, chip);

	if (chip->gauge_type == MAX1730X_GAUGE_TYPE) {
		chip->regmap_nvram.regmap =
			devm_regmap_init_i2c(chip->secondary,
					     &max1730x_regmap_nvram_cfg);
		if (IS_ERR(chip->regmap_nvram.regmap)) {
			dev_err(chip->dev, "Failed to initialize nvram regmap (%ld)\n",
				PTR_ERR(chip->regmap_nvram.regmap));
			return -EINVAL;
		}

		chip->regmap_nvram.regtags.max = ARRAY_SIZE(max1730x);
		chip->regmap_nvram.regtags.map = max1730x;
	} else {
		chip->regmap_nvram.regmap =
			devm_regmap_init_i2c(chip->secondary,
					     &max1720x_regmap_nvram_cfg);
		if (IS_ERR(chip->regmap_nvram.regmap)) {
			dev_err(chip->dev, "Failed to initialize nvram regmap (%ld)\n",
				PTR_ERR(chip->regmap_nvram.regmap));
			return -EINVAL;
		}

		chip->regmap_nvram.regtags.max = ARRAY_SIZE(max1720x);
		chip->regmap_nvram.regtags.map = max1720x;
	}

	return 0;
}

static int max1720x_init_irq(struct max1720x_chip *chip)
{
	unsigned long irqf = IRQF_TRIGGER_LOW | IRQF_ONESHOT;
	int ret, irqno;

	chip->irq_shared = of_property_read_bool(chip->dev->of_node,
						 "maxim,irqf-shared");
	irqno = chip->primary->irq;
	if (!irqno) {
		int irq_gpio;

		irq_gpio = of_get_named_gpio(chip->dev->of_node,
					     "maxim,irq-gpio", 0);
		if (irq_gpio >= 0) {
			chip->primary->irq = gpio_to_irq(irq_gpio);
			if (chip->primary->irq <= 0) {
				chip->primary->irq = 0;
				dev_warn(chip->dev, "fg irq not avalaible\n");
				return 0;
			}
		}
	}

	if (chip->irq_shared)
		irqf |= IRQF_SHARED;

	ret = request_threaded_irq(chip->primary->irq, NULL,
				   max1720x_fg_irq_thread_fn, irqf,
				   MAX1720X_I2C_DRIVER_NAME, chip);
	dev_info(chip->dev, "FG irq handler registered at %d (%d)\n",
			    chip->primary->irq, ret);
	if (ret == 0)
		enable_irq_wake(chip->primary->irq);

	return ret;
}

/* possible race */
void *max1720x_get_model_data(struct i2c_client *client)
{
	struct max1720x_chip *chip = i2c_get_clientdata(client);

	return chip ? chip->model_data : NULL;
}

static int max1720x_probe(struct i2c_client *client,
			  const struct i2c_device_id *id)
{
	struct max1720x_chip *chip;
	struct device *dev = &client->dev;
	struct power_supply_config psy_cfg = { };
	const struct max17x0x_reg *reg;
	const char *psy_name = NULL;
	int ret = 0;

	chip = devm_kzalloc(dev, sizeof(*chip), GFP_KERNEL);
	if (!chip)
		return -ENOMEM;

	chip->dev = dev;
	chip->primary = client;
	chip->batt_id_defer_cnt = DEFAULT_BATTERY_ID_RETRIES;
	i2c_set_clientdata(client, chip);

	/* NOTE: < 0 not avalable, it could be a bare MLB */
	chip->gauge_type = max17xxx_read_gauge_type(chip);
	if (chip->gauge_type < 0)
		chip->gauge_type = -1;

	/* needs chip->primary and (optional) chip->secondary */
	ret = max17x0x_regmap_init(chip);
	if (ret < 0) {
		dev_err(dev, "Failed to initialize regmap(s)\n");
		goto i2c_unregister;
	}

	dev_warn(chip->dev, "device gauge_type: %d shadow_override=%d\n",
		 chip->gauge_type, chip->shadow_override);

	if (of_property_read_bool(dev->of_node, "maxim,log_writes")) {
		bool debug_reglog;

		debug_reglog = max17x0x_reglog_init(chip);
		dev_info(dev, "write log %savailable\n",
			 debug_reglog ? "" : "not ");
	}

	/* M5 requires zero IRQ */
	chip->zero_irq  = -1;
	if (chip->gauge_type == MAX_M5_GAUGE_TYPE)
		chip->zero_irq = 1;
	if (chip->zero_irq == -1)
		chip->zero_irq = of_property_read_bool(chip->dev->of_node,
						       "maxim,zero-irq");

	/* TODO: do not request the interrupt if the gauge is not present */
	ret = max1720x_init_irq(chip);
	if (ret < 0) {
		dev_err(dev, "cannot allocate irq\n");
		return ret;
	}

	psy_cfg.drv_data = chip;
	psy_cfg.of_node = chip->dev->of_node;

	ret = of_property_read_string(dev->of_node,
				      "maxim,dual-battery", &psy_name);
	if (ret == 0)
		chip->max1720x_psy_desc.name = devm_kstrdup(dev, psy_name, GFP_KERNEL);
	else
		chip->max1720x_psy_desc.name = "maxfg";

	pr_info("max1720x_psy_desc.name=%s\n",chip->max1720x_psy_desc.name);

	chip->max1720x_psy_desc.type = POWER_SUPPLY_TYPE_BATTERY;
	chip->max1720x_psy_desc.get_property = max1720x_get_property;
	chip->max1720x_psy_desc.set_property = max1720x_set_property;
	chip->max1720x_psy_desc.property_is_writeable = max1720x_property_is_writeable;
	chip->max1720x_psy_desc.properties = max1720x_battery_props;
	chip->max1720x_psy_desc.num_properties = ARRAY_SIZE(max1720x_battery_props);

	if (of_property_read_bool(dev->of_node, "maxim,psy-type-unknown"))
		chip->max1720x_psy_desc.type = POWER_SUPPLY_TYPE_UNKNOWN;

	chip->psy = devm_power_supply_register(dev, &chip->max1720x_psy_desc,
					       &psy_cfg);
	if (IS_ERR(chip->psy)) {
		dev_err(dev, "Couldn't register as power supply\n");
		ret = PTR_ERR(chip->psy);
		goto irq_unregister;
	}

	ret = device_create_file(&chip->psy->dev, &dev_attr_offmode_charger);
	if (ret) {
		dev_err(dev, "Failed to create offmode_charger attribute\n");
		goto psy_unregister;
	}

	/* Was POWER_SUPPLY_PROP_RESISTANCE_ID */
	ret = device_create_file(&chip->psy->dev, &dev_attr_resistance_id);
	if (ret)
		dev_err(dev, "Failed to create resistance_id attribute\n");

	/* POWER_SUPPLY_PROP_RESISTANCE */
	ret = device_create_file(&chip->psy->dev, &dev_attr_resistance);
	if (ret)
		dev_err(dev, "Failed to create resistance attribute\n");

	/*
	 * TODO:
	 *	POWER_SUPPLY_PROP_CHARGE_FULL_ESTIMATE -> GBMS_TAG_GCFE
	 *	POWER_SUPPLY_PROP_RES_FILTER_COUNT -> GBMS_TAG_RFCN
	 *	POWER_SUPPLY_PROP_RESISTANCE_AVG -> GBMS_TAG_RAVG
	 */

	/* M5 battery model needs batt_id and is setup during init() */
	chip->model_reload = MAX_M5_LOAD_MODEL_DISABLED;
	if (chip->gauge_type == MAX_M5_GAUGE_TYPE) {
		ret = device_create_file(&chip->psy->dev,
					 &dev_attr_m5_model_state);
		if (ret)
			dev_err(dev, "Failed to create model_state, ret=%d\n",
				ret);
	}

	chip->ce_log = logbuffer_register(chip->max1720x_psy_desc.name);
	if (IS_ERR(chip->ce_log)) {
		ret = PTR_ERR(chip->ce_log);
		dev_err(dev, "failed to obtain logbuffer, ret=%d\n", ret);
		chip->ce_log = NULL;
	}

	/* use VFSOC until it can confirm that FG Model is running */
	reg = max17x0x_find_by_tag(&chip->regmap, MAX17X0X_TAG_vfsoc);
	chip->reg_prop_capacity_raw = (reg) ? reg->reg : MAX1720X_REPSOC;

	INIT_DELAYED_WORK(&chip->cap_estimate.settle_timer,
			  batt_ce_capacityfiltered_work);
	INIT_DELAYED_WORK(&chip->init_work, max1720x_init_work);
	INIT_DELAYED_WORK(&chip->model_work, max1720x_model_work);

	schedule_delayed_work(&chip->init_work, 0);

	return 0;

psy_unregister:
	power_supply_unregister(chip->psy);
irq_unregister:
	free_irq(chip->primary->irq, chip);
i2c_unregister:
	i2c_unregister_device(chip->secondary);

	return ret;
}

static int max1720x_remove(struct i2c_client *client)
{
	struct max1720x_chip *chip = i2c_get_clientdata(client);

	if (chip->ce_log) {
		logbuffer_unregister(chip->ce_log);
		chip->ce_log = NULL;
	}

	max1720x_cleanup_history(chip);
	max_m5_free_data(chip->model_data);
	cancel_delayed_work(&chip->init_work);
	cancel_delayed_work(&chip->model_work);

	if (chip->primary->irq)
		free_irq(chip->primary->irq, chip);
	power_supply_unregister(chip->psy);

	if (chip->secondary)
		i2c_unregister_device(chip->secondary);

	return 0;
}

static const struct of_device_id max1720x_of_match[] = {
	{ .compatible = "maxim,max1720x"},
	{ .compatible = "maxim,max77729f"},
	{ .compatible = "maxim,max77759"},
	{},
};
MODULE_DEVICE_TABLE(of, max1720x_of_match);

static const struct i2c_device_id max1720x_id[] = {
	{"max1720x", 0},
	{}
};
MODULE_DEVICE_TABLE(i2c, max1720x_id);

#ifdef CONFIG_PM_SLEEP
static int max1720x_pm_suspend(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct max1720x_chip *chip = i2c_get_clientdata(client);

	pm_runtime_get_sync(chip->dev);
	chip->resume_complete = false;
	pm_runtime_put_sync(chip->dev);

	return 0;
}

static int max1720x_pm_resume(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct max1720x_chip *chip = i2c_get_clientdata(client);

	pm_runtime_get_sync(chip->dev);
	chip->resume_complete = true;
	pm_runtime_put_sync(chip->dev);

	return 0;
}
#endif

static const struct dev_pm_ops max1720x_pm_ops = {
	SET_LATE_SYSTEM_SLEEP_PM_OPS(max1720x_pm_suspend, max1720x_pm_resume)
};

static struct i2c_driver max1720x_i2c_driver = {
	.driver = {
		   .name = "max1720x",
		   .of_match_table = max1720x_of_match,
		   .pm = &max1720x_pm_ops,
		   .probe_type = PROBE_PREFER_ASYNCHRONOUS,
		   },
	.id_table = max1720x_id,
	.probe = max1720x_probe,
	.remove = max1720x_remove,
};

module_i2c_driver(max1720x_i2c_driver);
MODULE_AUTHOR("Thierry Strudel <tstrudel@google.com>");
MODULE_AUTHOR("AleX Pelosi <apelosi@google.com>");
MODULE_DESCRIPTION("MAX17x01/MAX17x05 Fuel Gauge");
MODULE_LICENSE("GPL");
