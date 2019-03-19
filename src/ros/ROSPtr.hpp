/* \file src/ros/ROSPtr.cc
 * \author Patrick MacArthur <patrick@patrickmacarthur.net>
 */

#include "ros.h"

#include <cstdint>

#include <exception>
#include <future>
#include <map>

#include <boost/endian/conversion.hpp>

#include <rdma/rdma_verbs.h>

struct ClientConnState;

extern std::map<unsigned long, struct ClientConnState *> cnxions;

namespace {
void process_wc(struct ClientConnState *cs, struct ibv_wc *wc);
}

template <typename T>
class ROSPtr {
public:
	ROSPtr(uint64_t obj_uid);
	~ROSPtr();
	T *operator *();
	T *operator ->();
	operator bool() const;
private:
	void obtain_obj();
	uint64_t uid;
	T *realptr;
	uint64_t remote_addr;
};

struct ClientConnState {
	struct rdma_cm_id *id;
	struct ibv_mr *send_mr;
	struct ibv_mr *recv_mr;
	unsigned long server_hostid;
	uint32_t remote_rkey;
	uint16_t next_req_id;
	struct ROSPtr<int> root;
	union MessageBuf *nextsend;
	union MessageBuf recv_bufs[32];

	ClientConnState() : root(1) {}
};

template <typename T>
ROSPtr<T>::ROSPtr(uint64_t obj_uid) : uid(obj_uid), realptr(nullptr)
{
}

template <typename T>
ROSPtr<T>::~ROSPtr()
{
}

template <typename T>
T *ROSPtr<T>::operator *()
{
	if (!realptr)
		obtain_obj();
	return realptr;
}

template <typename T>
T *ROSPtr<T>::operator ->()
{
	if (!realptr)
		obtain_obj();
	return realptr;
}

template <typename T>
ROSPtr<T>::operator bool() const
{
	return true;
}

template <typename T>
void ROSPtr<T>::obtain_obj()
{
	auto *cs = cnxions.find(this->uid >> 32)->second;
	struct GetHdrMessage *msg = reinterpret_cast<union MessageBuf *>(
			aligned_alloc(CACHE_LINE_SIZE, sizeof(*cs->nextsend)));
	auto req_id = cs->next_req_id++;
	CHECK_PTR_ERRNO(cs->nextsend);
	cs->nextsend->hdr.version = 0;
	cs->nextsend->hdr.opcode = OPCODE_GETHDR_REQ;
	native_to_big_inplace(cs->nextsend->gethdrreq.hdr.req_id = req_id);
	native_to_big_inplace(cs->nextsend->gethdrreq.hdr.hostid = 0);
	native_to_big_inplace(cs->nextsend->gethdrreq.uid = this->uid);

	msg_promise send_promise;
	msg_future send_future = send_promise.get_future();
	recv_wc_promises.insert(make_pair(req_id, &send_promise));
	CHECK_ERRNO(rdma_post_send(cs->id, cs->nextsend, cs->nextsend,
				   sizeof(*cs->nextsend),
				   nullptr, IBV_SEND_SIGNALED|IBV_SEND_INLINE));

	realptr = reinterpret_cast<T *>(aligned_alloc(CACHE_LINE_SIZE, page_size));
	auto *mr = ibv_reg_mr(cs->id->pd, realptr, page_size, IBV_ACCESS_REMOTE_WRITE);
	if (!mr) {
		throw std::system_error(errno, std::system_category());
	}

	auto *response = reinterpret_cast<union MessageBuf *>(send_future.get());
	if (response->hdr.opcode != OPCODE_GETHDR_RESP) {
		throw "???";
	}
	big_to_native_inplace(this->remote_addr = response->gethdrresp.addr);
	uint32_t rkey = big_to_native(this->rkey = response->gethdrresp.rkey);

	msg_promise read_promise;
	msg_future read_future = send_promise.get_future();
	send_wc_promises.insert(make_pair(req_id, &read_promise));
	rdma_post_read(cs->id, this, realptr, page_size, mr, 0, this->remote_addr, rkey);
	read_future.get();
}
