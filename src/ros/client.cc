/** \file server.cc
 * \author Patrick MacArthur <patrick@patrickmacarthur.net>
 */

#include <cstddef>
#include <cstdint>
#include <cstdlib>

#include <exception>
#include <future>
#include <iomanip>
#include <iostream>
#include <map>
#include <type_traits>

#include <boost/endian/conversion.hpp>
#include <boost/format.hpp>

#include <arpa/inet.h>
#include <netdb.h>
#include <rdma/rdma_cma.h>
#include <rdma/rdma_verbs.h>

#include "ros.h"
#include "ROSPtr.hpp"
#include "gai_category.h"

using boost::endian::big_to_native;
using boost::endian::native_to_big_inplace;
using boost::format;

namespace {

struct ROSRemoteObject {
	uint64_t uid;
	uint32_t rkey;
};

std::map<unsigned long, struct ClientConnState *> cnxions;

void process_announce(struct ClientConnState *cs, struct AnnounceMessage *msg)
{
	cs->server_hostid = big_to_native(msg->hdr.hostid);
	cs->remote_rkey = big_to_native(msg->pool_rkey);
	std::cout << format("announce from hostid %x\n") % cs->server_hostid;
	std::cout << format("rkey is %x\n") % cs->remote_rkey;
	std::cout.flush();
	cnxions.insert(std::make_pair(cs->server_hostid, cs));
}

void process_gethdrreq(struct ClientConnState *cs, struct GetHdrRequest *msg)
{
	throw "not implemented";
}

void process_gethdrresp(struct ClientConnState *cs, struct GetHdrResponse *msg)
{
	std::cout << format("gethdr response for object %x remote addr %x rkey %x\n")
			% big_to_native(msg->uid)
			% big_to_native(msg->addr) % big_to_native(msg->rkey);
	std::cout.flush();
}

void process_wc(struct ClientConnState *cs, struct ibv_wc *wc)
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

using msg_promise = std::promise<union MessageBuf *>;
using msg_future = std::future<union MessageBuf *>;
using promise_map = std::map<unsigned long, msg_promise>;

void completion_thread(struct ibv_comp_channel *chan, promise_map &wc_promises)
{
	static const size_t max_wc_count = 8;
	struct ibv_cq *cq;
	struct ibv_wc wc[max_wc_count];
	void *context;
	int event_cnt = 0;
	int ret;
	do {
		ret = ibv_get_cq_event(chan, &cq, &context);
		if (ret)
			goto err;
		event_cnt++;
		ret = ibv_req_notify_cq(cq, 0);
		if (ret)
			goto err;
		ret = ibv_poll_cq(cq, max_wc_count, wc);
		if (ret < 0)
			goto err;
		for (int i = 0; i < ret; i++) {
			if (wc->status == IBV_WC_WR_FLUSH_ERR)
				goto err;
			auto msg = reinterpret_cast<union MessageBuf *>(wc->wr_id);
			auto req_id = big_to_native(msg->hdr.req_id);
			auto iter = wc_promises.find(req_id);
			if (iter != wc_promises.end()) {
				if (!wc->status) {
					iter->second.set_value(msg);
				}
				wc_promises.erase(iter);
			} else {
				std::cerr << format("Unexpected message with opcode %u and req_id %x")
					% msg->hdr.opcode % req_id;
			}
		}
	} while (1);

err:
	wc_promises.erase(wc_promises.begin(), wc_promises.end());
}

std::string get_first_announce(struct sockaddr_in *local_addr, uint64_t cluster_id)
{
	struct addrinfo hints, *ai;
	ssize_t ret;

	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_DGRAM;
	ret = getaddrinfo(ros_mcast_addr, "9002", &hints, &ai);
	if (ret) {
		throw std::system_error(ret, gai_category());
	}

	int mcfd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
	CHECK_ERRNO(mcfd);
	struct sockaddr_in any;
	any.sin_family = AF_INET;
	any.sin_addr = in_addr{INADDR_ANY};
	native_to_big_inplace(any.sin_port = ros_mcast_port);
	CHECK_ERRNO(bind(mcfd, (struct sockaddr *)&any, sizeof(any)));
	int val = 1;
	CHECK_ERRNO(setsockopt(mcfd, IPPROTO_IP, IP_MULTICAST_ALL,
				&val, sizeof(val)));
	struct ip_mreqn mreq;
	CHECK_ERRNO(inet_pton(AF_INET, ros_mcast_addr, &mreq.imr_multiaddr));
	mreq.imr_address = local_addr->sin_addr;
	mreq.imr_ifindex = 0;
	CHECK_ERRNO(setsockopt(mcfd, IPPROTO_IP, IP_ADD_MEMBERSHIP,
				&mreq, sizeof(mreq)));

	struct sockaddr_storage localaddr_store;
#if 0
	auto localaddr = reinterpret_cast<struct sockaddr *>(&localaddr_store);
	socklen_t localaddr_len = sizeof(localaddr_store);
	getsockname(mcfd, localaddr, &localaddr_len);
	char mchost[256];
	char mcport[10];
	ret = getnameinfo(localaddr, localaddr_len, mchost, 256, mcport, 10,
			NI_NUMERICSERV);
	if (ret)
		throw std::system_error(ret, gai_category());
	std::cerr << "Multicast socket bound to " << mchost << ":" << mcport << "\n";
#endif

	struct QueryServersMessage sendmsg;
	sendmsg.hdr.version = 0;
	sendmsg.hdr.opcode = OPCODE_QUERY_SERVERS;
	native_to_big_inplace(sendmsg.hdr.req_id = 0);
	native_to_big_inplace(sendmsg.hdr.hostid = 0);
	native_to_big_inplace(sendmsg.reserved8 = 0);
	native_to_big_inplace(sendmsg.cluster_id = cluster_id);

	CHECK_ERRNO(sendto(mcfd, &sendmsg, sizeof(sendmsg), 0,
				ai->ai_addr, ai->ai_addrlen));

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

void run(const char *local_ip, const char *cluster_id_str)
{
	struct rdma_addrinfo hints, *rai;
	struct ibv_qp_init_attr attr;
	struct rdma_conn_param cparam;
	struct addrinfo *ai;
	struct ibv_cq *cq;
	struct ibv_wc wc;
	uint64_t cluster_id;
	void *cq_context;
	std::string host;
	char *endp;
	int ret;

	errno = 0;
	cluster_id = strtoull(cluster_id_str, &endp, 16);
	if (errno || *endp != '\0') {
		throw format("bad cluster id \"%s\"") % cluster_id;
	}
	std::cerr << format("cluster id is %x\n") % cluster_id;

	ret = getaddrinfo(local_ip, NULL, NULL, &ai);
	if (ret)
		throw std::system_error(ret, gai_category());
	host = get_first_announce(reinterpret_cast<struct sockaddr_in *>(ai->ai_addr),
				  cluster_id);
	freeaddrinfo(ai);
	std::cerr << format("server is at %s\n") % host;

	auto *cs = new ClientConnState;

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
				 sizeof(cs->recv_bufs),
				 IBV_ACCESS_LOCAL_WRITE);
	if (!cs->recv_mr) {
		rdma_reject(cs->id, NULL, 0);
		return;
	}

	ibv_req_notify_cq(cs->id->send_cq, 0);
	promise_map send_wc_promises;
	std::thread send_cq_thread{completion_thread,
				   cs->id->send_cq_channel,
				   std::ref(send_wc_promises)};
	ibv_req_notify_cq(cs->id->recv_cq, 0);
	promise_map recv_wc_promises;
	std::thread recv_cq_thread{completion_thread,
				   cs->id->recv_cq_channel,
				   std::ref(recv_wc_promises)};

	msg_promise announce_promise;
	msg_future announce_future = announce_promise.get_future();
	recv_wc_promises.insert(std::make_pair(0, std::move(announce_promise)));

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

	process_announce(cs, &announce_future.get()->announce);

	cs->nextsend = reinterpret_cast<union MessageBuf *>(
			aligned_alloc(CACHE_LINE_SIZE, sizeof(*cs->nextsend)));
	CHECK_PTR_ERRNO(cs->nextsend);
	cs->nextsend->hdr.version = 0;
	cs->nextsend->hdr.opcode = OPCODE_GETHDR_REQ;
	auto req_id = cs->next_req_id++;
	native_to_big_inplace(cs->nextsend->gethdrreq.hdr.req_id = req_id);
	native_to_big_inplace(cs->nextsend->gethdrreq.hdr.hostid = 0);
	native_to_big_inplace(cs->nextsend->gethdrreq.uid = 1);

	cs->send_mr = ibv_reg_mr(cs->id->pd, cs->nextsend,
				 sizeof(*cs->nextsend), 0);
	CHECK_PTR_ERRNO(cs->send_mr);

	msg_promise gethdrresp_promise;
	msg_future gethdrresp_future = gethdrresp_promise.get_future();
	recv_wc_promises.insert(std::make_pair(req_id, std::move(gethdrresp_promise)));
	CHECK_ERRNO(rdma_post_send(cs->id, cs->nextsend, cs->nextsend,
				   sizeof(*cs->nextsend),
				   cs->send_mr, IBV_SEND_SIGNALED));

	process_gethdrresp(cs, &announce_future.get()->gethdrresp);
}

}

int main(int argc, char *argv[])
{
	if (argc != 3)
		throw "not enough arguments";
	run(argv[1], argv[2]);
	exit(EXIT_SUCCESS);
}
