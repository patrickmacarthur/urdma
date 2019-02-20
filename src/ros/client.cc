/** \file server.cc
 * \author Patrick MacArthur <patrick@patrickmacarthur.net>
 */

#include <cstddef>
#include <cstdint>
#include <cstdlib>

#include <exception>
#include <iomanip>
#include <iostream>

#include <boost/endian/conversion.hpp>
#include <boost/format.hpp>

#include <rdma/rdma_cma.h>
#include <rdma/rdma_verbs.h>

#include "ros.h"

using boost::endian::big_to_native;
using boost::endian::native_to_big_inplace;
using boost::format;

static const int CACHE_LINE_SIZE = 64;

struct ROSRemoteObject {
	uint64_t uid;
	uint64_t remote_addr;
	uint32_t rkey;
};

struct ConnState {
	struct rdma_cm_id *id;
	struct ibv_mr *send_mr;
	struct ibv_mr *recv_mr;
	unsigned long server_hostid;
	struct ROSRemoteObject root;
	union MessageBuf *nextsend;
	union MessageBuf recv_bufs[32];
};

void process_announce(struct ConnState *cs, struct AnnounceMessage *msg)
{
	cs->server_hostid = big_to_native(msg->hdr.hostid);
	cs->root.uid = big_to_native(msg->root_uid);
	cs->root.remote_addr = big_to_native(msg->root_addr);
	cs->root.rkey = big_to_native(msg->root_rkey);
	std::cout << format("announce from hostid %x\n") % cs->server_hostid;
}

void process_gethdrreq(struct ConnState *cs, struct GetHdrRequest *msg)
{
	std::terminate();
}

void process_gethdrresp(struct ConnState *cs, struct GetHdrResponse *msg)
{
	std::cout << format("gethdr response for object %x remote addr %x rkey %x\n")
			% big_to_native(msg->uid)
			% big_to_native(msg->addr) % big_to_native(msg->rkey);
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

void run(char *host)
{
	struct ConnState *cs;
	struct rdma_addrinfo hints, *rai;
	struct ibv_qp_init_attr attr;
	struct rdma_conn_param cparam;
	int ret;

	cs = new ConnState;

	memset(&hints, 0, sizeof(hints));
	hints.ai_flags = 0;
	hints.ai_port_space = RDMA_PS_TCP;

	ret = rdma_getaddrinfo(host, "9001", &hints, &rai);
	if (ret)
		std::terminate();

	memset(&attr, 0, sizeof(attr));
	attr.qp_type = IBV_QPT_RC;
	attr.cap.max_send_wr = 64;
	attr.cap.max_recv_wr = 64;
	ret = rdma_create_ep(&cs->id, rai, NULL, &attr);
	if (ret)
		std::terminate();

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

	memset(&cparam, 0, sizeof(cparam));
	cparam.initiator_depth = 1;
	cparam.responder_resources = 1;
	ret = rdma_connect(cs->id, &cparam);
	if (ret) {
		std::cerr << "rdma_connect returned " << ret << " errno " << errno << "\n";
		std::terminate();
	}

	cs->nextsend = reinterpret_cast<union MessageBuf *>(
			aligned_alloc(CACHE_LINE_SIZE, sizeof(*cs->nextsend)));
	if (!cs->nextsend)
		std::terminate();
	cs->nextsend->hdr.version = 0;
	cs->nextsend->hdr.opcode = OPCODE_GETHDR_REQ;
	native_to_big_inplace(cs->nextsend->gethdrreq.hdr.reserved2 = 0);
	native_to_big_inplace(cs->nextsend->gethdrreq.hdr.hostid = 0);
	native_to_big_inplace(cs->nextsend->gethdrreq.uid = 1);

	cs->send_mr = ibv_reg_mr(cs->id->pd, cs->nextsend,
				       sizeof(*cs->nextsend), 0);
	if (!cs->send_mr)
		std::terminate();

	ret = rdma_post_send(cs->id, cs->nextsend, cs->nextsend,
			     sizeof(*cs->nextsend),
			     cs->send_mr, IBV_SEND_SIGNALED);
	if (ret) {
		std::cerr << "rdma post send returned " << ret << " errno " << errno << "\n";
		abort();
	}

	struct ibv_wc wc[32];
	int count;
	while ((count = ibv_poll_cq(cs->id->recv_cq, 32, wc)) >= 0) {
		for (int i = 0; i < count; i++) {
			process_wc(cs, &wc[i]);
		}
	}
}

int main(int argc, char *argv[])
{
	run(argv[1]);
	exit(EXIT_SUCCESS);
}
