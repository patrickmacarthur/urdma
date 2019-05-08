#include <rdma/rdma_verbs.h>

namespace rdma {

using msg_promise = std::promise<union MessageBuf *>;
using msg_future = std::future<union MessageBuf *>;
using promise_map = std::map<unsigned long, msg_promise>;

class RdmaConnection {
public:
	RdmaConnection(const std::string &host, const std::string &port,
			unsigned recv_queue_size, size_t control_msg_size);
	~RdmaConnection();
	msg_future getRecvFuture(uint16_t req_id);

private:
	struct rdma_cm_id *id;
	struct ibv_mr *send_mr;
	struct ibv_mr *recv_mr;
	uint16_t next_req_id;
	size_t control_msg_size;
	BufferPool *recv_pool;
	std::thread *send_cq_thread;
	std::thread *recv_cq_thread;
	promise_map recv_wc_promises;
	promise_map send_wc_promises;
};

RdmaConnection::RdmaConnection(const std::string &host, const std::string &port,
		unsigned recv_queue_size, size_t control_msg_size) {
	struct rdma_addrinfo hints;
	struct ibv_qp_init_attr attr;
	struct rdma_conn_param cparam;
	struct addrinfo *ai;
	struct ibv_cq *cq;
	struct ibv_wc wc;

	memset(&hints, 0, sizeof(hints));
	hints.ai_flags = 0;
	hints.ai_port_space = RDMA_PS_TCP;

	struct rdma_addrinfo *rai;
	if (rdma_getaddrinfo(host.c_str(), "9001", &hints, &rai)) {
		throw std::system_error{errno, std::system_category()};
	}

	memset(&attr, 0, sizeof(attr));
	attr.qp_type = IBV_QPT_RC;
	attr.cap.max_send_wr = 64;
	attr.cap.max_recv_wr = 64;
	if (rdma_create_ep(&id, rai, NULL, &attr)) {
		throw std::system_error{errno, std::system_category()};
	}

	try {
		recv_pool = new BufferPool(recv_queue_size, control_msg_size,
				IBV_ACCESS_LOCAL_WRITE, id->pd);
	} catch (...) {
		rdma_reject(id, NULL, 0);
		throw;
	}

	ibv_req_notify_cq(id->send_cq, 0);
	send_cq_thread = new std::thread{completion_thread,
				   id->send_cq_channel,
				   std::ref(send_wc_promises)};
	ibv_req_notify_cq(id->recv_cq, 0);
	recv_cq_thread = new std::thread{completion_thread,
				   id->recv_cq_channel,
				   std::ref(recv_wc_promises)};

	memset(&cparam, 0, sizeof(cparam));
	cparam.initiator_depth = 1;
	cparam.responder_resources = 1;
	if (rdma_connect(id, &cparam)) {
		throw std::system_error{errno, std::system_category()};
	}
}

RdmaConnection::~RdmaConnection()
{
	rdma_disconnect(id);
	recv_cq_thread.join();
	send_cq_thread.join();
	rdma_destroy_ep(id);
}

msg_future RdmaConnection::getRecvFuture(uint16_t req_id)
{
	msg_promise send_promise;
	msg_future send_future = send_promise.get_future();
	conn->recv_wc_promises.insert(make_pair(req_id, std::move(send_promise)));
	return send_future;
}

}
