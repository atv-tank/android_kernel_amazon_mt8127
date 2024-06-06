/*
 * Copyright (C) 2011 Amazon Technologies, Inc.
 * Portions Copyright (C) 2007-2008 Google, Inc.
 *
 * portion copyright 2023 Amazon Technologies, Inc. All Rights Reserved
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#ifndef _LINUX_METRICSLOG_H
#define _LINUX_METRICSLOG_H

#if defined (CONFIG_AMAZON_METRICS_LOG) || (CONFIG_AMAZON_MINERVA_METRICS_LOG)

#include <linux/xlog.h>

typedef enum {
    VITALS_NORMAL = 0,
    VITALS_FGTRACKING,
    VITALS_TIME_BUCKET,
} vitals_type;

void log_to_metrics(enum android_log_priority priority,
	const char *domain, const char *logmsg);
void log_to_vitals(enum android_log_priority priority,
	const char *domain, const char *log_msg);
#endif /* (CONFIG_AMAZON_METRICS_LOG) || (CONFIG_AMAZON_MINERVA_METRICS_LOG)  */

#ifdef CONFIG_AMAZON_METRICS_LOG
void log_counter_to_vitals(enum android_log_priority priority,
    const char *domain, const char *program,
    const char *source, const char *key,
    long counter_value, const char *unit,
    const char *metadata, vitals_type type);
void log_timer_to_vitals(enum android_log_priority priority,
    const char *domain, const char *program,
    const char *source, const char *key,
    long timer_value, const char *unit, vitals_type type);
#endif /* CONFIG_AMAZON_METRICS_LOG  */

#ifdef CONFIG_AMAZON_LOG
void log_to_amzmain(enum android_log_priority priority,
		const char *domain, const char *logmsg);
#endif

#ifdef CONFIG_AMAZON_MINERVA_METRICS_LOG
void log_counter_to_vitals_v2(enum android_log_priority priority,
	const char *group_id, const char *schema_id,
	const char *domain, const char *program,
	const char *source, const char *key,
	long counter_value, const char *unit,
	const char *metadata, vitals_type type,
	const char *dimensions, const char *annotations);

void log_timer_to_vitals_v2(enum android_log_priority priority,
	const char *group_id, const char *schema_id,
	const char *domain, const char *program,
	const char *source, const char *key,
	long timer_value, const char *unit, vitals_type type,
	const char *dimensions, const char *annotations);
#endif /* CONFIG_AMAZON_MINERVA_METRICS_LOG */

#endif /* _LINUX_METRICSLOG_H */

