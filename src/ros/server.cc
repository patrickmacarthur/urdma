/** \file server.cc
 * \author Patrick MacArthur <patrick@patrickmacarthur.net>
 */

#include <cstddef>
#include <cstdint>
#include <cstdlib>

#include <exception>
#include <iostream>
#include <thread>

#include <boost/dynamic_bitset.hpp>
#include <boost/endian/conversion.hpp>
#include <boost/format.hpp>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <sys/mman.h>

#include <rdma/rdma_cma.h>
#include <rdma/rdma_verbs.h>

#include "Tree.h"
#include "ros.h"
#include "gai_category.h"

using boost::dynamic_bitset;
using boost::endian::big_to_native;
using boost::endian::native_to_big_inplace;
using boost::format;

static uint64_t ros_magic = 0x2752005552005572;
static uint64_t cluster_id = 0x1122334455667788;
static unsigned long hostid = 0x12345678;

struct RDMALock {
	char lock[8];
};

struct RDMAObjectID {
	uint64_t nodeid;
	uint64_t uid;
};

struct ROSPoolHeader {
	uint64_t magic;
	uint64_t cluster_id;
	uint64_t host_id;
	uint64_t cur_obj_count;
	uint64_t max_obj_count;
};

struct ROSObjectHeader {
	struct RDMALock lock;
	uint64_t uid;
	uint32_t replica_hostid1;
	uint32_t replica_hostid2;
	uint32_t refcnt;
	uint32_t version;
};

struct TreeRoot {
	struct ROSObjectHeader objhdr;
};
void *pool_base;
const size_t pool_size = 1073741824ULL;
struct ROSPoolHeader *pool_header;
struct ROSObjectHeader *root_obj;
struct ibv_mr *pool_mr;
dynamic_bitset<> *store_bitset;

static_assert(sizeof(struct MessageHeader) == 8, "incorrect size for MessageHeader");
static_assert(offsetof(struct AnnounceMessage, reserved28) == 28, "wrong offset for reserved28");
static_assert(sizeof(struct AnnounceMessage) == 32, "incorrect size for LockAnnounceMessage");
static_assert(sizeof(struct GetHdrRequest) == 16, "incorrect size for GetHdrRequest");
static_assert(offsetof(struct GetHdrResponse, reserved36) == 36, "wrong offset for reserved36");
static_assert(sizeof(struct GetHdrResponse) == 40, "incorrect size for GetHdrResponse");

namespace {

struct ConnState {
	struct rdma_cm_id *id;
	struct AnnounceMessage *announce;
	struct ibv_mr *announce_mr;
	struct ibv_mr *recv_mr;
	union MessageBuf *send_buf;
	union MessageBuf recv_bufs[32];
};

struct AnnounceMessage *announcemsg;

uint64_t make_obj_id(size_t idx)
{
	return hostid << 32 + 1 << 16 + idx;
}

void process_announce(struct ConnState *cs, struct AnnounceMessage *msg)
{
	throw "not implemented";
}

void process_gethdrreq(struct ConnState *cs, struct GetHdrRequest *msg)
{
	std::cout << format("gethdr request for object %x\n")
			% big_to_native(msg->uid);
	cs->send_buf = reinterpret_cast<union MessageBuf *>(
			aligned_alloc(CACHE_LINE_SIZE, sizeof(*cs->send_buf)));
	cs->send_buf->hdr.version = 0;
	cs->send_buf->hdr.opcode = OPCODE_GETHDR_RESP;
	cs->send_buf->gethdrresp.hdr.req_id = msg->hdr.req_id;
	native_to_big_inplace(cs->send_buf->gethdrresp.hdr.hostid = hostid);
	native_to_big_inplace(cs->send_buf->gethdrresp.replica_hostid1 = 0);
	native_to_big_inplace(cs->send_buf->gethdrresp.replica_hostid2 = 0);
	native_to_big_inplace(cs->send_buf->gethdrresp.addr = (uintptr_t)root_obj);
	native_to_big_inplace(cs->send_buf->gethdrresp.rkey = pool_mr->rkey);
	native_to_big_inplace(cs->send_buf->gethdrresp.reserved36 = 0);

	int ret = rdma_post_send(cs->id, cs,
			     reinterpret_cast<void *>(cs->send_buf),
			     sizeof(*cs->send_buf), NULL,
			     IBV_SEND_SIGNALED|IBV_SEND_INLINE);
}

void process_gethdrresp(struct ConnState *cs, struct GetHdrResponse *msg)
{
	throw "not implemented";
}

void process_allocreq(struct ConnState *cs, struct AllocRequest *msg)
{
	std::cout << "alloc request from client\n";
	auto idx = store_bitset->find_first();
	if (idx == store_bitset->npos) {
		/* error case */
		return;
	}

	ROSObjectHeader *newobj = reinterpret_cast<struct ROSObjectHeader *>(
			reinterpret_cast<uint8_t *>(pool_base) + idx * page_size);
	newobj->uid = make_obj_id(idx);

	auto alloc_msg = reinterpret_cast<struct AllocResponse *>(
			aligned_alloc(CACHE_LINE_SIZE, sizeof(*cs->send_buf)));
	alloc_msg->hdr.version = 0;
	alloc_msg->hdr.opcode = OPCODE_ALLOC_RESP;
	cs->send_buf->gethdrresp.hdr.req_id = msg->hdr.req_id;
	native_to_big_inplace(alloc_msg->hdr.hostid = hostid);
	native_to_big_inplace(alloc_msg->uid = newobj->uid);
	native_to_big_inplace(alloc_msg->replica_hostid1 = 0);
	native_to_big_inplace(alloc_msg->replica_hostid2 = 0);
	native_to_big_inplace(alloc_msg->addr = (uintptr_t)root_obj);
}

void process_wc(struct ConnState *cs, struct ibv_wc *wc)
{
	auto mb = reinterpret_cast<union MessageBuf *>(wc->wr_id);
	switch (mb->hdr.opcode) {
	case OPCODE_ANNOUNCE:
		process_announce(cs, &mb->announce);
		break;
	case OPCODE_GETHDR_REQ:
		process_gethdrreq(cs, &mb->gethdrreq);
		break;
	case OPCODE_GETHDR_RESP:
		process_gethdrresp(cs, &mb->gethdrresp);
		break;
	case OPCODE_ALLOC_REQ:
		process_allocreq(cs, &mb->allocreq);
		break;
	default:
		return;
	}
}

void handle_connection(struct ConnState *cs)
{
	struct ibv_qp *qp = cs->id->qp;
	int ret;

	cs->recv_mr = ibv_reg_mr(cs->id->pd, cs->recv_bufs,
				 sizeof(cs->recv_bufs), 0);
	if (!cs->recv_mr) {
		rdma_reject(cs->id, NULL, 0);
		return;
	}

	for (int i = 0; i < 32; i++) {
		ret = rdma_post_recv(cs->id, cs->recv_bufs[i].buf,
				     cs->recv_bufs[i].buf,
				     sizeof(cs->recv_bufs[i]), cs->recv_mr);
		if (ret) {
			rdma_reject(cs->id, NULL, 0);
			return;
		}
	}

	ret = rdma_accept(cs->id, NULL);
	if (ret)
		return;

	cs->announce_mr = ibv_reg_mr(cs->id->pd, announcemsg,
				     sizeof(*announcemsg), 0);
	CHECK_PTR_ERRNO(cs->announce_mr);

	ret = rdma_post_send(cs->id, cs,
			     reinterpret_cast<void *>(announcemsg),
			     sizeof(*cs->announce), cs->announce_mr,
			     IBV_SEND_SIGNALED);
	if (ret)
		return;

	struct ibv_wc wc[32];
	int count;
	while ((count = ibv_poll_cq(cs->id->recv_cq, 32, wc)) >= 0) {
		for (int i = 0; i < count; i++) {
			if (wc[i].status == IBV_WC_SUCCESS) {
				process_wc(cs, &wc[i]);
			} else {
				std::cerr << format("completion failed: %s\n")
					% ibv_wc_status_str(wc[i].status);
				return;
			}
		}
	}
}

void init_tree_root(struct ibv_pd *pd)
{
	int fd = open("/opt/local-scratch/pagemap.dat", O_RDWR|O_CREAT, 0644);
	if (fd < 0)
		throw std::system_error(errno, std::system_category());

	int ret = ftruncate(fd, pool_size);
	if (ret < 0)
		throw std::system_error(errno, std::system_category());

	pool_base = mmap(NULL, pool_size, PROT_READ|PROT_WRITE,
			MAP_SHARED, fd, 0);
	if (!pool_base)
		throw std::system_error(errno, std::system_category());
	pool_header = reinterpret_cast<struct ROSPoolHeader *>(pool_base);
	root_obj = reinterpret_cast<struct ROSObjectHeader *>(
			reinterpret_cast<uint8_t *>(pool_base) + page_size);

	if (pool_header->magic != ros_magic) {
		pool_header->magic = ros_magic;
		pool_header->cluster_id = cluster_id;
		pool_header->host_id = hostid;
		pool_header->cur_obj_count = 1;
		pool_header->max_obj_count = pool_size / page_size - 1;
		root_obj->uid = 1;
	}

	store_bitset = new dynamic_bitset<>(pool_header->max_obj_count, 0);
	for (dynamic_bitset<>::size_type i = 0; i < store_bitset->size(); ++i) {
		(*store_bitset)[i] = reinterpret_cast<struct ROSObjectHeader *>(
			reinterpret_cast<uint8_t *>(pool_base) + i * page_size)->uid != 0;
	}

	pool_mr = ibv_reg_mr(pd, pool_base, pool_size,
				 IBV_ACCESS_LOCAL_WRITE|IBV_ACCESS_REMOTE_READ
				 |IBV_ACCESS_REMOTE_WRITE);
	if (!pool_mr) {
		throw std::system_error(errno, std::system_category());
	}
	std::cerr << format("rkey is %x\n") % pool_mr->rkey;
}

struct ibv_pd *get_pd()
{
	struct ibv_context **dev = rdma_get_devices(NULL);
	struct ibv_pd *pd = ibv_alloc_pd(*dev);
	rdma_free_devices(dev);
	return pd;
}

void mcast_responder(const char *userhost)
{
	struct addrinfo hints, *ai;
	ssize_t ret;

	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_DGRAM;
	hints.ai_flags = AI_PASSIVE;
	ret = getaddrinfo(userhost, NULL, &hints, &ai);
	if (ret) {
		throw std::system_error(ret, gai_category());
	}

	int mcfd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
	CHECK_ERRNO(mcfd);
	struct sockaddr_in any;
	any.sin_family = AF_INET;
	any.sin_addr = in_addr{INADDR_ANY};
	native_to_big_inplace(any.sin_port = ros_mcast_port);
	CHECK_ERRNO(bind(mcfd, reinterpret_cast<struct sockaddr *>(&any), sizeof(any)));
	int val = 1;
	CHECK_ERRNO(setsockopt(mcfd, IPPROTO_IP, IP_MULTICAST_ALL,
				&val, sizeof(val)));
	struct ip_mreqn mreq;
	CHECK_ERRNO(inet_pton(AF_INET, ros_mcast_addr, &mreq.imr_multiaddr));
	mreq.imr_address = ((struct sockaddr_in *)ai->ai_addr)->sin_addr;
	mreq.imr_ifindex = 0;
	CHECK_ERRNO(setsockopt(mcfd, IPPROTO_IP, IP_ADD_MEMBERSHIP,
				&mreq, sizeof(mreq)));

	while (1) {
		union MessageBuf recvbuf;
		struct sockaddr_storage sastore;
		auto sa = reinterpret_cast<struct sockaddr *>(&sastore);
		socklen_t salen = sizeof(sa);

		ret = recvfrom(mcfd, &recvbuf, sizeof(recvbuf), 0,
			       sa, &salen);
		CHECK_ERRNO(ret);

		if (recvbuf.hdr.version != 0
				|| recvbuf.hdr.opcode != OPCODE_QUERY_SERVERS
				|| big_to_native(recvbuf.qsmsg.cluster_id) != cluster_id) {
			/* Ignore announce messages from other servers or
			 * otherwise invalid messages */
			continue;
		}

		struct sockaddr_in multiout;
		multiout.sin_family = AF_INET;
		multiout.sin_addr = mreq.imr_multiaddr;
		native_to_big_inplace(multiout.sin_port = ros_mcast_port);
		ret = sendto(mcfd, announcemsg, sizeof(*announcemsg), 0,
			     (struct sockaddr *)&multiout, sizeof(multiout));
		CHECK_ERRNO(ret);
	}
}

void run(char *host)
{
	struct rdma_addrinfo hints, *rai;
	struct rdma_cm_id *listen_id, *id;
	struct ConnState *cs;
	std::vector<std::thread> client_threads;
	int ret;

	memset(&hints, 0, sizeof(hints));
	hints.ai_flags = RAI_PASSIVE;
	hints.ai_port_space = RDMA_PS_TCP;

	ret = rdma_getaddrinfo(host, "9001", &hints, &rai);
	if (ret)
		exit(EXIT_FAILURE);

	struct ibv_qp_init_attr attr;
	memset(&attr, 0, sizeof(attr));
	attr.qp_type = IBV_QPT_RC;
	attr.cap.max_send_wr = 64;
	attr.cap.max_recv_wr = 64;
	ret = rdma_create_ep(&listen_id, rai, get_pd(), &attr);
	if (ret)
		exit(EXIT_FAILURE);

	init_tree_root(listen_id->pd);

	ret = rdma_listen(listen_id, 0);
	if (ret)
		exit(EXIT_FAILURE);

	struct sockaddr *sa = rdma_get_local_addr(listen_id);
	assert(sa != NULL);
	int salen;
	switch (sa->sa_family) {
	case AF_INET:
		salen = sizeof(struct sockaddr_in);
		break;
	case AF_INET6:
		salen = sizeof(struct sockaddr_in6);
		break;
	default:
		throw format("unexpected sa_family %d") % sa->sa_family;
	}
	char userhost[256];
	char userport[10];
	ret = getnameinfo(sa, sizeof(struct sockaddr_in6), userhost, 256, userport, 10,
			NI_NUMERICSERV);
	if (ret) {
		throw std::system_error(ret, gai_category());
	}
	std::cerr << "Listening on " << userhost << ":" << userport << "\n";
	std::cerr << format("cluster id is %x\n") % cluster_id;

	announcemsg = reinterpret_cast<struct AnnounceMessage *>(
			aligned_alloc(CACHE_LINE_SIZE, sizeof(*announcemsg)));
	if (!announcemsg)
		return;
	announcemsg->hdr.version = 0;
	announcemsg->hdr.opcode = OPCODE_ANNOUNCE;
	native_to_big_inplace(announcemsg->hdr.req_id = 0);
	native_to_big_inplace(announcemsg->hdr.hostid = hostid);
	announcemsg->rdma_ipv4_addr
		= (reinterpret_cast<struct sockaddr_in *>(sa)->sin_addr.s_addr);
	native_to_big_inplace(announcemsg->cluster_id = cluster_id);
	native_to_big_inplace(announcemsg->pool_rkey = pool_mr->rkey);

	std::thread mcast_thread{mcast_responder, userhost};

	while (1) {
		ret = rdma_get_request(listen_id, &id);
		if (ret)
			exit(EXIT_FAILURE);

		std::cerr << "Got connection!\n";

		cs = new ConnState;
		cs->id = id;
		std::thread t{handle_connection, cs};
		client_threads.push_back(std::move(t));
	}
	for (auto &t: client_threads) {
		t.join();
	}
}

}

int main(int argc, char *argv[])
{
	char buf[1024];
	auto *t = new(buf) Tree<int, 10>();
	run(argv[1]);
	exit(EXIT_SUCCESS);
}
