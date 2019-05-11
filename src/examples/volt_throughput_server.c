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
#include <semaphore.h>
#include "verbs.h"

#define CACHE_LINE_SIZE 64

static const char *server = "127.0.0.1";
static const char *port = "7471";

sem_t lock_sem;
static uint8_t lock_storage[8];

enum opcode {
	op_announce,
	op_lock_poll,
	op_lock_queue,
	op_unlock,
	op_lock_response,
};

struct lock_message {
	uint32_t opcode;
	uint32_t lock_rkey;
	uint64_t lock_addr;
};

struct context {
	struct lock_message *send_msg;
	struct ibv_mr *send_mr;
	struct lock_message *recv_msg;
	struct ibv_mr *recv_mr;
	struct ibv_mr *lock_mr;
	char peerhost[40];
	char peerport[6];
};

static void *agent_thread(void *arg)
{
	struct sockaddr *sa;
	struct context *ctx;
	struct rdma_cm_id *id = arg;
	struct ibv_qp_init_attr init_attr;
	struct ibv_qp_attr qp_attr;
	struct ibv_wc wc;
	int socklen, send_flags, ret, ret2;

	ctx = calloc(1, sizeof(*ctx));
	if (!ctx) {
		goto out_destroy_accept_ep;
	}

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
	getnameinfo(sa, socklen, ctx->peerhost, sizeof(ctx->peerhost),
			ctx->peerport, sizeof(ctx->peerport),
			NI_NUMERICHOST|NI_NUMERICSERV);
	printf("Got connect request from client: %s:%s\n", ctx->peerhost, ctx->peerport);

	memset(&qp_attr, 0, sizeof qp_attr);
	memset(&init_attr, 0, sizeof init_attr);
	ret = ibv_query_qp(id->qp, &qp_attr, IBV_QP_CAP,
			   &init_attr);
	if (ret) {
		perror("ibv_query_qp");
		goto out_free_ctx;
	}
	if (init_attr.cap.max_inline_data >= 16)
		send_flags = IBV_SEND_INLINE;
	else
		printf("rdma_server: device doesn't support IBV_SEND_INLINE, "
		       "using sge sends\n");

	ctx->send_msg = aligned_alloc(sizeof(*ctx->send_msg), CACHE_LINE_SIZE);
	if (!ctx->send_msg) {
		perror("aligned_alloc for send_msg");
		goto out_dereg_recv;
	}
	ctx->send_msg->lock_addr = (uintptr_t)&lock_storage;
	ctx->lock_mr = rdma_reg_write(id, &lock_storage, sizeof(lock_storage));
	if (!ctx->lock_mr) {
		perror("rdma_reg_write");
		goto out_destroy_accept_ep;
	}
	ctx->send_msg->lock_rkey = ctx->lock_mr->rkey;

	ctx->recv_msg = aligned_alloc(sizeof(*ctx->recv_msg), CACHE_LINE_SIZE);
	if (!ctx->recv_msg) {
		perror("aligned_alloc for recv_msg");
		goto out_dereg_recv;
	}
	ctx->recv_mr = rdma_reg_msgs(id, ctx->recv_msg, 16);
	if (!ctx->recv_mr) {
		ret = -1;
		perror("rdma_reg_msgs for recv_msg");
		goto out_dereg_lock;
	}
	if ((send_flags & IBV_SEND_INLINE) == 0) {
		ctx->send_mr = rdma_reg_msgs(id, ctx->send_msg, 16);
		if (!ctx->send_mr) {
			ret = -1;
			perror("rdma_reg_msgs for send_msg");
			goto out_dereg_recv;
		}
	}

	ret = rdma_post_recv(id, NULL, ctx->recv_msg, 16, ctx->recv_mr);
	if (ret) {
		perror("rdma_post_recv");
		goto out_dereg_send;
	}

	ret = rdma_accept(id, NULL);
	if (ret) {
		perror("rdma_accept");
		goto out_dereg_send;
	}

	ret = rdma_post_send(id, NULL, ctx->send_msg, sizeof(*ctx->send_msg),
			ctx->send_mr, send_flags);

	while ((ret = rdma_get_send_comp(id, &wc)) == 0);
	if (ret < 0) {
		perror("rdma_get_send_comp");
		goto out_disconnect;
	}

	do {
		while ((ret = rdma_get_recv_comp(id, &wc)) == 0);
		if (ret < 0) {
			perror("rdma_get_recv_comp");
			goto out_disconnect;
		}
		if (wc.status != IBV_WC_SUCCESS) {
			if (wc.status != IBV_WC_WR_FLUSH_ERR) {
				fprintf(stderr, "got unexpected WC result from client %s:%s: %s\n",
						ctx->peerhost, ctx->peerport,
						ibv_wc_status_str(wc.status));
			} else {
				fprintf(stderr, "flush error on client %s:%s\n",
						ctx->peerhost, ctx->peerport);
			}
			break;
		}
		if (wc.opcode == IBV_WC_RECV) {
			uint64_t *lock_ptr = (void *)&lock_storage;
			switch (ntohl(ctx->recv_msg->opcode)) {
			case op_lock_queue:
				ctx->send_msg->opcode = htonl(op_lock_response);
				ret = sem_wait(&lock_sem);
				if (ret < 0) {
					perror("sem_wait");
					goto out_disconnect;
				}
				*lock_ptr = 1;
				ctx->send_msg->lock_rkey = htonl(0);

				ret = rdma_post_recv(id, NULL, ctx->recv_msg, 16, ctx->recv_mr);
				if (ret) {
					perror("rdma_post_recv");
					goto out_disconnect;
				}

				ret = rdma_post_send(id, NULL, ctx->send_msg,
						sizeof(*ctx->send_msg), ctx->send_mr,
						send_flags);
				if (ret) {
					perror("rdma_post_send");
					goto out_disconnect;
				}
				ret = rdma_get_send_comp(id, &wc);
				if (ret < 0) {
					perror("rdma_get_send_comp");
					goto out_disconnect;
				}
				break;
			case op_lock_poll:
				ctx->send_msg->opcode = htonl(op_lock_response);
				ret = sem_trywait(&lock_sem);
				if (ret < 0 && errno == EINTR) {
					ctx->send_msg->lock_rkey = htonl(1);
				} else {
					ctx->send_msg->lock_rkey = htonl(0);
					*lock_ptr = 1;
				}

				ret = rdma_post_recv(id, NULL, ctx->recv_msg, 16, ctx->recv_mr);
				if (ret) {
					perror("rdma_post_recv");
					goto out_disconnect;
				}

				ret = rdma_post_send(id, NULL, ctx->send_msg,
						sizeof(*ctx->send_msg), ctx->send_mr,
						send_flags);
				if (ret) {
					perror("rdma_post_send");
					goto out_disconnect;
				}
				ret = rdma_get_send_comp(id, &wc);
				if (ret < 0) {
					perror("rdma_get_send_comp");
					goto out_disconnect;
				}
				break;
			case op_unlock:
				ctx->send_msg->opcode = htonl(op_lock_response);
				if (*lock_ptr == 1) {
					*lock_ptr = 0;
					ctx->send_msg->lock_rkey = htonl(0);
				} else {
					ctx->send_msg->lock_rkey = htonl(1);
				}
				sem_post(&lock_sem);

				ret = rdma_post_recv(id, NULL, ctx->recv_msg, 16, ctx->recv_mr);
				if (ret) {
					perror("rdma_post_recv");
					goto out_disconnect;
				}

				ret = rdma_post_send(id, NULL, ctx->send_msg,
						sizeof(*ctx->send_msg), ctx->send_mr,
						send_flags);
				if (ret) {
					perror("rdma_post_send");
					goto out_disconnect;
				}

				ret = rdma_get_send_comp(id, &wc);
				if (ret < 0) {
					perror("rdma_get_send_comp");
					goto out_disconnect;
				}
				break;
			}
		}
	} while (ret >= 0);

out_disconnect:
	rdma_disconnect(id);
	printf("Got disconnect from client: %s:%s\n", ctx->peerhost, ctx->peerport);
out_dereg_send:
	if ((send_flags & IBV_SEND_INLINE) == 0)
		rdma_dereg_mr(ctx->send_mr);
out_dereg_recv:
	rdma_dereg_mr(ctx->recv_mr);
out_dereg_lock:
	rdma_dereg_mr(ctx->lock_mr);
out_free_ctx:
	free(ctx);
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

	sem_init(&lock_sem, 0, 1);
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
