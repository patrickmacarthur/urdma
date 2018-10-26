/* driver.c */

/*
 * Userspace Software iWARP library for DPDK
 *
 * Authors: Patrick MacArthur <patrick@patrickmacarthur.net>
 *
 * Copyright (c) 2016, IBM Corporation
 * Copyright (c) 2016-2018, University of New Hampshire
 *
 * This software is available to you under a choice of one of two
 * licenses.  You may choose to be licensed under the terms of the GNU
 * General Public License (GPL) Version 2, available from the file
 * COPYING in the main directory of this source tree, or the
 * BSD license below:
 *
 *   Redistribution and use in source and binary forms, with or
 *   without modification, are permitted provided that the following
 *   conditions are met:
 *
 *   - Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *
 *   - Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *
 *   - Neither the name of IBM nor the names of its contributors may be
 *     used to endorse or promote products derived from this software without
 *     specific prior written permission.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <assert.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/prctl.h>
#include <sys/un.h>
#include <unistd.h>

#include <ccan/list/list.h>

#include <rte_config.h>
#include <rte_ethdev.h>
#include <rte_errno.h>
#include <rte_ip.h>
#include <rte_jhash.h>
#include <rte_malloc.h>
#include <rte_ring.h>

#include <spdk/env.h>

#include <netlink/addr.h>
#include <netlink/cache.h>
#include <netlink/errno.h>
#include <netlink/socket.h>
#include <netlink/utils.h>
#include <netlink/route/addr.h>
#include <netlink/route/link.h>

#include "config_file.h"
#include "interface.h"
#include "urdma_kabi.h"
#include "util.h"

static struct usiw_driver *driver;

int
driver_add_context(struct usiw_context *ctx)
{
	int i, ret;
	ret = rte_ring_enqueue(driver->new_ctxs, ctx->h);
	for (i = 0; ret == -ENOBUFS && i < 1000; ++i) {
		ret = rte_ring_enqueue(driver->new_ctxs, ctx->h);
	}
	return ret;
} /* driver_add_context */

void
start_progress_thread(void)
{
	sem_post(&driver->go);
} /* start_progress_thread */


static int
setup_nl_sock(void)
{
	int rv;

	driver->sock = nl_socket_alloc();
	if (!driver->sock)
		return -1;
	if ((rv = nl_connect(driver->sock, NETLINK_ROUTE)))
		goto free_socket;

	rv = rtnl_link_alloc_cache(driver->sock,
			AF_UNSPEC, &driver->link_cache);
	if (rv)
		goto free_socket;
	rv = rtnl_addr_alloc_cache(driver->sock,
			&driver->addr_cache);
	if (rv)
		goto free_link_cache;

	return 0;

free_link_cache:
	nl_cache_free(driver->link_cache);
free_socket:
	nl_socket_free(driver->sock);

	return -1;
} /* setup_netlink */


static int
get_ipv4addr(int portid, uint32_t *result)
{
	char kni_name[RTE_KNI_NAMESIZE];
	struct rtnl_link *link;
	struct nl_cache *subset;
	struct rtnl_addr *hints, *addr;
	struct nl_addr *local;
	int ifindex;
	int rv = -1;

	if (!driver->sock) {
		if (setup_nl_sock()) {
			return -1;
		}
	}

	snprintf(kni_name, RTE_KNI_NAMESIZE, "kni%u", portid);
	link = rtnl_link_get_by_name(driver->link_cache, kni_name);
	if (!link) {
		return -1;
	}
	ifindex = rtnl_link_get_ifindex(link);
	rtnl_link_put(link);

	/* Create an address object with only the ifindex defined.  We can then
	 * use this partial object as a filter to only select address entries
	 * with this interface index, which effectively gets us every IP address
	 * assigned to this interface. */
	hints = rtnl_addr_alloc();
	if (!hints) {
		fprintf(stderr, "Could not allocate network address hints\n");
		return -1;
	}
	rtnl_addr_set_ifindex(hints, ifindex);
	rtnl_addr_set_family(hints, AF_INET);
	subset = nl_cache_subset(driver->addr_cache, (struct nl_object *)hints);
	rtnl_addr_put(hints);
	addr = (struct rtnl_addr *)nl_cache_get_first(subset);
	if (!addr) {
		goto free_hints;
	}

	local = rtnl_addr_get_local(addr);
	memcpy(result, nl_addr_get_binary_addr(local), sizeof(*result));
	rv = 0;

free_hints:
	nl_cache_put(subset);

	return rv;
} /* get_ipaddr */


static struct ibv_device *
usiw_driver_init(int portid)
{
	static const uint32_t tx_checksum_offloads
		= DEV_TX_OFFLOAD_UDP_CKSUM|DEV_TX_OFFLOAD_IPV4_CKSUM;

	struct usiw_device *dev;
	struct rte_eth_dev_info info;
	char name[RTE_MEMPOOL_NAMESIZE];

	dev = calloc(1, sizeof(*dev));
	if (!dev) {
		errno = ENOMEM;
		return NULL;
	}

	dev->portid = portid;
	rte_eth_macaddr_get(dev->portid, &dev->ether_addr);
	if (get_ipv4addr(dev->portid, &dev->ipv4_addr)) {
		free(dev);
		errno = ENOENT;
		return NULL;
	}
	rte_eth_dev_info_get(dev->portid, &info);

	if ((info.tx_offload_capa & tx_checksum_offloads)
						== tx_checksum_offloads) {
		dev->flags |= port_checksum_offload;
	}
	if (rte_eth_dev_filter_supported(dev->portid,
						RTE_ETH_FILTER_FDIR) == 0) {
		dev->flags |= port_fdir;
	}

	snprintf(name, RTE_MEMPOOL_NAMESIZE, "port_%u_rx_mempool", portid);
	dev->rx_mempool = rte_mempool_lookup(name);
	if (!dev->rx_mempool) {
		free(dev);
		errno = ENOENT;
		return NULL;
	}

	snprintf(name, RTE_MEMPOOL_NAMESIZE, "port_%u_tx_mempool", portid);
	dev->tx_ddp_mempool = dev->tx_hdr_mempool = rte_mempool_lookup(name);
	if (!dev->tx_ddp_mempool) {
		free(dev);
		errno = ENOENT;
		return NULL;
	}

	dev->urdmad_fd = driver->urdmad_fd;
	dev->max_qp = driver->max_qp[dev->portid];
	dev->driver = driver;

	return &dev->vdev.device;
} /* usiw_driver_init */


static int
open_socket(int family, int socktype, int proto)
{
	int ret, fd;

#if HAVE_DECL_SOCK_CLOEXEC
	/* Atomically set the FD_CLOEXEC flag when creating the socket */
	fd = socket(family, socktype | SOCK_CLOEXEC, proto);
	if (fd >= 0 || (errno != EINVAL && errno != EPROTOTYPE))
		return fd;
#endif

	/* The system doesn't support SOCK_CLOEXEC; set the flag using
	 * fcntl() and live with the small window for a race with fork+exec
	 * from another thread */
	fd = socket(family, socktype, proto);
	if (fd < 0)
		return fd;

	ret = fcntl(fd, F_GETFD);
	if (ret < 0)
		goto close_fd;
	ret = fcntl(fd, F_SETFD, ret | FD_CLOEXEC);
	if (ret < 0)
		goto close_fd;
	return fd;

close_fd:
	close(fd);
	return ret;
} /* open_socket */


static int
setup_socket(const char *sock_name)
{
	struct sockaddr_un addr;
	int fd, ret;

	if (strlen(sock_name) >= sizeof(addr.sun_path) - 1) {
		fprintf(stderr, "Invalid socket path %s: too long\n",
				sock_name);
		errno = EINVAL;
		return -1;
	}

	fd = open_socket(AF_UNIX, SOCK_SEQPACKET, 0);
	if (fd < 0) {
		fprintf(stderr, "Could not create socket: %s\n",
				strerror(errno));
		return fd;
	}

	addr.sun_family = AF_UNIX;
	strncpy(addr.sun_path, sock_name, sizeof(addr.sun_path));
	ret = connect(fd, (struct sockaddr *)&addr, sizeof(addr));
	if (ret < 0) {
		if (getenv("IBV_SHOW_WARNINGS")) {
			fprintf(stderr, "Could not connect to %s: %s\n",
					addr.sun_path, strerror(errno));
		}
		goto err;
	}

	return fd;

err:
	close(fd);
	return -1;
} /* setup_socket */


/** Parses the config file and fills in sock_name with the name of the socket to
 * use. */
static bool
do_config(char **sock_name)
{
	struct usiw_config config;
	bool result = false;
	int ret;

	ret = urdma__config_file_open(&config);
	if (ret < 0) {
		fprintf(stderr, "Could not read config file: %s\n",
				strerror(errno));
		goto out;
	}

	*sock_name = urdma__config_file_get_sock_name(&config);
	if (!*sock_name) {
		fprintf(stderr, "Could not parse socket name from config file: %s\n",
				strerror(errno));
		goto close_config;
	}
	result = true;
	goto close_config;

free_sock_name:
	free(*sock_name);
close_config:
	urdma__config_file_close(&config);
out:
	return result;
} /* do_config */


static int
do_hello(void)
{
	struct urdmad_sock_hello_req req;
	struct urdmad_sock_hello_resp *resp;
	struct pollfd poll_list;
	uint32_t lid;
	int i;
	ssize_t ret;
	int resp_size;

	memset(&req, 0, sizeof(req));
	req.hdr.opcode = rte_cpu_to_be_32(urdma_sock_hello_req);
	req.proto_version = URDMA_SOCK_PROTO_VERSION;
	req.req_lcore_count = rte_cpu_to_be_16(1);
	ret = send(driver->urdmad_fd, &req, sizeof(req), 0);
	if (ret != sizeof(req)) {
		return -1;
	}
	poll_list.fd = driver->urdmad_fd;
	poll_list.events = POLLIN;
	poll_list.revents = 0;
	ret = poll(&poll_list, 1, -1);
	if (ret < 0) {
		return -1;
	}
	ret = ioctl(driver->urdmad_fd, FIONREAD, &resp_size);
	if (ret < 0 || resp_size < sizeof(*resp)) {
		return -1;
	}
	resp = alloca(resp_size);
	ret = recv(driver->urdmad_fd, resp, resp_size, 0);
	if (ret != resp_size) {
		return -1;
	}

	if (resp->proto_version != URDMA_SOCK_PROTO_VERSION)
		return -1;
	for (i = 0; i < RTE_DIM(resp->lcore_mask); i++) {
		driver->lcore_mask[i] = rte_be_to_cpu_32(resp->lcore_mask[i]);
	}
	driver->shm_id = rte_be_to_cpu_16(resp->shm_id);
	driver->device_count = rte_be_to_cpu_16(resp->device_count);
	driver->rdma_atomic_mutex = (void *)(uintptr_t)rte_be_to_cpu_64(
				resp->rdma_atomic_mutex_addr);
	driver->max_qp = malloc(driver->device_count * sizeof(*driver->max_qp));
	if (!driver->max_qp) {
		return -1;
	}
	for (i = 0; i < driver->device_count; i++) {
		driver->max_qp[i] = rte_be_to_cpu_16(resp->max_qp[i]);
	}

	return 0;
} /* do_hello */


/** Formats the coremask as a hexadecimal string.  Array size is the number of
 * uint32_t elements in coremask. */
static char *
format_coremask(uint32_t *coremask, size_t array_size)
{
	static const size_t width = 2 * sizeof(*coremask);
	char *p, *result;
	int i;

	/* "0xabcdabcdabcdabcd" */
	p = result = malloc(width * array_size + 3);
	if (!result) {
		return NULL;
	}
	*(p++) = '0';
	*(p++) = 'x';
	for (i = array_size - 1; i >= 0; i--) {
		snprintf(p, width + 1, "%0*" PRIx32, (int)width, coremask[i]);
		p += width;
	}
	*p = '\0';

	return result;
} /* format_coremask */


/** Initialize the DPDK in a separate thread; this way we do not affect the
 * affinity of the user thread which first calls ibv_get_device_list, whether
 * directly or indirectly. */
static void *
our_eal_master_thread(void *sem)
{
	struct spdk_env_opts opts;
	char *sock_name;
	int eal_argc, ret;

	spdk_env_opts_init(&opts);

	driver = calloc(1, sizeof(*driver) + rte_ring_get_memsize(
							NEW_CTX_MAX + 1));
	if (!driver)
		goto err;
	list_head_init(&driver->ctxs);

	do_config(&sock_name);
	driver->urdmad_fd = setup_socket(sock_name);
	if (driver->urdmad_fd < 0)
		goto err;
	free(sock_name);
	if (do_hello() < 0) {
		fprintf(stderr, "Could not setup socket: %s\n",
				strerror(errno));
		goto close_fd;
	}
	/* Send log messages to stderr instead of syslog */
	opts.core_mask = format_coremask(driver->lcore_mask,
					 RTE_DIM(driver->lcore_mask));
	opts.shm_id = driver->shm_id;

	rte_openlog_stream(stderr);
	spdk_env_init(&opts);

	driver->new_ctxs = (struct rte_ring *)(driver + 1);
	ret = rte_ring_init(driver->new_ctxs, "new_ctx_ring", NEW_CTX_MAX + 1,
			    RING_F_SC_DEQ);
	if (ret < 0) {
		RTE_LOG(ERR, USER1, "cannot allocate new context ring: %s\n",
				rte_strerror(ret));
		goto close_fd;
	}

	/* Here we create a semaphore "go" which is used to start the progress
	 * thread once a uverbs context is established, and then post on our
	 * initialization semaphore to let the "parent" thread know that we have
	 * completed initialization. */
	if (sem_init(&driver->go, 0, 0))
		goto free_ring;
	ret = sem_post(sem);
	if (ret) {
		goto destroy_sem;
	}
	kni_loop(driver);

	return NULL;

destroy_sem:
	sem_destroy(&driver->go);
free_ring:
	rte_ring_free(driver->new_ctxs);
close_fd:
	close(driver->urdmad_fd);
	free(driver->max_qp);
err:
	free(driver);
	driver = NULL;
	ret = sem_post(sem);
	return NULL;
} /* our_eal_master_thread */


static void
do_init_driver(void)
{
	pthread_t thread;
	sem_t sem;
	int ret;

	if (sem_init(&sem, 0, 0)) {
		if (getenv("IBV_SHOW_WARNINGS")) {
			fprintf(stderr, "Could not initialize semaphore: %s\n",
					strerror(errno));
		}
		return;
	}

	ret = pthread_create(&thread, NULL, &our_eal_master_thread, &sem);
	if (ret) {
		if (getenv("IBV_SHOW_WARNINGS")) {
			fprintf(stderr,
				"Could not create urdma progress thread: %s\n",
				strerror(ret));
		}
		return;
	}

	do {
		ret = sem_wait(&sem);
	} while (ret < 0 && errno == EINTR);
	if (ret < 0) {
		if (getenv("IBV_SHOW_WARNINGS")) {
			fprintf(stderr,
				"Error waiting on initialization semaphore: %s\n",
				strerror(errno));
		}
		return;
	}
	sem_destroy(&sem);
}


static struct verbs_device *
urdma_device_alloc(struct verbs_sysfs_dev *sysfs_dev)
{
	static pthread_once_t driver_init_once = PTHREAD_ONCE_INIT;
	struct ibv_device *ibdev;
	char value[16];
	int portid;

	if (ibv_read_sysfs_file(sysfs_dev->sysfs_path, "ibdev",
				value, sizeof value) < 0)
		return NULL;

	if (sscanf(value, URDMA_DEV_PREFIX "%d", &portid) < 1)
		return NULL;

	pthread_once(&driver_init_once, &do_init_driver);
	if (!driver) {
		/* driver initialization failed */
		return NULL;
	}

	ibdev = usiw_driver_init(portid);
	return ibdev ? verbs_get_device(ibdev) : NULL;
} /* usiw_verbs_driver_init */

static void urdma_device_uninit(struct verbs_device *verbs_device)
{
	struct usiw_device *dev = container_of(verbs_device,
		struct usiw_device, vdev);
	free(dev);
}


static const struct verbs_match_ent hca_table[] = {
	/* FIXME: urdma needs a more reliable way to detect the urdma device */
	VERBS_NAME_MATCH("urdma", NULL),
};


struct verbs_device_ops urdma_device_ops = {
	.name = "urdma",
	.match_min_abi_version = URDMA_ABI_VERSION_MIN,
	.match_max_abi_version = URDMA_ABI_VERSION_MAX,
	.match_table = hca_table,
	.alloc_device = urdma_device_alloc,
	.uninit_device = urdma_device_uninit,
	.alloc_context = urdma_alloc_context,
	.free_context = urdma_free_context,
};
PROVIDER_DRIVER(urdma_device_ops);
