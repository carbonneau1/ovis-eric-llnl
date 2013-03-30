/* -*- c-basic-offset: 8 -*-
 * Copyright (c) 2010 Open Grid Computing, Inc. All rights reserved.
 * Copyright (c) 2010 Sandia Corporation. All rights reserved.
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
 *      Neither the name of the Network Appliance, Inc. nor the names of
 *      its contributors may be used to endorse or promote products
 *      derived from this software without specific prior written
 *      permission.
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
 *
 * Author: Tom Tucker <tom@opengridcomputing.com>
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
#include "ldms.h"
#include "ldms_xprt.h"

static pthread_mutex_t dir_lock;
static pthread_cond_t dir_cv;
static int dir_done;
static int dir_status;

static pthread_mutex_t done_lock;
static pthread_cond_t done_cv;
static int done;

struct ls_set {
	char *name;
	LIST_ENTRY(ls_set) entry;
};
LIST_HEAD(set_list, ls_set) set_list;

#define FMT "h:p:x:w:lv"
void usage(char *argv[])
{
	printf("%s -h <hostname> -x <transport> [ set_name ... ]\n"
	       "\n    -h <hostname>    The name of the host to query. Default is localhost.\n"
	       "\n    -p <port_num>    The port number. The default is 50000.\n"
	       "\n    -l               Show the values of the metrics in each metric set.\n"
	       "\n    -x <name>        The transport name: sock, rdma, or local. Default is\n"
	       "                       localhost unless -h is specified in which case it is sock.\n"
	       "\n    -w <secs>        The time to wait before giving up on the server.\n"
	       "                       The default is 10 seconds.\n"
	       "\n    -v               Show detail information about the metric set. Specifying\n"
	       "                     this option multiple times increases the verbosity.\n",
	       argv[0]);
	exit(1);
}

void server_timeout(void)
{
	printf("A timeout occurred waiting for a response from the server.\n"
	       "Use the -w option to specify the amount of time to wait "
	       "for the server\n");
	exit(1);
}

void metric_printer(struct ldms_value_desc *vd, union ldms_value *v, void *arg)
{
	char value_str[64];
	printf("%4s ", ldms_type_to_str(vd->type));

	switch (vd->type) {
	case LDMS_V_U8:
		sprintf(value_str, "%hhu", v->v_u8);
		break;
	case LDMS_V_S8:
		sprintf(value_str, "%hhd", v->v_s8);
		break;
	case LDMS_V_U16:
		sprintf(value_str, "%hu", v->v_u16);
		break;
	case LDMS_V_S16:
		sprintf(value_str, "%hd", v->v_s16);
		break;
	case LDMS_V_U32:
		sprintf(value_str, "%8u", v->v_u32);
		break;
	case LDMS_V_S32:
		sprintf(value_str, "%d", v->v_s32);
		break;
	case LDMS_V_U64:
		sprintf(value_str, "%" PRIu64, v->v_u64);
		break;
	case LDMS_V_S64:
		sprintf(value_str, "%" PRId64, v->v_s64);
		break;
	case LDMS_V_F:
		sprintf(value_str, "%f", v->v_d);
		break;
	case LDMS_V_D:
		sprintf(value_str, "%f", v->v_d);
		break;
	case LDMS_V_LD:
		sprintf(value_str, "%Lf", v->v_ld);
		break;
	}
	printf("%-16s %s\n", value_str, vd->name);
}
void print_detail(ldms_set_t s)
{
	struct ldms_set_desc *sd = s;

	printf("  METADATA --------\n");
	printf("             Size : %" PRIu32 "\n", sd->set->meta->meta_size);
	printf("            Inuse : %" PRIu32 "\n", sd->set->meta->tail_off);
	printf("     Metric Count : %" PRIu32 "\n", sd->set->meta->card);
	printf("               GN : %" PRIu64 "\n", sd->set->meta->meta_gn);
	printf("  DATA ------------\n");
	printf("             Size : %" PRIu32 "\n", sd->set->meta->data_size);
	printf("            Inuse : %" PRIu32 "\n", sd->set->data->tail_off);
	printf("               GN : %" PRIu64 "\n", sd->set->data->gn);
	printf("  -----------------\n");
}

static int verbose = 0;
static int long_format = 0;

void print_cb(ldms_t t, ldms_set_t s, int rc, void *arg)
{
	unsigned long last = (unsigned long)arg;
	printf("%s\n", ldms_get_set_name(s));
	if (rc) {
		printf("    Error %d updating metric set.\n", rc);
		goto out;
	}
	if (verbose)
		print_detail(s);
	if (long_format)
		ldms_visit_metrics(s, metric_printer, NULL);

	ldms_destroy_set(s);
 out:
	printf("\n");
	if (last) {
		done = 1;
		pthread_cond_signal(&done_cv);
	}
}

void lookup_cb(ldms_t t, enum ldms_lookup_status status,
	       ldms_set_t s, void *arg)
{
	unsigned long last = (unsigned long)arg;
	if (status)
		goto err;

	ldms_update(s, print_cb, (void *)last);
	return;
 err:
	printf("ldms_ls: Error %d looking up metric set.\n", status);
	if (last) {
		pthread_cond_signal(&done_cv);
		done = 1;
		done = status;
	}
}

static void add_set(char *name)
{
	struct ls_set *lss;

	lss = calloc(1, sizeof(struct ls_set));
	if (!lss) {
		dir_status = ENOMEM;
		return;
	}
	lss->name = strdup(name);
	LIST_INSERT_HEAD(&set_list, lss, entry);
}

void add_set_list(ldms_t t, ldms_dir_t _dir)
{
	int i;
	for (i = 0; i < _dir->set_count; i++)
		add_set(_dir->set_names[i]);
}

void dir_cb(ldms_t t, int status, ldms_dir_t _dir, void *cb_arg)
{
	int more;
	if (status) {
		dir_status = status;
		goto wakeup;
	}
	more = _dir->more;
	add_set_list(t, _dir);
	ldms_dir_release(t, _dir);
	if (more)
		return;

 wakeup:
	if (!verbose && !long_format)
		done = 1;
	dir_done = 1;
	pthread_cond_signal(&dir_cv);
}

void null_log(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vfprintf(stdout, fmt, ap);
	fflush(stdout);

	// print nothing at all!!!!
}

int main(int argc, char *argv[])
{
	struct sockaddr_in sin;
	ldms_t ldms;
	int ret;
	struct hostent *h;
	u_char *oc;
	char *hostname = "localhost";
	short port_no = LDMS_DEFAULT_PORT;
	int op;
	int i;
	char *xprt = "local";
	int waitsecs = 10;
	struct timespec ts;

	opterr = 0;
	while ((op = getopt(argc, argv, FMT)) != -1) {
		switch (op) {
		case 'h':
			hostname = strdup(optarg);
			if (strcmp(xprt, "local")==0)
				xprt = "sock";
			break;
		case 'p':
			port_no = atoi(optarg);
			break;
		case 'l':
			long_format = 1;
			break;
		case 'v':
			verbose++;
			break;
		case 'x':
			xprt = strdup(optarg);
			break;
		case 'w':
			waitsecs = atoi(optarg);
			break;
		default:
			usage(argv);
		}
	}
	/* If they specify a host name change the default transport to socket */
	if (0 != strcmp(hostname, "localhost") && 0 == strcmp(xprt, "local"))
		xprt = "sock";
	h = gethostbyname(hostname);
	if (!h) {
		herror(argv[0]);
		usage(argv);
	}

	if (h->h_addrtype != AF_INET)
		usage(argv);
	oc = (u_char *)h->h_addr_list[0];

	ldms = ldms_create_xprt(xprt, null_log);
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
	ret  = ldms_connect(ldms, (struct sockaddr *)&sin, sizeof(sin));
	if (ret) {
		perror("ldms_ls");
		exit(2);
	}

	pthread_mutex_init(&dir_lock, 0);
	pthread_cond_init(&dir_cv, NULL);
	pthread_mutex_init(&done_lock, 0);
	pthread_cond_init(&done_cv, NULL);

	if (optind == argc) {
		ret = ldms_dir(ldms, dir_cb, NULL, 0);
		if (ret) {
			printf("ldms_dir returned synchronous error %d\n",
			      ret);
			exit(1);
		}
	} else {
		if (!verbose && !long_format)
			usage(argv);
		/*
		 * Set list specified on the command line. Dummy up a
		 * directory and call our ldms_dir callback
		 * function
		 */
		struct ldms_dir_s *dir =
			calloc(1, sizeof(*dir) +
			       ((argc - optind) * sizeof (char *)));
		if (!dir) {
			perror("ldms: ");
			exit(2);
		}
		dir->set_count = argc - optind;
		dir->type = LDMS_DIR_LIST;
		for (i = optind; i < argc; i++)
			dir->set_names[i - optind] = strdup(argv[i]);
		add_set_list(ldms, dir);
		dir_done = 1;	/* no need to wait */
		dir_status = 0;
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
	while (!LIST_EMPTY(&set_list)) {

		lss = LIST_FIRST(&set_list);
		LIST_REMOVE(lss, entry);

		if (verbose || long_format) {
			ret = ldms_lookup(ldms, lss->name, lookup_cb,
					  (void *)(unsigned long)
					  LIST_EMPTY(&set_list));
			if (ret) {
				printf("ldms_lookup returned %d for set '%s'\n",
				       ret, lss->name);
			}
		} else
			printf("%s\n", lss->name);
	}
	pthread_mutex_lock(&done_lock);
	while (!done)
		pthread_cond_wait(&done_cv, &done_lock);
	pthread_mutex_unlock(&done_lock);

	ldms_close(ldms);
	exit(0);
}
