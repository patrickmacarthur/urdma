/* volt_throughput_client.c
 * Patrick MacArthur <patrick@patrickmacarthur.net>
 */

/*
 * Copyright (c) 2010 Intel Corporation.  All rights reserved.
 *
 * This software is available to you under the OpenIB.org BSD license
 * below:
 *
 *     Redistribution and use in source and binary forms, with or
 *     without modification, are permitted provided that the following
 *     conditions are met:
 *
 *      - Redistributions of source code must retain the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer.
 *
 *      - Redistributions in binary form must reproduce the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer in the documentation and/or other materials
 *        provided with the distribution.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AWV
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <netdb.h>
#include <errno.h>
#include <getopt.h>
#include <pthread.h>
#include <rdma/rdma_cma.h>
#include <rdma/rdma_verbs.h>
#include "verbs.h"

struct lock_announce_message {
	uint64_t lock_addr;
	uint32_t lock_rkey;
};

static const char *server = "127.0.0.1";
static const char *port = "7471";

static struct ibv_qp_init_attr qp_init_attr_template = {
	.cap.max_send_wr = 1,
	.cap.max_recv_wr = 1,
	.cap.max_send_sge = 1,
	.cap.max_recv_sge = 1,
	.cap.max_inline_data = 16,
	.qp_context = NULL,
	.sq_sig_all = 1,
};

static struct rdma_addrinfo *addr_info;

static double timespec_diff(struct timespec *start, struct timespec *end)
{
	static const unsigned long ns_per_s = 1000000000L;
	int carry = (end->tv_nsec < start->tv_nsec) ? 1 : 0;
	return (end->tv_sec - start->tv_sec - carry)
		+ (carry * ns_per_s + end->tv_nsec - start->tv_nsec)
		/ (double)ns_per_s;
}

static int client_thread()
{
	struct timespec start_time, end_time;
	struct ibv_qp_init_attr attr;
	struct rdma_cm_id *id;
	struct ibv_mr *mr, *send_mr;
	struct lock_announce_message lock_msg;
	struct ibv_wc wc;
	uint32_t lock_status;
	int send_flags, ret;

	memcpy(&attr, &qp_init_attr_template, sizeof attr);
	attr.qp_context = id;
	ret = rdma_create_ep(&id, addr_info, NULL, &attr);
	// Check to see if we got inline data allowed or not
	if (attr.cap.max_inline_data >= 16)
		send_flags = IBV_SEND_INLINE;
	else
		printf("rdma_client: device doesn't support IBV_SEND_INLINE, "
		       "using sge sends\n");

	if (ret) {
		perror("rdma_create_ep");
		goto out;
	}

	mr = rdma_reg_msgs(id, &lock_msg, sizeof(lock_msg));
	if (!mr) {
		perror("rdma_reg_msgs for lock_msg");
		ret = -1;
		goto out_destroy_ep;
	}

	ret = rdma_post_recv(id, NULL, &lock_msg, sizeof(lock_msg), mr);
	if (ret) {
		perror("rdma_post_recv");
		goto out_dereg_send;
	}

	ret = rdma_connect(id, NULL);
	if (ret) {
		perror("rdma_connect");
		goto out_dereg_send;
	}

	while ((ret = rdma_get_recv_comp(id, &wc)) == 0);
	if (ret < 0) {
		perror("rdma_get_recv_comp");
		goto out_disconnect;
	}

	clock_gettime(CLOCK_MONOTONIC, &start_time);
	for (int i = 0; i < 1000000; i++) {
		ret = urdma_remote_lock(id->qp, &lock_status,
					lock_msg.lock_addr,
					lock_msg.lock_rkey, NULL);
		if (ret < 0) {
			fprintf(stderr, "urdma_remote_lock: %s\n", strerror(-ret));
			goto out_disconnect;
		}

		while ((ret = rdma_get_send_comp(id, &wc)) == 0);
		if (ret < 0) {
			perror("rdma_get_send_comp");
			goto out_disconnect;
		}
		if (wc.status != IBV_WC_SUCCESS) {
			fprintf(stderr, "Client 1 Got unexpected wc status %x not 0\n",
					wc.status);
			break;
		}
		if (wc.opcode != 255) {
			fprintf(stderr, "Client 1 Got unexpected wc opcode %d not 255\n",
					wc.opcode);
			break;
		}

		ret = urdma_remote_unlock(id->qp, &lock_status,
					  lock_msg.lock_addr, lock_msg.lock_rkey,
					  NULL);
		if (ret < 0) {
			fprintf(stderr, "urdma_remote_unlock: %s\n", strerror(-ret));
			goto out_disconnect;
		}

		while ((ret = rdma_get_send_comp(id, &wc)) == 0);
		if (ret < 0) {
			perror("rdma_get_send_comp");
			goto out_disconnect;
		}
		if (wc.status != IBV_WC_SUCCESS) {
			fprintf(stderr, "Client 1 Got unexpected wc status %x not 0\n",
					wc.status);
		}
		if (wc.opcode != 255) {
			fprintf(stderr, "Client 1 Got unexpected wc opcode %d not 255\n",
					wc.opcode);
		}
	}
	clock_gettime(CLOCK_MONOTONIC, &end_time);

	printf("client done; took %0.3lf seconds\n",
			timespec_diff(&start_time, &end_time));
	fflush(stdout);


out_disconnect:
	rdma_disconnect(id);
out_dereg_send:
	if ((send_flags & IBV_SEND_INLINE) == 0)
		rdma_dereg_mr(send_mr);
out_dereg_recv:
	rdma_dereg_mr(mr);
out_destroy_ep:
	rdma_destroy_ep(id);
out:
	return ret;
}

static int run(void)
{
	struct rdma_addrinfo hints;
	pthread_t tid1, tid2;
	int ret;

	memset(&hints, 0, sizeof hints);
	hints.ai_port_space = RDMA_PS_TCP;
	ret = rdma_getaddrinfo(server, port, &hints, &addr_info);
	if (ret) {
		printf("rdma_getaddrinfo: %s\n", gai_strerror(ret));
		goto out;
	}

	client_thread();

out:
	return 0;
}

int main(int argc, char **argv)
{
	int op, ret;

	while ((op = getopt(argc, argv, "s:p:")) != -1) {
		switch (op) {
		case 's':
			server = optarg;
			break;
		case 'p':
			port = optarg;
			break;
		default:
			printf("usage: %s\n", argv[0]);
			printf("\t[-s server_address]\n");
			printf("\t[-p port_number]\n");
			exit(1);
		}
	}

	printf("rdma_client: start\n");
	ret = run();
	printf("rdma_client: end %d\n", ret);
	return ret;
}
