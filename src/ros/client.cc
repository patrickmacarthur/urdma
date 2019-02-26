/** \file server.cc
 * \author Patrick MacArthur <patrick@patrickmacarthur.net>
 */

#include <cstddef>
#include <cstdint>
#include <cstdlib>

#include <exception>
#include <iomanip>
#include <iostream>
#include <type_traits>

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

namespace {

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
	throw "not implemented";
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

std::string get_first_announce(uint64_t cluster_id)
{
	struct addrinfo hints, *ai;
	ssize_t ret;

	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_DGRAM;
	ret = getaddrinfo(ros_mcast_addr, ros_mcast_port, &hints, &ai);
	if (ret) {
		throw std::system_error(ret, gai_category());
	}

	int mcfd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
	CHECK_ERRNO(mcfd);
	CHECK_ERRNO(connect(mcfd, ai->ai_addr, ai->ai_addrlen));

	struct QueryServersMessage sendmsg;
	sendmsg.hdr.version = 0;
	sendmsg.hdr.opcode = OPCODE_QUERY_SERVERS;
	native_to_big_inplace(sendmsg.hdr.reserved2 = 0);
	native_to_big_inplace(sendmsg.hdr.hostid = 0);
	native_to_big_inplace(sendmsg.reserved8 = 0);
	native_to_big_inplace(sendmsg.cluster_id = cluster_id);

	CHECK_ERRNO(send(mcfd, &sendmsg, sizeof(sendmsg), 0));

	union MessageBuf recvmsg;
	do {
		ret = recv(mcfd, &recvmsg, sizeof(recvmsg), 0);
		CHECK_ERRNO(ret);
	} while (recvmsg.hdr.version != 0
			|| recvmsg.hdr.opcode != OPCODE_ANNOUNCE
			|| big_to_native(recvmsg.announce.cluster_id) != cluster_id);

	struct in_addr inaddr{recvmsg.announce.rdma_ipv4_addr};
	char addrbuf[INET_ADDRSTRLEN];
	CHECK_PTR_ERRNO(inet_ntop(AF_INET, &inaddr, addrbuf, INET_ADDRSTRLEN));
	return addrbuf;
}

void run(const char *cluster_id_str)
{
	struct ConnState *cs;
	struct rdma_addrinfo hints, *rai;
	struct ibv_qp_init_attr attr;
	struct rdma_conn_param cparam;
	struct ibv_cq *cq;
	struct ibv_wc wc;
	uint64_t cluster_id;
	void *cq_context;
	std::string host;
	int ret;

	std::istringstream iss;
	iss >> std::hex >> cluster_id;

	host = get_first_announce(cluster_id);

	cs = new ConnState;

	memset(&hints, 0, sizeof(hints));
	hints.ai_flags = 0;
	hints.ai_port_space = RDMA_PS_TCP;

	CHECK_ERRNO(rdma_getaddrinfo(host.c_str(), "9001", &hints, &rai));

	memset(&attr, 0, sizeof(attr));
	attr.qp_type = IBV_QPT_RC;
	attr.cap.max_send_wr = 64;
	attr.cap.max_recv_wr = 64;
	CHECK_ERRNO(rdma_create_ep(&cs->id, rai, NULL, &attr));

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
	CHECK_ERRNO(rdma_connect(cs->id, &cparam));

	CHECK_ERRNO(rdma_get_recv_comp(cs->id, &wc));
	process_wc(cs, &wc);

	cs->nextsend = reinterpret_cast<union MessageBuf *>(
			aligned_alloc(CACHE_LINE_SIZE, sizeof(*cs->nextsend)));
	CHECK_PTR_ERRNO(cs->nextsend);
	cs->nextsend->hdr.version = 0;
	cs->nextsend->hdr.opcode = OPCODE_GETHDR_REQ;
	native_to_big_inplace(cs->nextsend->gethdrreq.hdr.reserved2 = 0);
	native_to_big_inplace(cs->nextsend->gethdrreq.hdr.hostid = 0);
	native_to_big_inplace(cs->nextsend->gethdrreq.uid = 1);

	cs->send_mr = ibv_reg_mr(cs->id->pd, cs->nextsend,
				 sizeof(*cs->nextsend), 0);
	CHECK_PTR_ERRNO(cs->send_mr);

	CHECK_ERRNO(rdma_post_send(cs->id, cs->nextsend, cs->nextsend,
				   sizeof(*cs->nextsend),
				   cs->send_mr, IBV_SEND_SIGNALED));
	std::cerr << "packet sent\n";

	CHECK_ERRNO(rdma_get_recv_comp(cs->id, &wc));
	process_wc(cs, &wc);
}

}

int main(int argc, char *argv[])
{
	run(argv[1]);
	exit(EXIT_SUCCESS);
}
