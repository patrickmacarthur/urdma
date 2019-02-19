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

void process_announce(struct ConnState *cs, struct AnnounceMessage *msg)
{
	std::cout << format("announce from hostid %x\n")
			% big_to_native(msg->hdr.hostid);
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
	struct ibv_mr *recv_mr;
	union MessageBuf recv_bufs[32];
	struct rdma_addrinfo hints, *rai;
	struct ibv_qp_init_attr attr;
	struct rdma_conn_param cparam;
	struct rdma_cm_id *id;
	int ret;

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
	ret = rdma_create_ep(&id, rai, NULL, &attr);
	if (ret)
		std::terminate();

	recv_mr = ibv_reg_mr(id->pd, recv_bufs,
				 sizeof(recv_bufs), 0);
	if (!recv_mr) {
		rdma_reject(id, NULL, 0);
		return;
	}

	for (int i = 0; i < 32; i++) {
		ret = rdma_post_recv(id, recv_bufs[i].buf,
				     recv_bufs[i].buf,
				     sizeof(recv_bufs[i]), recv_mr);
		if (ret) {
			rdma_reject(id, NULL, 0);
			return;
		}
	}

	memset(&cparam, 0, sizeof(cparam));
	cparam.initiator_depth = 1;
	cparam.responder_resources = 1;
	ret = rdma_connect(id, &cparam);
	if (ret) {
		std::cerr << "rdma_connect returned " << ret << " errno " << errno << "\n";
		std::terminate();
	}

	struct GetHdrRequest *gethdr_req_msg = reinterpret_cast<struct GetHdrRequest *>(
			aligned_alloc(CACHE_LINE_SIZE, sizeof(*gethdr_req_msg)));
	if (!gethdr_req_msg)
		std::terminate();
	gethdr_req_msg->hdr.version = 0;
	gethdr_req_msg->hdr.opcode = OPCODE_GETHDR_REQ;
	native_to_big_inplace(gethdr_req_msg->hdr.reserved2 = 0);
	native_to_big_inplace(gethdr_req_msg->hdr.hostid = 0);
	native_to_big_inplace(gethdr_req_msg->uid = 1);

	struct ibv_mr *mr = ibv_reg_mr(id->pd, gethdr_req_msg,
				       sizeof(*gethdr_req_msg), 0);
	if (!mr)
		std::terminate();

	ret = rdma_post_send(id, gethdr_req_msg, gethdr_req_msg,
			     sizeof(*gethdr_req_msg),
			     mr, IBV_SEND_SIGNALED);
	if (ret) {
		std::cerr << "rdma post send returned " << ret << " errno " << errno << "\n";
		abort();
	}

	struct ibv_wc wc[32];
	int count;
	while ((count = ibv_poll_cq(id->recv_cq, 32, wc)) >= 0) {
		for (int i = 0; i < count; i++) {
			process_wc(NULL, &wc[i]);
		}
	}
}

int main(int argc, char *argv[])
{
	run(argv[1]);
	exit(EXIT_SUCCESS);
}
