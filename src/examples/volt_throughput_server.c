/* volt_throughput_server.c
 * Patrick MacArthur <patrick@patrickmacarthur.net>
 */

/*
 * Copyright (c) 2005-2009 Intel Corporation.  All rights reserved.
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
#include <errno.h>
#include <getopt.h>
#include <netdb.h>
#include <infiniband/ib.h>
#include <rdma/rdma_cma.h>
#include <rdma/rdma_verbs.h>
#include "verbs.h"

static const char *server = "127.0.0.1";
static const char *port = "7471";

static uint8_t lock_storage[8];
static uint8_t recv_msg[16];

struct lock_announce_message {
	uint64_t lock_addr;
	uint32_t lock_rkey;
};

static void *agent_thread(void *arg)
{
	struct sockaddr *sa;
	struct rdma_cm_id *id = arg;
	struct lock_announce_message lock_msg;
	struct ibv_qp_init_attr init_attr;
	struct ibv_qp_attr qp_attr;
	struct ibv_mr *lock_mr, *send_mr, *recv_mr;
	struct ibv_wc wc;
	char peerhost[40], peerport[6];
	int socklen, send_flags, ret;

	sa = rdma_get_peer_addr(id);
	switch (sa->sa_family) {
	case AF_INET:
		socklen = sizeof(struct sockaddr_in);
		break;
	case AF_INET6:
		socklen = sizeof(struct sockaddr_in6);
		break;
	case AF_IB:
		socklen = sizeof(struct sockaddr_ib);
		break;
	default:
		socklen = sizeof(struct sockaddr);
		break;
	}
	getnameinfo(sa, socklen, peerhost, sizeof(peerhost),
			peerport, sizeof(peerport),
			NI_NUMERICHOST|NI_NUMERICSERV);
	printf("Got connect request from client: %s:%s\n", peerhost, peerport);

	memset(&qp_attr, 0, sizeof qp_attr);
	memset(&init_attr, 0, sizeof init_attr);
	ret = ibv_query_qp(id->qp, &qp_attr, IBV_QP_CAP,
			   &init_attr);
	if (ret) {
		perror("ibv_query_qp");
		goto out_destroy_accept_ep;
	}
	if (init_attr.cap.max_inline_data >= 16)
		send_flags = IBV_SEND_INLINE;
	else
		printf("rdma_server: device doesn't support IBV_SEND_INLINE, "
		       "using sge sends\n");

	lock_msg.lock_addr = (uintptr_t)&lock_storage;
	lock_mr = rdma_reg_write(id, &lock_storage, sizeof(lock_storage));
	if (!lock_mr) {
		perror("rdma_reg_write");
		goto out_destroy_accept_ep;
	}
	lock_msg.lock_rkey = lock_mr->rkey;

	recv_mr = rdma_reg_msgs(id, recv_msg, 16);
	if (!recv_mr) {
		ret = -1;
		perror("rdma_reg_msgs for recv_msg");
		goto out_dereg_lock;
	}
	if ((send_flags & IBV_SEND_INLINE) == 0) {
		send_mr = rdma_reg_msgs(id, &lock_msg, 16);
		if (!send_mr) {
			ret = -1;
			perror("rdma_reg_msgs for lock_msg");
			goto out_dereg_recv;
		}
	}

	ret = rdma_post_recv(id, NULL, recv_msg, 16, recv_mr);
	if (ret) {
		perror("rdma_post_recv");
		goto out_dereg_send;
	}

	ret = rdma_accept(id, NULL);
	if (ret) {
		perror("rdma_accept");
		goto out_dereg_send;
	}

	ret = rdma_post_send(id, NULL, &lock_msg, sizeof(lock_msg), send_mr, send_flags);

	while ((ret = rdma_get_send_comp(id, &wc)) == 0);
	if (ret < 0) {
		perror("rdma_get_send_comp");
		goto out_disconnect;
	}

	do {
		while ((ret = rdma_get_recv_comp(id, &wc)) == 0);
		if (wc.status != IBV_WC_SUCCESS) {
			if (wc.status != IBV_WC_WR_FLUSH_ERR) {
				fprintf(stderr, "got unexpected WC result from client %s:%s: %s\n",
						peerhost, peerport,
						ibv_wc_status_str(wc.status));
			}
			break;
		}
	} while (ret >= 0);
	if (ret < 0) {
		perror("rdma_get_recv_comp");
		goto out_disconnect;
	}

out_disconnect:
	rdma_disconnect(id);
	printf("Got disconnect from client: %s:%s\n", peerhost, peerport);
out_dereg_send:
	if ((send_flags & IBV_SEND_INLINE) == 0)
		rdma_dereg_mr(send_mr);
out_dereg_recv:
	rdma_dereg_mr(recv_mr);
out_dereg_lock:
	rdma_dereg_mr(lock_mr);
out_destroy_accept_ep:
	rdma_destroy_ep(id);

	return NULL;
}

static int run(void)
{
	struct rdma_addrinfo hints, *res;
	struct ibv_qp_init_attr init_attr;
	struct rdma_cm_id *id, *listen_id;
	pthread_attr_t thread_attr;
	pthread_t tid;
	int ret;

	memset(&hints, 0, sizeof hints);
	hints.ai_flags = RAI_PASSIVE;
	hints.ai_port_space = RDMA_PS_TCP;
	ret = rdma_getaddrinfo(server, port, &hints, &res);
	if (ret) {
		printf("rdma_getaddrinfo: %s\n", gai_strerror(ret));
		return ret;
	}

	memset(&init_attr, 0, sizeof init_attr);
	init_attr.cap.max_send_wr = init_attr.cap.max_recv_wr = 1;
	init_attr.cap.max_send_sge = init_attr.cap.max_recv_sge = 1;
	init_attr.cap.max_inline_data = 16;
	init_attr.sq_sig_all = 1;
	ret = rdma_create_ep(&listen_id, res, NULL, &init_attr);
	if (ret) {
		perror("rdma_create_ep");
		goto out_free_addrinfo;
	}

	ret = rdma_listen(listen_id, 64);
	if (ret) {
		perror("rdma_listen");
		goto out_destroy_listen_ep;
	}

	pthread_attr_init(&thread_attr);
	pthread_attr_setdetachstate(&thread_attr, PTHREAD_CREATE_DETACHED);

	while (1) {
		ret = rdma_get_request(listen_id, &id);
		if (ret) {
			perror("rdma_get_request");
			goto out_destroy_listen_ep;
		}
		printf("got connection request\n");
		if (pthread_create(&tid, &thread_attr, &agent_thread, id) != 0) {
			fprintf(stderr, "pthread create failed\n");
			rdma_destroy_id(id);
			goto out_destroy_listen_ep;
		}
	}

out_destroy_listen_ep:
	rdma_destroy_ep(listen_id);
out_free_addrinfo:
	rdma_freeaddrinfo(res);
	return ret;
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
			printf("\t[-p port_number]\n");
			exit(1);
		}
	}

	printf("rdma_server: start\n");
	ret = run();
	printf("rdma_server: end %d\n", ret);
	return ret;
}
