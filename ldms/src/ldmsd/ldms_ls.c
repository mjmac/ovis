/* -*- c-basic-offset: 8 -*-
 * Copyright (c) 2010-2019 National Technology & Engineering Solutions
 * of Sandia, LLC (NTESS). Under the terms of Contract DE-NA0003525 with
 * NTESS, the U.S. Government retains certain rights in this software.
 * Copyright (c) 2010-2019 Open Grid Computing, Inc. All rights reserved.
 *
 * This software is available to you under a choice of one of two
 * licenses.  You may choose to be licensed under the terms of the GNU
 * General Public License (GPL) Version 2, available from the file
 * COPYING in the main directory of this source tree, or the BSD-type
 * license below:
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 *      Redistributions of source code must retain the above copyright
 *      notice, this list of conditions and the following disclaimer.
 *
 *      Redistributions in binary form must reproduce the above
 *      copyright notice, this list of conditions and the following
 *      disclaimer in the documentation and/or other materials provided
 *      with the distribution.
 *
 *      Neither the name of Sandia nor the names of any contributors may
 *      be used to endorse or promote products derived from this software
 *      without specific prior written permission.
 *
 *      Neither the name of Open Grid Computing nor the names of any
 *      contributors may be used to endorse or promote products derived
 *      from this software without specific prior written permission.
 *
 *      Modified source versions must be plainly marked as such, and
 *      must not be misrepresented as being the original software.
 *
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */
#include <getopt.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <signal.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <inttypes.h>
#include <fcntl.h>
#include <sys/queue.h>
#include <time.h>
#include <ovis_util/util.h>
#include <semaphore.h>
#include <regex.h>
#include <pwd.h>
#include <grp.h>
#include "ldms.h"
#include "ldms_xprt.h"
#include "config.h"

#define LDMS_LS_MEM_SZ_ENVVAR "LDMS_LS_MEM_SZ"
#define LDMS_LS_MAX_MEM_SIZE 512L * 1024L * 1024L
#define LDMS_LS_MAX_MEM_SZ_STR "512MB"

/* from ldmsd_group.c */
#define GRP_SCHEMA_NAME "ldmsd_grp_schema"
#define GRP_KEY_PREFIX "    grp_member: "
#define GRP_GN_NAME "ldmsd_grp_gn"
/* ----- */

static size_t max_mem_size;
static char *mem_sz;

static pthread_mutex_t dir_lock;
static pthread_cond_t dir_cv;
static int dir_done;
static int dir_status;

static pthread_mutex_t print_lock;
static pthread_cond_t print_cv;
static int print_done;

static pthread_mutex_t done_lock;
static pthread_cond_t done_cv;
static int done;

static sem_t conn_sem;

static int schema;

struct ls_set {
	struct ldms_dir_set_s *set_data;
	LIST_ENTRY(ls_set) entry;
};
LIST_HEAD(set_list, ls_set) set_list;

/*
 * A wrapper so that we can keep all received dir's
 * so that we can
 * 1) store all dir sets without copying them
 * 2) free all dirs and dir sets when we are done.
 */
struct ldms_ls_dir {
	ldms_dir_t dir;
	LIST_ENTRY(ldms_ls_dir) entry;
};
LIST_HEAD(ldms_ls_dir_list, ldms_ls_dir) dir_list;

struct match_str {
	char *str;
	regex_t regex;
	LIST_ENTRY(match_str) entry;
};
LIST_HEAD(match_list, match_str) match_list;

const char *auth_name = "none";
struct attr_value_list *auth_opt = NULL;
const int auth_opt_max = 128;

#define FMT "h:p:x:w:m:ESIlvua:A:VP"
void usage(char *argv[])
{
	printf("%s -h <hostname> -x <transport> [ name ... ]\n"
	       "\n    -h <hostname>    The name of the host to query. Default is localhost.\n"
	       "\n    -p <port_num>    The port number. The default is 50000.\n"
	       "\n    -l               Show the values of the metrics in each metric set.\n"
	       "\n    -u               Show the user-defined metric meta data value.\n"
	       "\n    -x <name>        The transport name: sock, rdma, or local. Default is\n"
	       "                     localhost unless -h is specified in which case it is sock.\n"
	       "\n    -w <secs>        The time to wait before giving up on the server.\n"
	       "                     The default is 10 seconds.\n"
	       "\n    -v             Show detail information about the metric set. Specifying\n"
	       "                     this option multiple times increases the verbosity.\n"
	       "\n    -E               The <name> arguments are regular expressions.\n"
	       "\n    -S               The <name>s refers to the schema name.\n"
	       "\n    -I               The <name>s refer to the instance name (default).\n",
		argv[0]);
	printf("\n    -m <memory size> Maximum size of pre-allocated memory for metric sets.\n"
	       "                     The given size must be less than 1 petabytes.\n"
	       "                     The default is %s.\n"
	       "                     For example, 20M or 20mb are 20 megabytes.\n"
	       "                     - The environment variable %s could be set\n"
	       "                     instead of giving the -m option. If both are given,\n"
	       "                     this option takes precedence over the environment variable.\n"
	       "\n    -a <auth>        LDMS Authentication plugin to be used (default: 'none').\n"
	       "\n    -A <key>=<value> (repeatable) LDMS Authentication plugin parameters.\n"
	       , LDMS_LS_MAX_MEM_SZ_STR, LDMS_LS_MEM_SZ_ENVVAR);
	printf("\n    -V           Print LDMS version and exit.\n");
	printf("\n    -P           Register for push updates.\n");
	exit(1);
}

int __compile_regex(regex_t *regex, const char *regex_str) {
	char errmsg[128];
	memset(regex, 0, sizeof(*regex));
	int rc = regcomp(regex, regex_str, REG_EXTENDED | REG_NOSUB);
	if (rc) {
		(void)regerror(rc, regex, errmsg, 128);
		printf("%s\n", errmsg);
	}
	return rc;
}

void server_timeout(void)
{
	printf("A timeout occurred waiting for a response from the server.\n"
	       "Use the -w option to specify the amount of time to wait "
	       "for the server\n");
	exit(1);
}

#define BUF_LEN 128
size_t prim_value_format(enum ldms_value_type type, ldms_mval_t val, size_t n, int width, int print)
{
	int i;
	char buf[BUF_LEN];
	size_t cnt;

	switch (type) {
	case LDMS_V_CHAR_ARRAY:
		cnt = snprintf(buf, BUF_LEN, "\"%s\"", val->a_char);
		break;
	case LDMS_V_CHAR:
		cnt = snprintf(buf, BUF_LEN, "'%c'", val->v_char);
		break;
	case LDMS_V_U8:
		cnt = snprintf(buf, BUF_LEN, "%hhu", val->v_u8);
		break;
	case LDMS_V_U8_ARRAY:
		cnt = 0;
		for (i = 0; i < n; i++) {
			if (i)
				cnt += snprintf(&buf[cnt], BUF_LEN - cnt, ",");
			cnt += snprintf(&buf[cnt], BUF_LEN - cnt, "0x%02hhx", val->a_u8[i]);
		}
		break;
	case LDMS_V_S8:
		cnt = snprintf(buf, BUF_LEN, "%hhd", val->v_s8);
		break;
	case LDMS_V_S8_ARRAY:
		cnt = 0;
		for (i = 0; i < n; i++) {
			if (i)
				cnt += snprintf(&buf[cnt], BUF_LEN - cnt, ",");
			cnt += snprintf(&buf[cnt], BUF_LEN - cnt, "%hhd", val->a_s8[i]);
		}
		break;
	case LDMS_V_U16:
		cnt = snprintf(buf, BUF_LEN, "%hu", val->v_u16);
		break;
	case LDMS_V_U16_ARRAY:
		cnt = 0;
		for (i = 0; i < n; i++) {
			if (i)
				cnt += snprintf(&buf[cnt], BUF_LEN - cnt, ",");
			cnt += snprintf(&buf[cnt], BUF_LEN - cnt, "%hu", val->a_u16[i]);
		}
		break;
	case LDMS_V_S16:
		cnt = snprintf(buf, BUF_LEN, "%hd", val->v_s16);
		break;
	case LDMS_V_S16_ARRAY:
		cnt = 0;
		for (i = 0; i < n; i++) {
			if (i)
				cnt += snprintf(&buf[cnt], BUF_LEN - cnt, ",");
			cnt += snprintf(&buf[cnt], BUF_LEN - cnt, "%hd", val->a_s16[i]);
		}
		break;
	case LDMS_V_U32:
		cnt = snprintf(buf, BUF_LEN, "%u", val->v_u32);
		break;
	case LDMS_V_U32_ARRAY:
		cnt = 0;
		for (i = 0; i < n; i++) {
			if (i)
				cnt += snprintf(&buf[cnt], BUF_LEN - cnt, ",");
			cnt += snprintf(&buf[cnt], BUF_LEN - cnt, "%u", val->a_u32[i]);
		}
		break;
	case LDMS_V_S32:
		cnt = snprintf(buf, BUF_LEN, "%d", val->v_s32);
		break;
	case LDMS_V_S32_ARRAY:
		cnt = 0;
		for (i = 0; i < n; i++) {
			if (i)
				cnt += snprintf(&buf[cnt], BUF_LEN-cnt, ",");
			cnt += snprintf(&buf[cnt], BUF_LEN - cnt, "%d", val->a_s32[i]);
		}
		break;
	case LDMS_V_U64:
		cnt = snprintf(buf, BUF_LEN, "%"PRIu64, val->v_u64);
		break;
	case LDMS_V_U64_ARRAY:
		cnt = 0;
		for (i = 0; i < n; i++) {
			if (i)
				cnt += snprintf(&buf[cnt], BUF_LEN - cnt, ",");
			cnt += snprintf(&buf[cnt], BUF_LEN - cnt, "%"PRIu64, val->a_u64[i]);
		}
		break;
	case LDMS_V_S64:
		cnt = snprintf(buf, BUF_LEN, "%"PRId64, val->v_s64);
		break;
	case LDMS_V_S64_ARRAY:
		cnt = 0;
		for (i = 0; i < n; i++) {
			if (i)
				cnt += snprintf(&buf[cnt], BUF_LEN - cnt, ",");
			cnt += snprintf(&buf[cnt], BUF_LEN - cnt, "%"PRId64, val->a_s64[i]);
		}
		break;
	case LDMS_V_F32:
		cnt = snprintf(buf, BUF_LEN, "%f", val->v_f);
		break;
	case LDMS_V_F32_ARRAY:
		cnt = 0;
		for (i = 0; i < n; i++) {
			if (i)
				cnt += snprintf(&buf[cnt], BUF_LEN - cnt, ",");
			cnt += snprintf(&buf[cnt], BUF_LEN - cnt, "%f", val->a_f[i]);
		}
		break;
	case LDMS_V_D64:
		cnt = snprintf(buf, BUF_LEN, "%f", val->v_d);
		break;
	case LDMS_V_D64_ARRAY:
		cnt = 0;
		for (i = 0; i < n; i++) {
			if (i)
				cnt += snprintf(&buf[cnt], BUF_LEN - cnt, ",");
			cnt += snprintf(&buf[cnt], BUF_LEN - cnt, "%f", val->a_d[i]);
		}
		break;
	default:
		return 0;
	}
	if (print)
		printf("%*s", width, buf);
	return cnt;
}

void rec_col_width(ldms_mval_t rec, size_t card, int *col)
{
	int i;
	char *unit;
	size_t cnt;
	enum ldms_value_type etype;
	size_t ecount;
	ldms_mval_t e;

	if (0 == col[0]) {
		for (i = 0; i < card; i++) {
			col[i] = strlen(ldms_record_metric_name_get(rec, i));
			unit = (char*)ldms_record_metric_unit_get(rec, i);
			if (unit && strlen(unit))
				col[i] += strlen(unit) + 3;
		}
	}
	for (i = 0; i < card; i++) {
		e = ldms_record_metric_get(rec, i);
		etype = ldms_record_metric_type_get(rec, i, &ecount);
		cnt = prim_value_format(etype, e, ecount, 0, 0);
		if (col[i] < cnt)
			col[i] = cnt;
	}
}

void record_header_format(ldms_mval_t rec, size_t card, int *col)
{
	int i;
	char *unit;
	char buf[128];
	size_t len = 128;

	for (i = 0; i < card; i++) {
		if (i)
			printf(" ");
		unit = (char*)ldms_record_metric_unit_get(rec, i);
		if (unit && strlen(unit)) {
			snprintf(buf, len, "%s (%s)",
				ldms_record_metric_name_get(rec, i), unit);
		} else {
			snprintf(buf, len, "%s",
				ldms_record_metric_name_get(rec, i));
		}
		printf("%*s", col[i], buf);
	}
}

void record_format(ldms_mval_t rec, size_t card, int *col)
{
	int i;
	ldms_mval_t e;
	enum ldms_value_type etyp;
	size_t count;

	for (i = 0; i < card; i++) {
		if (i)
			printf(" ");
		e = ldms_record_metric_get(rec, i);
		etyp = ldms_record_metric_type_get(rec, i, &count);
		prim_value_format(etyp, e, count, col[i], 1);
	}
}

void list_record_format(ldms_set_t s, ldms_mval_t lh)
{
	ldms_mval_t lval;
	size_t count;
	enum ldms_value_type prev_ltype, ltype;
	int rec_type_id, prev_rec_type_id;
	int *cw;
	int card;
	prev_rec_type_id = -1;
	int i;

	card = ldms_record_card(ldms_list_first(s, lh, &ltype, &count));
	cw = calloc(card, sizeof(int));

	/* Determine the column widths */
	for (lval = ldms_list_first(s, lh, &ltype, &count), i = 0; lval;
			lval = ldms_list_next(s, lval, &ltype, &count), i++) {
		if (i && prev_ltype != ltype) {
			printf("Error: A list contains entries of different types.\n");
			exit(ENOTSUP);
		}
		if (i && prev_rec_type_id != rec_type_id) {
			printf("Error: A list contains entries of different record types.\n");
			exit(ENOTSUP);
		}
		rec_type_id = ldms_record_type_get(lval);
		rec_col_width(lval, card, cw);
		prev_ltype = ltype;
		prev_rec_type_id = rec_type_id;
	}

	/* Print header */
	lval = ldms_list_first(s, lh, &ltype, &count);
	printf("  ");
	record_header_format(lval, card, cw);

	printf("\n");

	/* Print record values */
	for (lval = ldms_list_first(s, lh, &ltype, &count); lval;
			lval = ldms_list_next(s, lval, &ltype, &count)) {
		printf("  ");
		record_format(lval, card, cw);
		printf("\n");
	}
}

void record_array_format(ldms_set_t s, ldms_mval_t rh)
{
	int i;
	ldms_mval_t rec, frec;
	size_t card;
	int *width;
	size_t len = ldms_record_array_len(rh);

	frec = ldms_record_array_get_inst(rh, 0);
	card = ldms_record_card(frec);
	width = calloc(card, sizeof(int));

	for (i = 0; i < len; i++) {
		rec = ldms_record_array_get_inst(rh, i);
		rec_col_width(rec, card, width);
	}

	printf("  ");
	record_header_format(frec, card, width);
	printf("\n");

	for (i = 0; i < len; i++) {
		rec = ldms_record_array_get_inst(rh, i);
		printf("  ");
		record_format(rec, card, width);
		printf("\n");
	}
}

void value_format(ldms_set_t s, enum ldms_value_type type, ldms_mval_t val, size_t n)
{
	ldms_mval_t lval;
	enum ldms_value_type ltype, prev_type;
	int i;
	size_t count;

	switch (type) {
	case LDMS_V_CHAR_ARRAY:
		printf("\"%s\"", val->a_char);
		break;
	case LDMS_V_CHAR:
		printf("'%c'", val->v_char);
		break;
	case LDMS_V_U8:
		printf("%hhu", val->v_u8);
		break;
	case LDMS_V_U8_ARRAY:
		for (i = 0; i < n; i++) {
			if (i)
				printf(",");
			printf("0x%02hhx", val->a_u8[i]);
		}
		break;
	case LDMS_V_S8:
		printf("%hhd", val->v_s8);
		break;
	case LDMS_V_S8_ARRAY:
		for (i = 0; i < n; i++) {
			if (i)
				printf(",");
			printf("%hhd", val->a_s8[i]);
		}
		break;
	case LDMS_V_U16:
		printf("%hu", val->v_u16);
		break;
	case LDMS_V_U16_ARRAY:
		for (i = 0; i < n; i++) {
			if (i)
				printf(",");
			printf("%hu", val->a_u16[i]);
		}
		break;
	case LDMS_V_S16:
		printf("%hd", val->v_s16);
		break;
	case LDMS_V_S16_ARRAY:
		for (i = 0; i < n; i++) {
			if (i)
				printf(",");
			printf("%hd", val->a_s16[i]);
		}
		break;
	case LDMS_V_U32:
		printf("%u", val->v_u32);
		break;
	case LDMS_V_U32_ARRAY:
		for (i = 0; i < n; i++) {
			if (i)
				printf(",");
			printf("%u", val->a_u32[i]);
		}
		break;
	case LDMS_V_S32:
		printf("%d", val->v_s32);
		break;
	case LDMS_V_S32_ARRAY:
		for (i = 0; i < n; i++) {
			if (i)
				printf(",");
			printf("%d", val->a_s32[i]);
		}
		break;
	case LDMS_V_U64:
		printf("%"PRIu64, val->v_u64);
		break;
	case LDMS_V_U64_ARRAY:
		for (i = 0; i < n; i++) {
			if (i)
				printf(",");
			printf("%"PRIu64, val->a_u64[i]);
		}
		break;
	case LDMS_V_S64:
		printf("%"PRId64, val->v_s64);
		break;
	case LDMS_V_S64_ARRAY:
		for (i = 0; i < n; i++) {
			if (i)
				printf(",");
			printf("%"PRId64, val->a_s64[i]);
		}
		break;
	case LDMS_V_F32:
		printf("%f", val->v_f);
		break;
	case LDMS_V_F32_ARRAY:
		for (i = 0; i < n; i++) {
			if (i)
				printf(",");
			printf("%f", val->a_f[i]);
		}
		break;
	case LDMS_V_D64:
		printf("%f", val->v_d);
		break;
	case LDMS_V_D64_ARRAY:
		for (i = 0; i < n; i++) {
			if (i)
				printf(",");
			printf("%f", val->a_d[i]);
		}
		break;
	case LDMS_V_RECORD_TYPE:
		printf("LDMS_V_RECORD_TYPE");
		break;
	case LDMS_V_RECORD_INST:
		/*
		 * Record instances are handled in the record array and
		 * list cases.
		 */
		assert(0);
		break;
	case LDMS_V_RECORD_ARRAY:
		printf("\n");
		record_array_format(s, val);
		break;
	case LDMS_V_LIST:
		prev_type = 0;
		lval = ldms_list_first(s, val, &ltype, &count);
		if (LDMS_V_RECORD_INST == ltype) {
			printf("\n");
			list_record_format(s, val);
		} else {
			printf("[");
			for (lval = ldms_list_first(s, val, &ltype, &count), prev_type=ltype, i = 0;
			     lval; lval = ldms_list_next(s, lval, &ltype, &count), i++) {
				if (i++) {
					if (prev_type != ltype) {
						printf("Error: A list contains entries of different types.\n");
						exit(ENOTSUP);
					}
					printf(",");
				}
				if (ldms_type_is_array(ltype) && ltype != LDMS_V_CHAR_ARRAY)
					printf("[");
				value_format(s, ltype, lval, count);
				if (ldms_type_is_array(ltype) && ltype != LDMS_V_CHAR_ARRAY)
					printf("]");
			}
			printf("]");
			printf("\n");
		}
		break;
	default:
		printf("Unknown metric type");
	}
}

void value_printer(ldms_set_t s, int idx)
{
	enum ldms_value_type type = ldms_metric_type_get(s, idx);
	ldms_mval_t val = ldms_metric_get(s, idx);
	int n = ldms_metric_array_get_len(s, idx);
	value_format(s, type, val, n);
}

static int user_data = 0;
void metric_printer(ldms_set_t s, int i)
{
	enum ldms_value_type type = ldms_metric_type_get(s, i);

	const char *metname, *metunit;
	if (type != LDMS_V_NONE) {
		metname = ldms_metric_name_get(s, i);
		metunit = ldms_metric_unit_get(s, i);
	} else {
		metname = "SET_ERROR";
		metunit = NULL;
	}

	printf("%c %-10s %-42s ",
	       (ldms_metric_flags_get(s, i) & LDMS_MDESC_F_DATA ? 'D' : 'M'),
	       ldms_metric_type_to_str(type), metname);
	if (user_data)
		printf("0x%" PRIx64 " ", ldms_metric_user_data_get(s,i));

	value_printer(s, i);
	if (metunit)
		printf(" %s", metunit);
	printf("\n");
}

static int is_matched(char *inst_name, char *schema_name)
{
	if (LIST_EMPTY(&match_list))
		return 1;

	char *name;
	struct match_str *match;
	if (schema)
		name = schema_name;
	else
		name = inst_name;
	LIST_FOREACH(match, &match_list, entry) {
		if (0 == regexec(&match->regex, name, 0, NULL, 0))
			return 1;
	}
	return 0;
}

static int verbose = 0;
static int long_format = 0;

void print_cb(ldms_t t, ldms_set_t s, int rc, void *arg)
{
	int err;
	unsigned long last = (unsigned long)arg;
	err = LDMS_UPD_ERROR(rc);
	if (err) {
		printf("    Error %x updating metric set.\n", err);
		goto out;
	}
	/* Ignore if more update of this set is expected */
	if (rc & LDMS_UPD_F_MORE)
		return;
	/* If this is a push update and it's not the last, ignore it. */
	if (rc & LDMS_UPD_F_PUSH) {
		if (!(rc & LDMS_UPD_F_PUSH_LAST)) {
			/* This will trigger the last update */
			ldms_xprt_cancel_push(s);
			return;
		}
	}
	struct ldms_timestamp _ts = ldms_transaction_timestamp_get(s);
	struct ldms_timestamp const *ts = &_ts;
	int consistent = ldms_set_is_consistent(s);
	struct tm *tm;
	char dtsz[200];
	time_t ti = ts->sec;
	tm = localtime(&ti);
	strftime(dtsz, sizeof(dtsz), "%a %b %d %H:%M:%S %Y %z", tm);

	printf("%s: %s, last update: %s [%dus] ",
	       ldms_set_instance_name_get(s),
	       (consistent?"consistent":"inconsistent"), dtsz, ts->usec);
	if (rc & LDMS_UPD_F_PUSH)
		printf("PUSH ");
	if (rc & LDMS_UPD_F_PUSH_LAST)
		printf("LAST ");
	printf("\n");
	if (long_format) {
		int i;
		for (i = 0; i < ldms_set_card_get(s); i++)
			metric_printer(s, i);
	}
	if ((rc == 0) || (rc & LDMS_UPD_F_PUSH_LAST))
		ldms_set_delete(s);
 out:
	printf("\n");
	if (last) {
		pthread_mutex_lock(&print_lock);
		print_done = 1;
		pthread_cond_signal(&print_cv);
		pthread_mutex_unlock(&print_lock);
		done = 1;
		pthread_cond_signal(&done_cv);
	}
}

void lookup_cb(ldms_t t, enum ldms_lookup_status status, int more,
	       ldms_set_t s, void *arg);
static const char *ldmsd_group_member_name(const char *info_key)
{
	if (0 != strncmp(GRP_KEY_PREFIX, info_key, sizeof(GRP_KEY_PREFIX)-1))
		return NULL;
	return info_key + sizeof(GRP_KEY_PREFIX) - 1;
}

void lookup_cb(ldms_t t, enum ldms_lookup_status status,
	       int more,
	       ldms_set_t s, void *arg)
{
	unsigned long last = (unsigned long)arg;
	if (status) {
		last = 1;
		pthread_mutex_lock(&print_lock);
		print_done = 1;
		pthread_cond_signal(&print_cv);
		pthread_mutex_unlock(&print_lock);
		goto err;
	}
	ldms_xprt_update(s, print_cb, (void *)(unsigned long)(!more));
	return;
 err:
	printf("ldms_ls: Error %d looking up metric set.\n", status);
	if (status == ENOMEM) {
		printf("Change the LDMS_LS_MEM_SZ environment variable or the "
		       "-m option to a bigger value. The current "
		       "value is %s\n", mem_sz);
	}
	if (last && !more) {
		pthread_mutex_lock(&done_lock);
		done = 1;
		pthread_cond_signal(&done_cv);
		pthread_mutex_unlock(&done_lock);
	}
}

void lookup_push_cb(ldms_t t, enum ldms_lookup_status status,
		    int more,
		    ldms_set_t s, void *arg)
{
	unsigned long last = (unsigned long)arg;
	if (LDMS_UPD_ERROR(status)) {
		/* Lookup failed, signal the main thread to finish up */
		last = 1;
		pthread_mutex_lock(&print_lock);
		print_done = 1;
		pthread_cond_signal(&print_cv);
		pthread_mutex_unlock(&print_lock);
		goto err;
	}
	if (strcmp(GRP_SCHEMA_NAME, ldms_set_schema_name_get(s)) == 0) {
		/*
		 * This is a set group. Don't register for push update otherwise
		 * ldms_ls will wait indefinitely. Get the update by pulling.
		 */
		ldms_xprt_update(s, print_cb, (void *)(unsigned long)(!more));
		return;
	}
	/* Register this set for push updates */
	ldms_xprt_register_push(s, LDMS_XPRT_PUSH_F_CHANGE, print_cb,
					(void *)(unsigned long)(!more));
	return;
 err:
	printf("ldms_ls: Error %d looking up metric set.\n", status);
	if (status == ENOMEM) {
		printf("Change the LDMS_LS_MEM_SZ environment variable or the "
				"-m option to a bigger value. The current "
				"value is %s\n", mem_sz);
	}
	if (last && !more) {
		pthread_mutex_lock(&done_lock);
		done = 1;
		pthread_cond_signal(&done_cv);
		pthread_mutex_unlock(&done_lock);
	}
}

long total_meta;
long total_data;
long total_sets;

void print_set(struct ldms_dir_set_s *set_data)
{
	if (!verbose) {
		printf("%s\n", set_data->inst_name);
	} else {
		if (verbose > 1)
			printf("%-64s ", set_data->digest_str);
		printf("%-14s %-24s %6s %6lu %6lu %6lu %6d %6d %10s %10d.%06d %10d.%06d ",
		       set_data->schema_name,
		       set_data->inst_name,
		       set_data->flags,
		       set_data->meta_size,
		       set_data->data_size,
		       set_data->heap_size,
		       set_data->uid,
		       set_data->gid,
		       set_data->perm,
		       set_data->timestamp.sec,
		       set_data->timestamp.usec,
		       set_data->duration.sec,
		       set_data->duration.usec);
		total_meta += set_data->meta_size;
		total_data += set_data->data_size;
		total_sets ++;
		int j;
		for (j = 0; j < set_data->info_count; j++) {
			printf("\"%s\"=\"%s\" ", set_data->info[j].key, set_data->info[j].value);
		}
		printf("\n");
	}
}

static int add_set(struct ldms_dir_set_s *set_data)
{
	struct ls_set *lss;
	LIST_FOREACH(lss, &set_list, entry) {
		if (0 == strcmp(lss->set_data->inst_name, set_data->inst_name)) {
			/*
			 * Already in the list.
			 *
			 * This could happen if the set is added because
			 * it is a member of a set group and is added
			 * after all dirs have been delivered.
			 */
			return EEXIST;
		}
	}
	lss = calloc(1, sizeof(struct ls_set));
	if (!lss) {
		return ENOMEM;
	}
	lss->set_data = set_data;
	LIST_INSERT_HEAD(&set_list, lss, entry);
	return 0;
}

void __add_dir(ldms_dir_t dir)
{
	struct ldms_ls_dir *lsdir;
	lsdir = malloc(sizeof(*lsdir));
	if (!lsdir) {
		dir_status = ENOMEM;
		return;
	}
	lsdir->dir = dir;
	LIST_INSERT_HEAD(&dir_list, lsdir, entry);
}

void dir_cb(ldms_t t, int status, ldms_dir_t _dir, void *cb_arg)
{
	int i;
	int more;
	if (status) {
		dir_status = status;
		goto wakeup;
	}
	more = _dir->more;

	__add_dir(_dir);
	for (i = 0; i < _dir->set_count; i++) {
		if (is_matched(_dir->set_data[i].inst_name, _dir->set_data[i].schema_name)) {
			/*
			 * Always add matched sets to the set list.
			 *
			 * After the dir is done
			 * the set_list is checked whether it is empty or not,
			 * to print an error message in case there are no
			 * sets matched the given criteria.
			 */
			dir_status = add_set(&_dir->set_data[i]);
			if (verbose || (!verbose && !long_format))
				print_set(&_dir->set_data[i]);
		}
	}

	if (more)
		return;

 wakeup:
	dir_done = 1;
	pthread_cond_signal(&dir_cv);
}

void ldms_connect_cb(ldms_t x, ldms_xprt_event_t e, void *cb_arg)
{
	switch (e->type) {
	case LDMS_XPRT_EVENT_ERROR:
	case LDMS_XPRT_EVENT_REJECTED:
		printf("Connection failed/rejected.\n");
		done = 1;
		/* let-through */
	case LDMS_XPRT_EVENT_CONNECTED:
	case LDMS_XPRT_EVENT_DISCONNECTED:
		sem_post(&conn_sem);
		break;
	case LDMS_XPRT_EVENT_RECV:
	case LDMS_XPRT_EVENT_SEND_COMPLETE:
	case LDMS_XPRT_EVENT_SET_DELETE:
		/* ignore */
		break;
	default:
		assert(0 == "Unknown event.");
		break;
	}

}

const char *repeat(char c, size_t count)
{
	int i;
	static char buf[1024];
	count = count >= 1024 ? 1023 : count;
	for (i = 0; i < count; i++)
		buf[i] = c;
	buf[i] = '\0';
	return buf;
}

struct ldms_dir_set_s *find_set_data_in_dirs(const char *inst_name)
{
	struct ldms_ls_dir *lsdir;
	int i;

	LIST_FOREACH(lsdir, &dir_list, entry) {
		for (i = 0; i < lsdir->dir->set_count; i++) {
			if (0 == strcmp(lsdir->dir->set_data[i].inst_name, inst_name))
				return &lsdir->dir->set_data[i];
		}
	}
	return NULL;
}

int is_in_set_list(const char *name)
{
	struct ls_set *lss;
	LIST_FOREACH(lss, &set_list, entry) {
		if (0 == strcmp(lss->set_data->inst_name, name)) {
			/*
			 * Already in the list.
			 *
			 * This could happen if the set is added because
			 * it is a member of a set group and is added
			 * after all dirs have been delivered.
			 */
			return 1;
		}
	}
	return 0;
}

int main(int argc, char *argv[])
{
	struct ldms_version version;
	struct sockaddr_in sin;
	ldms_t ldms;
	int ret;
	struct hostent *h;
	char *hostname = strdup("localhost");
	unsigned short port_no = LDMS_DEFAULT_PORT;
	int op;
	int i;
	char *xprt = strdup("sock");
	int waitsecs = 10;
	int regex = 0;
	ldms_lookup_cb_t lu_cb_fn = lookup_cb;
	struct timespec ts;
	char *lval, *rval;
	struct ldms_ls_dir *dir;

	/* If no arguments are given, print usage. */
	if (argc == 1)
		usage(argv);
	int rc = sem_init(&conn_sem, 0, 0);
	if (rc) {
		perror("sem_init");
		_exit(-1);
	}

	auth_opt = av_new(auth_opt_max);
	if (!auth_opt) {
		printf("ERROR: Not enough memory");
		exit(1);
	}

	opterr = 0;
	long ptmp;
	while ((op = getopt(argc, argv, FMT)) != -1) {
		switch (op) {
		case 'E':
			regex = 1;
			break;
		case 'S':
			schema = 1;
			break;
		case 'I':
			schema = 0;
			break;
		case 'h':
			free(hostname);
			hostname = strdup(optarg);
			if (!hostname) {
				printf("ERROR: out of memory\n");
				exit(1);
			}
			break;
		case 'p':
			ptmp = atol(optarg);
			if (ptmp < 1 || ptmp > USHRT_MAX) {
				printf("ERROR: -p %s invalid port number\n", optarg);
				exit(1);
			}
			port_no = ptmp;
			break;
		case 'l':
			long_format = 1;
			break;
		case 'u':
			user_data = 1;
			break;
		case 'v':
			verbose++;
			break;
		case 'x':
			free(xprt);
			xprt = strdup(optarg);
			if (!xprt) {
				printf("ERROR: out of memory\n");
				exit(1);
			}
			break;
		case 'w':
			waitsecs = atoi(optarg);
			break;
		case 'm':
			mem_sz = strdup(optarg);
			if (!mem_sz) {
				printf("ERROR: out of memory\n");
				exit(1);
			}
			break;
		case 'V':
			ldms_version_get(&version);
			printf("LDMS_LS Version: %s\n", PACKAGE_VERSION);
			printf("LDMS Protocol Version: %hhu.%hhu.%hhu.%hhu\n",
							version.major,
							version.minor,
							version.patch,
							version.flags);
			exit(0);
			break;
		case 'P':
			lu_cb_fn = lookup_push_cb;
			long_format = 1;
			break;
		case 'a':
			auth_name = optarg;
			break;
		case 'A':
			lval = strtok(optarg, "=");
			rval = strtok(NULL, "");
			if (!lval || !rval) {
				printf("ERROR: Expecting -A name=value");
				exit(1);
			}
			if (auth_opt->count == auth_opt->size) {
				printf("ERROR: Too many auth options");
				exit(1);
			}
			auth_opt->list[auth_opt->count].name = lval;
			auth_opt->list[auth_opt->count].value = rval;
			auth_opt->count++;
			break;
		default:
			printf("%s: unknown option %c\n", argv[0], optopt);
			usage(argv);
		}
	}

	h = gethostbyname(hostname);
	if (!h) {
		herror(argv[0]);
		printf("%s: %s does not resolve.\n", argv[0], hostname);
		exit(1);
	}

	if (h->h_addrtype != AF_INET) {
		printf("%s: -h %s does not provide an AF_INET address.\n",
			argv[0], hostname);
		printf("%s: please give a proper hostname.\n", argv[0]);
		exit(1);
	}

	/* Initialize LDMS */
	if (!mem_sz) {
		mem_sz = getenv(LDMS_LS_MEM_SZ_ENVVAR);
		if (!mem_sz)
			mem_sz = LDMS_LS_MAX_MEM_SZ_STR;
	}
	max_mem_size = ovis_get_mem_size(mem_sz);
	if (!max_mem_size) {
		printf("Invalid memory size '%s'. "
			"See the -m option in the ldms_ls help.\n",
							mem_sz);
		usage(argv);
	}
	if (ldms_init(max_mem_size)) {
		printf("LDMS could not pre-allocate the memory of size %s.\n",
		       mem_sz);
		exit(1);
	}

	ldms = ldms_xprt_new_with_auth(xprt, auth_name, auth_opt);
	if (!ldms) {
		printf("Error creating transport.\n");
		exit(1);
	}

	memset(&sin, 0, sizeof sin);
	sin.sin_addr.s_addr = *(unsigned int *)(h->h_addr_list[0]);
	sin.sin_family = h->h_addrtype;
	sin.sin_port = htons(port_no);
	if (verbose > 1) {
		printf("Hostname    : %s\n", hostname);
		printf("IP Address  : %s\n", inet_ntoa(sin.sin_addr));
		printf("Port        : %hu\n", port_no);
		printf("Transport   : %s\n", xprt);
	}
	free(xprt);
	free(hostname);
	xprt = NULL;
	hostname = NULL;
	ret  = ldms_xprt_connect(ldms, (struct sockaddr *)&sin, sizeof(sin),
				 ldms_connect_cb, NULL);
	if (ret) {
		perror("ldms_xprt_connect");
		exit(2);
	}

	sem_wait(&conn_sem);
	if (done) {
		/* Connection error/rejected */
		exit(1);
	}
	pthread_mutex_init(&dir_lock, 0);
	pthread_cond_init(&dir_cv, NULL);
	pthread_mutex_init(&done_lock, 0);
	pthread_cond_init(&done_cv, NULL);
	pthread_mutex_init(&print_lock, 0);
	pthread_cond_init(&print_cv, NULL);

	int is_filter_list = 0;

	if (verbose) {
		if (verbose > 1)
			printf("%-*s ", 64, "Schema Digest");
		printf("%-*s %-*s %-*s %-*s %-*s %-*s %-*s %-*s %-*s %-*s %-*s %-*s\n",
		       14, "Schema",
		       24, "Instance",
		       6, "Flags",
		       6, "Msize", 6, "Dsize", 6, "Hsize",
		       6, "UID", 6, "GID", 10, "Perm",
		       17, "Update", 17, "Duration", 8, "Info");
		if (verbose > 1)
			printf("---------------------------------------------------------------- ");
		printf("-------------- ------------------------ ------ ------ "
		       "------ ------ ------ ------ ---------- ----------------- "
		       "----------------- --------\n");
	}
	if (optind == argc) {
		/* List all existing metric sets */
		ret = ldms_xprt_dir(ldms, dir_cb, NULL, 0);
		if (ret) {
			printf("ldms_dir returned synchronous error %d\n",
			      ret);
			exit(1);
		}
	} else {
		is_filter_list = 1;
		/*
		 * List the metric sets that the instance name or
		 * schema name matched the given criteria.
		 */
		struct match_str *match;
		for (i = optind; i < argc; i++) {
			match = malloc(sizeof(*match));
			if (!match) {
				perror("ldms: ");
				exit(2);
			}
			if (!regex) {
				/* Take the given string literally */
				match->str = malloc(strlen(argv[i]) + 3);
				if (!match->str) {
					perror("ldms: ");
					exit(2);
				}
				sprintf(match->str, "^%s$", argv[i]);
			} else {
				/* Take the given string as regular expression */
				match->str = strdup(argv[i]);
				if (!match->str) {
					perror("ldms: ");
					exit(2);
				}
			}
			ret = __compile_regex(&match->regex, match->str);
			if (ret)
				exit(1);
			LIST_INSERT_HEAD(&match_list, match, entry);
		}
		ret = ldms_xprt_dir(ldms, dir_cb, NULL, 0);
		if (ret) {
			printf("ldms_dir returned synchronous "
			       "error %d\n", ret);
			exit(1);
		}
	}

	clock_gettime(CLOCK_REALTIME, &ts);
	ts.tv_sec += waitsecs;
	pthread_mutex_lock(&dir_lock);
	while (!dir_done)
		ret = pthread_cond_timedwait(&dir_cv, &dir_lock, &ts);
	pthread_mutex_unlock(&dir_lock);
	if (ret)
		server_timeout();

	if (dir_status) {
		printf("Error %d looking up the metric set directory.\n",
		       dir_status);
		exit(3);
	}

	struct ls_set *lss;
	if (LIST_EMPTY(&set_list)) {
		if (is_filter_list)
			printf("ldms_ls: No metric sets matched the given criteria\n");
		done = 1;
		goto done;
	}

	/* Take care of set groups */
	char *name;
	ldms_key_value_t info;
	struct ldms_dir_set_s *set_data;
	LIST_FOREACH(lss, &set_list, entry) {
		if (0 == strcmp(GRP_SCHEMA_NAME, lss->set_data->schema_name)) {
			info = lss->set_data->info;
			for (i = 0; i < lss->set_data->info_count; i++) {
				name = (char *)ldmsd_group_member_name(info[i].key);
				if (!name)
					continue;

				set_data = find_set_data_in_dirs(name);
				if (set_data && !is_in_set_list(set_data->inst_name)) {
					rc = add_set(set_data);
					if (!rc) {
						if (verbose || (!verbose && !long_format))
							print_set(set_data);
					}
				} else {
					/*
					 * do nothing.
					 *
					 * It is possible that the LDMS set
					 * does not exist although it is a
					 * member of a set group which
					 * is manually created by users.
					 */
				}
			}
		}
	}

	if (verbose) {
		if (verbose > 1)
			printf("---------------------------------------------------------------- ");
		printf("-------------- ------------------------ ------ ------ "
		       "------ ------ ------ ------ ---------- ----------------- "
		       "----------------- --------\n");
		printf("Total Sets: %ld, Meta Data (kB): %.2f, Data (kB) %.2f, Memory (kB): %.2f\n",
		       total_sets, (double)total_meta / 1000.0, (double)total_data / 1000.0,
		       (double)(total_meta + total_data) / 1000.0);
	}

	/*
	 * At this point,
	 * - set_list contains all and only set data
	 *   that are matched the criteria including the members of
	 *   set groups.
	 * - the set instance name and/or the information of all sets
	 *   are printed.
	 */

	if (!long_format) {
		done = 1;
		goto done;
	}

	if (verbose && long_format)
		printf("\n=======================================================================\n\n");

	/*
	 * Handle the long format (-l)
	 */
	while (!LIST_EMPTY(&set_list)) {
		lss = LIST_FIRST(&set_list);
		LIST_REMOVE(lss, entry);

		pthread_mutex_lock(&print_lock);
		print_done = 0;
		pthread_mutex_unlock(&print_lock);

		ret = ldms_xprt_lookup(ldms, lss->set_data->inst_name,
				       LDMS_LOOKUP_BY_INSTANCE,
				       lu_cb_fn,
				       (void *)(unsigned long)
				       LIST_EMPTY(&set_list));
		if (ret) {
			printf("ldms_xprt_lookup returned %d for set '%s'\n",
			       ret, lss->set_data->inst_name);
		}
		pthread_mutex_lock(&print_lock);
		while (!print_done)
			pthread_cond_wait(&print_cv, &print_lock);
		pthread_mutex_unlock(&print_lock);
		free(lss);
	}
	done = 1;
done:
	pthread_mutex_lock(&done_lock);
	while (!done)
		pthread_cond_wait(&done_cv, &done_lock);
	pthread_mutex_unlock(&done_lock);

	while ((dir = LIST_FIRST(&dir_list))) {
		LIST_REMOVE(dir, entry);
		ldms_xprt_dir_free(ldms, dir->dir);
		free(dir);
	}

	/* gracefully close, and wait at most 2 seconds before exit */
	ldms_xprt_close(ldms);
	struct timespec _t;
	clock_gettime(CLOCK_REALTIME, &_t);
	_t.tv_sec += 2;
	sem_timedwait(&conn_sem, &_t);

	ldms_xprt_term(1);
	exit(0);
}
