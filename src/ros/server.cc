/** \file server.cc
 * \author Patrick MacArthur <patrick@patrickmacarthur.net>
 */

#include <cstddef>
#include <cstdint>
#include <cstdlib>

#include <exception>
#include <iostream>

#include <netdb.h>

#include <rdma/rdma_cma.h>
#include <rdma/rdma_verbs.h>

#include "ros.h"

static const int CACHE_LINE_SIZE = 64;

static_assert(sizeof(struct MessageHeader) == 8, "incorrect size for MessageHeader");
static_assert(offsetof(struct AnnounceMessage, hdr.reserved2) == 2, "wrong offset for reserved2");
static_assert(offsetof(struct AnnounceMessage, reserved28) == 28, "wrong offset for reserved28");
static_assert(sizeof(struct AnnounceMessage) == 32, "incorrect size for LockAnnounceMessage");
static_assert(offsetof(struct GetHdrRequest, hdr.reserved2) == 2, "wrong offset for reserved2");
static_assert(sizeof(struct GetHdrRequest) == 16, "incorrect size for GetHdrRequest");
static_assert(offsetof(struct GetHdrResponse, hdr.reserved2) == 2, "wrong offset for reserved2");
static_assert(offsetof(struct GetHdrResponse, reserved36) == 36, "wrong offset for reserved36");
static_assert(sizeof(struct GetHdrResponse) == 40, "incorrect size for GetHdrResponse");

void process_announce(struct ConnState *cs, struct AnnounceMessage *msg)
{
	std::terminate();
}

void process_gethdrreq(struct ConnState *cs, struct GetHdrRequest *msg)
{
	std::cout << "gethdr request for object " << msg->uid << "\n";
}

void process_gethdrresp(struct ConnState *cs, struct GetHdrResponse *msg)
{
	std::terminate();
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

	cs->announce = reinterpret_cast<struct AnnounceMessage *>(
			aligned_alloc(CACHE_LINE_SIZE, sizeof(*cs->announce)));
	if (!cs->announce)
		return;
	cs->announce->hdr.version = 0;
	cs->announce->hdr.opcode = OPCODE_ANNOUNCE;
	cs->announce->hdr.reserved2 = 0;
	cs->announce->hdr.hostid = 0x12345678;

	cs->announce_mr = ibv_reg_mr(cs->id->pd, cs->announce,
				     sizeof(*cs->announce), 0);
	if (!cs->announce_mr) {
		free(cs->announce);
		return;
	}

	ret = rdma_post_send(cs->id, cs,
			     reinterpret_cast<void *>(cs->announce),
			     sizeof(*cs->announce), cs->announce_mr,
			     IBV_SEND_SIGNALED);
	if (ret)
		return;

	struct ibv_wc wc[32];
	int count;
	while ((count = ibv_poll_cq(cs->id->recv_cq, 32, wc)) != 0) {
		for (int i = 0; i < count; i++) {
			process_wc(cs, &wc[i]);
		}
	}
}

void *conn_thread(void *arg)
{
	handle_connection(reinterpret_cast<struct ConnState *>(arg));
	return NULL;
}

void init_tree_root(struct ibv_pd *pd)
{
	root_obj = reinterpret_cast<struct TreeRoot *>(
			aligned_alloc(CACHE_LINE_SIZE, sizeof(*root_obj)));
	if (!root_obj)
		std::terminate();
	memset(root_obj, 0, sizeof(*root_obj));
	root_obj->objhdr.uid = 1;

	root_obj_mr = ibv_reg_mr(pd, root_obj, sizeof(*root_obj),
				 IBV_ACCESS_LOCAL_WRITE|IBV_ACCESS_REMOTE_READ
				 |IBV_ACCESS_REMOTE_WRITE);
	if (!root_obj_mr) {
		std::terminate();
	}
}

struct ibv_pd *get_pd()
{
	struct ibv_context **dev = rdma_get_devices(NULL);
	struct ibv_pd *pd = ibv_alloc_pd(*dev);
	rdma_free_devices(dev);
	return pd;

}

void run()
{
	struct rdma_addrinfo hints, *rai;
	struct rdma_cm_id *listen_id, *id;
	struct ConnState *cs;
	int ret;

	memset(&hints, 0, sizeof(hints));
	hints.ai_flags = RAI_PASSIVE;
	hints.ai_port_space = RDMA_PS_TCP;

	ret = rdma_getaddrinfo("mundilfari-iw", "9001", &hints, &rai);
	if (ret)
		exit(EXIT_FAILURE);

	ret = rdma_create_ep(&listen_id, rai, get_pd(), NULL);
	if (ret)
		exit(EXIT_FAILURE);

	init_tree_root(listen_id->pd);

	ret = rdma_listen(listen_id, 0);
	if (ret)
		exit(EXIT_FAILURE);

	struct sockaddr *sa = rdma_get_local_addr(listen_id);
	char userhost[20];
	char userport[20];
	getnameinfo(sa, sizeof(struct sockaddr_in6), userhost, 20, userport, 20, 0);
	std::cerr << "Listening on " << userhost << ":" << userport << "\n";

	while (1) {
		ret = rdma_get_request(listen_id, &id);
		if (ret)
			exit(EXIT_FAILURE);

		std::cerr << "Got connection!\n";

		cs = new ConnState;
		cs->id = id;
		pthread_t tid;
		ret = pthread_create(&tid, NULL, conn_thread, cs);
		if (ret)
			exit(EXIT_FAILURE);
	}
}

int main(int argc, char *argv[])
{
	run();
	exit(EXIT_SUCCESS);
}
