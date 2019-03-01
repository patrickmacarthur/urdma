/** \file server.cc
 * \author Patrick MacArthur <patrick@patrickmacarthur.net>
 */

#include <cstddef>
#include <cstdint>
#include <cstdlib>

#include <exception>
#include <iostream>
#include <thread>

#include <boost/endian/conversion.hpp>
#include <boost/format.hpp>

#include <arpa/inet.h>
#include <netdb.h>

#include <rdma/rdma_cma.h>
#include <rdma/rdma_verbs.h>

#include "ros.h"
#include "gai_category.h"

using boost::endian::big_to_native;
using boost::endian::native_to_big_inplace;
using boost::format;

static uint64_t cluster_id = 0x1122334455667788;
static unsigned long hostid = 0x12345678;

struct RDMALock {
	char lock[8];
};

struct RDMAObjectID {
	uint64_t nodeid;
	uint64_t uid;
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
	uint64_t rootnode_uid;
};
struct TreeRoot *root_obj;
struct ibv_mr *root_obj_mr;

static_assert(sizeof(struct MessageHeader) == 8, "incorrect size for MessageHeader");
static_assert(offsetof(struct AnnounceMessage, hdr.reserved2) == 2, "wrong offset for reserved2");
static_assert(offsetof(struct AnnounceMessage, reserved44) == 44, "wrong offset for reserved44");
static_assert(sizeof(struct AnnounceMessage) == 48, "incorrect size for LockAnnounceMessage");
static_assert(offsetof(struct GetHdrRequest, hdr.reserved2) == 2, "wrong offset for reserved2");
static_assert(sizeof(struct GetHdrRequest) == 16, "incorrect size for GetHdrRequest");
static_assert(offsetof(struct GetHdrResponse, hdr.reserved2) == 2, "wrong offset for reserved2");
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
	cs->send_buf->gethdrresp.hdr.version = 1;
	cs->send_buf->hdr.version = 0;
	cs->send_buf->hdr.opcode = OPCODE_GETHDR_RESP;
	native_to_big_inplace(cs->send_buf->gethdrresp.hdr.reserved2 = 0);
	native_to_big_inplace(cs->send_buf->gethdrresp.hdr.hostid = hostid);
	native_to_big_inplace(cs->send_buf->gethdrresp.replica_hostid1 = 0);
	native_to_big_inplace(cs->send_buf->gethdrresp.replica_hostid2 = 0);
	native_to_big_inplace(cs->send_buf->gethdrresp.addr = (uintptr_t)root_obj);
	native_to_big_inplace(cs->send_buf->gethdrresp.rkey = root_obj_mr->rkey);
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
	root_obj = reinterpret_cast<struct TreeRoot *>(
			aligned_alloc(CACHE_LINE_SIZE, sizeof(*root_obj)));
	if (!root_obj)
		throw std::system_error(errno, std::system_category());
	memset(root_obj, 0, sizeof(*root_obj));
	root_obj->objhdr.uid = 1;

	root_obj_mr = ibv_reg_mr(pd, root_obj, sizeof(*root_obj),
				 IBV_ACCESS_LOCAL_WRITE|IBV_ACCESS_REMOTE_READ
				 |IBV_ACCESS_REMOTE_WRITE);
	if (!root_obj_mr) {
		throw std::system_error(errno, std::system_category());
	}
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
	native_to_big_inplace(announcemsg->hdr.reserved2 = 0);
	native_to_big_inplace(announcemsg->hdr.hostid = hostid);
	announcemsg->rdma_ipv4_addr
		= (reinterpret_cast<struct sockaddr_in *>(sa)->sin_addr.s_addr);
	native_to_big_inplace(announcemsg->cluster_id = cluster_id);

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
	run(argv[1]);
	exit(EXIT_SUCCESS);
}
