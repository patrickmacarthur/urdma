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
#include <stdbool.h>
#include <byteswap.h>
#include <rdma/rdma_cma.h>
#include <rdma/rdma_verbs.h>
#include "verbs.h"

#define CACHE_LINE_SIZE 64

enum {
	MODE_POLL,
	MODE_QUEUE,
	MODE_ATOMIC,
	MODE_VOLT,
	MODE_COUNT
};

struct context;
struct mode {
	const char *name;
	bool (*lock)(struct context *, uint64_t, uint32_t);
	bool (*unlock)(struct context *, uint64_t, uint32_t);
};

#define DEFAULT_CYCLE_COUNT 100000
#define DEFAULT_THREAD_COUNT 1

struct context {
	struct rdma_addrinfo *addr_info;
	struct rdma_cm_id *id;
	const struct mode *mode;
	unsigned long cycle_count;
	struct lock_ops *ops;
	struct lock_message *send_msg;
	struct ibv_mr *send_mr;
	struct lock_message *recv_msg;
	struct ibv_mr *recv_mr;
	uint64_t lock_id;
	uint32_t lock_key;
	int send_flags;
};

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

static pthread_barrier_t start_barrier;

#if __BYTE_ORDER == __LITTLE_ENDIAN
static inline uint64_t htonll(uint64_t x) { return bswap_64(x); }
static inline uint64_t ntohll(uint64_t x) { return bswap_64(x); }
#else
static inline uint64_t htonll(uint64_t x) { return x; }
static inline uint64_t ntohll(uint64_t x) { return x; }
#endif

static double timespec_diff(struct timespec *start, struct timespec *end)
{
	static const unsigned long ns_per_s = 1000000000L;
	int carry = (end->tv_nsec < start->tv_nsec) ? 1 : 0;
	return (end->tv_sec - start->tv_sec - carry)
		+ (carry * ns_per_s + end->tv_nsec - start->tv_nsec)
		/ (double)ns_per_s;
}

static char *strerror_p(int errcode, char *buf, size_t bufsize) {
#if !defined(HAVE_STRERROR_R)
	static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
	char *err;
	pthread_mutex_lock(&mutex);
	err = strerror(errcode);
	strncpy(buf, err, bufsize);
	pthread_mutex_unlock(&mutex);
	return buf;
#elif defined(STRERROR_R_CHAR_P)
	int ret = strerror_r(ret, errbuf, sizeof(errbuf));
	if (ret)
		snprintf(buf, bufsize, "Unknown error %d", errcode);
	return buf;
#else
	int old_errno = errno;
	char *ret = strerror_r(ret, errbuf, sizeof(errbuf));
	errno = old_errno;
	return ret;
#endif
}

static bool do_lock_rpcpoll(struct context *ctx, uint64_t lock_id, uint32_t lock_key)
{
	struct ibv_wc wc;
	struct ibv_sge sge;
	bool result;
	int ret;

	do {
		ctx->send_msg->opcode = htonl(op_lock_poll);
		ctx->send_msg->lock_rkey = ctx->lock_key;
		ctx->send_msg->lock_addr = ctx->lock_id;

		ret = rdma_post_send(ctx->id, NULL, ctx->send_msg,
				sizeof(*ctx->send_msg), ctx->send_mr,
				ctx->send_flags);
		if (ret < 0) {
			fprintf(stderr, "urdma_remote_lock: %s\n", strerror(-ret));
			return false;
		}

		while ((ret = rdma_get_send_comp(ctx->id, &wc)) == 0);
		if (ret < 0) {
			perror("rdma_get_send_comp");
			return false;
		}
		if (wc.status != IBV_WC_SUCCESS) {
			fprintf(stderr, "Client 1 Got unexpected wc status %x not 0\n",
					wc.status);
			return false;
		}
		if (wc.opcode != IBV_WC_SEND) {
			fprintf(stderr, "Client 1 Got unexpected wc opcode %d not IBV_WC_SEND (%d)\n",
					wc.opcode, IBV_WC_SEND);
			return false;
		}
		while ((ret = rdma_get_recv_comp(ctx->id, &wc)) == 0);
		if (ret < 0) {
			perror("rdma_get_recv_comp");
			return false;
		}
		if (wc.status != IBV_WC_SUCCESS) {
			fprintf(stderr, "Client 1 Got unexpected wc status %x not 0\n",
					wc.status);
			return false;
		}
		if (wc.opcode != IBV_WC_RECV) {
			fprintf(stderr, "Client 1 Got unexpected wc opcode %d not IBV_WC_RECV (%d)\n",
					wc.opcode, IBV_WC_RECV);
			return false;
		}
		result = ctx->recv_msg->lock_rkey == 0;

		ret = rdma_post_recv(ctx->id, NULL, ctx->recv_msg, sizeof(*ctx->recv_msg), ctx->recv_mr);
		if (ret) {
			perror("rdma_post_recv");
			return false;
		}
	} while (!result);

	return true;
}

static bool do_unlock_rpcpoll(struct context *ctx, uint64_t lock_id, uint32_t lock_key)
{
	struct ibv_wc wc;
	struct ibv_sge sge;
	int ret;

	ctx->send_msg->opcode = htonl(op_unlock);
	ctx->send_msg->lock_rkey = ctx->lock_key;
	ctx->send_msg->lock_addr = ctx->lock_id;

	ret = rdma_post_send(ctx->id, NULL, ctx->send_msg,
			sizeof(*ctx->send_msg), ctx->send_mr,
			ctx->send_flags);
	if (ret < 0) {
		fprintf(stderr, "urdma_remote_lock: %s\n", strerror(-ret));
		return false;
	}

	while ((ret = rdma_get_send_comp(ctx->id, &wc)) == 0);
	if (ret < 0) {
		perror("rdma_get_send_comp");
		return false;
	}
	if (wc.status != IBV_WC_SUCCESS) {
		fprintf(stderr, "Client 1 Got unexpected wc status %x not 0\n",
				wc.status);
		return false;
	}
	if (wc.opcode != IBV_WC_SEND) {
		fprintf(stderr, "Client 1 Got unexpected wc opcode %d not IBV_WC_SEND (%d)\n",
				wc.opcode, IBV_WC_SEND);
		return false;
	}
	while ((ret = rdma_get_recv_comp(ctx->id, &wc)) == 0);
	if (ret < 0) {
		perror("rdma_get_recv_comp");
		return false;
	}
	if (wc.status != IBV_WC_SUCCESS) {
		fprintf(stderr, "Client 1 Got unexpected wc status %x not 0\n",
				wc.status);
		return false;
	}
	if (wc.opcode != IBV_WC_RECV) {
		fprintf(stderr, "Client 1 Got unexpected wc opcode %d not IBV_WC_RECV (%d)\n",
				wc.opcode, IBV_WC_RECV);
		return false;
	}

	ret = rdma_post_recv(ctx->id, NULL, ctx->recv_msg, sizeof(*ctx->recv_msg), ctx->recv_mr);
	if (ret) {
		perror("rdma_post_recv");
		return false;
	}

	return true;
}

static bool do_lock_rpcqueue(struct context *ctx, uint64_t lock_id, uint32_t lock_key)
{
	struct ibv_wc wc;
	struct ibv_sge sge;
	int ret;

	ctx->send_msg->opcode = htonl(op_lock_poll);
	ctx->send_msg->lock_rkey = ctx->lock_key;
	ctx->send_msg->lock_addr = ctx->lock_id;

	ret = rdma_post_send(ctx->id, NULL, ctx->send_msg,
			sizeof(*ctx->send_msg), ctx->send_mr,
			ctx->send_flags);
	if (ret < 0) {
		fprintf(stderr, "urdma_remote_lock: %s\n", strerror(-ret));
		return false;
	}

	while ((ret = rdma_get_send_comp(ctx->id, &wc)) == 0);
	if (ret < 0) {
		perror("rdma_get_send_comp");
		return false;
	}
	if (wc.status != IBV_WC_SUCCESS) {
		fprintf(stderr, "Client 1 Got unexpected wc status %x not 0\n",
				wc.status);
		return false;
	}
	if (wc.opcode != IBV_WC_SEND) {
		fprintf(stderr, "Client 1 Got unexpected wc opcode %d not IBV_WC_SEND (%d)\n",
				wc.opcode, IBV_WC_SEND);
		return false;
	}
	while ((ret = rdma_get_recv_comp(ctx->id, &wc)) == 0);
	if (ret < 0) {
		perror("rdma_get_recv_comp");
		return false;
	}
	if (wc.status != IBV_WC_SUCCESS) {
		fprintf(stderr, "Client 1 Got unexpected wc status %x not 0\n",
				wc.status);
		return false;
	}
	if (wc.opcode != IBV_WC_RECV) {
		fprintf(stderr, "Client 1 Got unexpected wc opcode %d not IBV_WC_RECV (%d)\n",
				wc.opcode, IBV_WC_RECV);
		return false;
	}
	if (ctx->recv_msg->lock_rkey != 0) {
		fprintf(stderr, "Client got failure for queue lock!!!!\n");
		return false;
	}

	ret = rdma_post_recv(ctx->id, NULL, ctx->recv_msg, sizeof(*ctx->recv_msg), ctx->recv_mr);
	if (ret) {
		perror("rdma_post_recv");
		return false;
	}

	return true;
}

static bool do_unlock_rpcqueue(struct context *ctx, uint64_t lock_id, uint32_t lock_key)
{
	return do_unlock_rpcpoll(ctx, lock_id, lock_key);
}

static bool do_lock_atomic(struct context *ctx, uint64_t lock_id, uint32_t lock_key)
{
	struct ibv_send_wr wr, *bad_wr;
	struct ibv_wc wc;
	struct ibv_sge sge;
	uint64_t target;
	int ret;

	do {
		wr.wr_id = (uintptr_t)ctx->id;
		wr.next = NULL;
		wr.sg_list = &sge;
		wr.sg_list[0].addr = (uintptr_t)(&target);
		wr.sg_list[0].length = sizeof(target);
		wr.sg_list[0].lkey = 0;
		wr.num_sge = 1;
		wr.opcode = IBV_WR_ATOMIC_CMP_AND_SWP;
		wr.send_flags = IBV_SEND_SIGNALED|IBV_SEND_INLINE;
		wr.wr.atomic.remote_addr = lock_id;
		wr.wr.atomic.rkey = lock_key;
		wr.wr.atomic.compare_add = 0;
		wr.wr.atomic.swap = htonll(1);

		ret = ibv_post_send(ctx->id->qp, &wr, &bad_wr);
		if (ret < 0) {
			fprintf(stderr, "urdma_remote_lock: %s\n", strerror(-ret));
			return false;
		}

		while ((ret = rdma_get_send_comp(ctx->id, &wc)) == 0);
		if (ret < 0) {
			perror("rdma_get_send_comp");
			return false;
		}
		if (wc.status != IBV_WC_SUCCESS) {
			fprintf(stderr, "Client 1 Got unexpected wc status %x not 0\n",
					wc.status);
			return false;
		}
		if (wc.opcode != IBV_WC_COMP_SWAP) {
			fprintf(stderr, "Client 1 Got unexpected wc opcode %d not IBV_WC_COMP_SWAP (%d)\n",
					wc.opcode, IBV_WC_COMP_SWAP);
			return false;
		}
	} while (target == 0);

	return true;
}

static bool do_unlock_atomic(struct context *ctx, uint64_t lock_id, uint32_t lock_key)
{
	struct ibv_send_wr wr, *bad_wr;
	struct ibv_sge sge;
	struct ibv_wc wc;
	uint64_t target;
	int ret;

	wr.wr_id = (uint64_t)ctx->id;
	wr.next = NULL;
	wr.sg_list = &sge;
	wr.sg_list[0].addr = (uint64_t)(&target);
	wr.sg_list[0].length = sizeof(target);
	wr.sg_list[0].lkey = 0;
	wr.num_sge = 1;
	wr.opcode = IBV_WR_ATOMIC_CMP_AND_SWP;
	wr.send_flags = IBV_SEND_SIGNALED|IBV_SEND_INLINE;
	wr.wr.atomic.remote_addr = lock_id;
	wr.wr.atomic.rkey = lock_key;
	wr.wr.atomic.compare_add = htonll(1);
	wr.wr.atomic.swap = htonll(0);

	ret = ibv_post_send(ctx->id->qp, &wr, &bad_wr);
	if (ret < 0) {
		fprintf(stderr, "urdma_remote_lock: %s\n", strerror(-ret));
		return false;
	}

	while ((ret = rdma_get_send_comp(ctx->id, &wc)) == 0);
	if (ret < 0) {
		perror("rdma_get_send_comp");
		return false;
	}
	if (wc.status != IBV_WC_SUCCESS) {
		fprintf(stderr, "Client 1 Got unexpected wc status %x not 0\n",
				wc.status);
		return false;
	}
	if (wc.opcode != IBV_WC_COMP_SWAP) {
		fprintf(stderr, "Client 1 Got unexpected wc opcode %d not IBV_WC_COMP_SWAP (%d)\n",
				wc.opcode, IBV_WC_COMP_SWAP);
		return false;
	}

	return true;
}

static bool do_lock_volt(struct context *ctx, uint64_t lock_id, uint32_t lock_key)
{
	struct ibv_wc wc;
	uint32_t lock_status;
	int ret;

	ret = urdma_remote_lock(ctx->id->qp, &lock_status, lock_id, lock_key, NULL);
	if (ret < 0) {
		fprintf(stderr, "urdma_remote_lock: %s\n", strerror(-ret));
		return false;
	}

	while ((ret = rdma_get_send_comp(ctx->id, &wc)) == 0);
	if (ret < 0) {
		perror("rdma_get_send_comp");
		return false;
	}
	if (wc.status != IBV_WC_SUCCESS) {
		fprintf(stderr, "Client 1 Got unexpected wc status %x not 0\n",
				wc.status);
		return false;
	}
	if (wc.opcode != 255) {
		fprintf(stderr, "Client 1 Got unexpected wc opcode %d not 255\n",
				wc.opcode);
		return false;
	}

	return true;
}

static bool do_unlock_volt(struct context *ctx, uint64_t lock_id, uint32_t lock_key)
{
	struct ibv_wc wc;
	uint32_t lock_status;
	int ret;

	ret = urdma_remote_unlock(ctx->id->qp, &lock_status, lock_id, lock_key,
				  NULL);
	if (ret < 0) {
		fprintf(stderr, "urdma_remote_unlock: %s\n", strerror(-ret));
		return false;
	}

	while ((ret = rdma_get_send_comp(ctx->id, &wc)) == 0);
	if (ret < 0) {
		perror("rdma_get_send_comp");
		return false;
	}
	if (wc.status != IBV_WC_SUCCESS) {
		fprintf(stderr, "Client 1 Got unexpected wc status %x not 0\n",
				wc.status);
		return false;
	}
	if (wc.opcode != 255) {
		fprintf(stderr, "Client 1 Got unexpected wc opcode %d not 255\n",
				wc.opcode);
		return false;
	}

	return true;
}

static void *client_thread(void *arg)
{
	struct context *ctx = arg;
	struct timespec start_time, end_time;
	struct ibv_qp_init_attr attr;
	struct rdma_cm_id *id;
	struct ibv_wc wc;
	unsigned long i;
	int ret;

	memcpy(&attr, &qp_init_attr_template, sizeof attr);
	attr.qp_context = ctx;
	ret = rdma_create_ep(&id, ctx->addr_info, NULL, &attr);
	ctx->id = id;
	// Check to see if we got inline data allowed or not
	if (attr.cap.max_inline_data >= 16)
		ctx->send_flags = IBV_SEND_INLINE;
	else
		printf("rdma_client: device doesn't support IBV_SEND_INLINE, "
		       "using sge sends\n");

	if (ret) {
		perror("rdma_create_ep");
		goto out;
	}

	ctx->recv_msg = aligned_alloc(sizeof(*ctx->recv_msg), CACHE_LINE_SIZE);
	if (!ctx->recv_msg) {
		perror("aligned_alloc for recv_msg");
		goto out_dereg_recv;
	}
	ctx->recv_mr = rdma_reg_msgs(id, ctx->recv_msg, sizeof(*ctx->recv_msg));
	if (!ctx->recv_mr) {
		perror("rdma_reg_msgs for ctx->recv_msg");
		ret = -1;
		goto out_destroy_ep;
	}
	ctx->send_msg = aligned_alloc(sizeof(*ctx->send_msg), CACHE_LINE_SIZE);
	if (!ctx->send_msg) {
		perror("aligned_alloc for send_msg");
		goto out_dereg_recv;
	}
	if ((ctx->send_flags & IBV_SEND_INLINE) == 0) {
		ctx->send_mr = rdma_reg_msgs(id, ctx->send_msg, 16);
		if (!ctx->send_mr) {
			ret = -1;
			perror("rdma_reg_msgs for ctx->recv_msg");
			goto out_free_send;
		}
	}

	ret = rdma_post_recv(id, NULL, ctx->recv_msg, sizeof(*ctx->recv_msg), ctx->recv_mr);
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
	ctx->lock_id = ctx->recv_msg->lock_addr;
	ctx->lock_key = ctx->recv_msg->lock_rkey;

	ret = rdma_post_recv(id, NULL, ctx->recv_msg, sizeof(*ctx->recv_msg), ctx->recv_mr);
	if (ret) {
		perror("rdma_post_recv");
		goto out_dereg_send;
	}

	pthread_barrier_wait(&start_barrier);
	clock_gettime(CLOCK_MONOTONIC, &start_time);
	for (i = 0; i < ctx->cycle_count; i++) {
		if (!ctx->mode->lock(ctx, ctx->recv_msg->lock_addr, ctx->recv_msg->lock_rkey)) {
			break;
		}

		if (!ctx->mode->unlock(ctx, ctx->recv_msg->lock_addr, ctx->recv_msg->lock_rkey)) {
			break;
		}
	}
	clock_gettime(CLOCK_MONOTONIC, &end_time);

	printf("client done; %lu iterations took %0.3lf seconds\n",
			i, timespec_diff(&start_time, &end_time));
	fflush(stdout);


out_disconnect:
	rdma_disconnect(id);
out_dereg_send:
	if ((ctx->send_flags & IBV_SEND_INLINE) == 0)
		rdma_dereg_mr(ctx->send_mr);
out_free_send:
	free(ctx->send_msg);
out_dereg_recv:
	rdma_dereg_mr(ctx->recv_mr);
out_destroy_ep:
	rdma_destroy_ep(id);
out:
	return (void *)(uintptr_t)ret;
}

static int run(const struct mode *mode, unsigned long cycle_count,
		unsigned int thread_count)
{
	struct rdma_addrinfo hints;
	struct context ctx, *perth_ctx;
	char errbuf[128];
	pthread_t threads[thread_count];
	unsigned int i;
	int ret;

	memset(&ctx, 0, sizeof(ctx));
	ctx.cycle_count = cycle_count;
	ctx.mode = mode;

	ret = pthread_barrier_init(&start_barrier, NULL, thread_count);
	if (ret) {
		fprintf(stderr, "pthread_barrier_init: %s\n",
				strerror_p(ret, errbuf, sizeof(errbuf)));
		goto out;
	}

	memset(&hints, 0, sizeof hints);
	hints.ai_port_space = RDMA_PS_TCP;
	ret = rdma_getaddrinfo(server, port, &hints, &ctx.addr_info);
	if (ret) {
		printf("rdma_getaddrinfo: %s\n", gai_strerror(ret));
		goto destroy_barrier;
	}

	for (i = 0; i < thread_count; ++i) {
		perth_ctx = malloc(sizeof(*perth_ctx));
		if (!perth_ctx) {
			perror("malloc perth_ctx");
			goto destroy_barrier;
		}
		memcpy(perth_ctx, &ctx, sizeof(*perth_ctx));
		ret = pthread_create(&threads[i], NULL, &client_thread, perth_ctx);
		if (ret) {
			fprintf(stderr, "error creating thread %i: %s\n", i,
					strerror_p(ret, errbuf, sizeof(errbuf)));
		}
	}

	for (i = 0; i < thread_count; ++i) {
		ret = pthread_join(threads[i], NULL);
		if (ret) {
			fprintf(stderr, "error joining thread %i: %s\n", i,
					strerror_p(ret, errbuf, sizeof(errbuf)));
		}
	}

destroy_barrier:
	pthread_barrier_destroy(&start_barrier);
out:
	return 0;
}

static const struct mode modes[] = {
	[MODE_POLL] = {
		.name = "poll",
		.lock = do_lock_rpcpoll,
		.unlock = do_unlock_rpcpoll,
	},
	[MODE_QUEUE] = {
		.name = "queue",
		.lock = do_lock_rpcqueue,
		.unlock = do_unlock_rpcqueue,
	},
	[MODE_ATOMIC] = {
		.name = "atomic",
		.lock = do_lock_atomic,
		.unlock = do_unlock_atomic,
	},
	[MODE_VOLT] = {
		.name = "volt",
		.lock = do_lock_volt,
		.unlock = do_unlock_volt,
	},
	{
		.name = NULL,
	}
};
#define DEFAULT_MODE (modes[MODE_VOLT].name)

int main(int argc, char **argv)
{
	unsigned int cycle_count = DEFAULT_CYCLE_COUNT;
	unsigned int thread_count = DEFAULT_THREAD_COUNT;
	const struct mode *mode;
	char *end;
	int i, op, ret;

	while ((op = getopt(argc, argv, "c:m:p:s:t:")) != -1) {
		switch (op) {
		case 's':
			server = optarg;
			break;
		case 'p':
			port = optarg;
			break;
		case 'c':
			errno = 0;
			cycle_count = strtoul(optarg, &end, 0);
			if (errno || !cycle_count || *end) {
				printf("invalid thread count \"%s\"\n", optarg);
				exit(1);
			}
			break;
		case 'm':
			for (mode = &modes[0]; mode != NULL; mode++) {
				if (strcmp(mode->name, optarg) == 0) {
					break;
				}
			}
			if (mode->name == NULL) {
				printf("invalid mode \"%s\"\n", optarg);
				exit(1);
			}
			break;
		case 't':
			errno = 0;
			thread_count = strtoul(optarg, &end, 0);
			if (errno || !thread_count || *end) {
				printf("invalid thread count \"%s\"\n", optarg);
				exit(1);
			}
			break;
		default:
			printf("usage: %s\n", argv[0]);
			printf("\t[-s server_address]\n");
			printf("\t[-p port_number]\n");
			printf("\t[-c cycle_count] (default %d)\n",
					DEFAULT_CYCLE_COUNT);
			printf("\t[-m mode] (default \"%s\")\n",
					DEFAULT_MODE);
			printf("\t[-t thread_count] (default %d)\n",
					DEFAULT_THREAD_COUNT);
			exit(1);
		}
	}

	printf("rdma_client: start\n");
	ret = run(mode, cycle_count, thread_count);
	printf("rdma_client: end %d\n", ret);
	return ret;
}
