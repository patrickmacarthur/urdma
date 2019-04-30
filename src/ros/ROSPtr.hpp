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

using msg_promise = std::promise<union MessageBuf *>;
using msg_future = std::future<union MessageBuf *>;
using promise_map = std::map<unsigned long, msg_promise>;

extern std::map<unsigned long, struct ClientConnState *> cnxions;

template <typename T>
class ROSPtr {
public:
	ROSPtr(uint64_t obj_uid);
	~ROSPtr();
	typename ROSPtr<T>::Handle &operator *();
	typename ROSPtr<T>::Handle &operator ->();
	operator bool() const;
private:
	class Handle
	{
	public:
		Handle(ROSPtr<T> *parent);
		~Handle();
		Handle &operator =(const T &);
		operator T &();
		operator const T &() const;
		void pushRange(unsigned first_byte, unsigned last_byte) const;
		void pullRange(unsigned first_byte, unsigned last_byte);
	private:
		struct ibv_mr *mr;
		ROSPtr<T> *parent;
		uint64_t remote_addr;
		T *realptr;
	};

	uint64_t uid;
	Handle *handle;
};

struct ClientConnState {
	struct rdma_cm_id *id;
	struct ibv_mr *send_mr;
	struct ibv_mr *recv_mr;
	unsigned long server_hostid;
	uint32_t remote_rkey;
	uint16_t next_req_id;
	union MessageBuf *nextsend;
	union MessageBuf recv_bufs[32];
	promise_map recv_wc_promises;
	promise_map send_wc_promises;
};

template <typename T>
ROSPtr<T>::ROSPtr(uint64_t obj_uid) : uid(obj_uid), handle(nullptr)
{
}

template <typename T>
ROSPtr<T>::~ROSPtr()
{
}

template <typename T>
typename ROSPtr<T>::Handle &ROSPtr<T>::operator *()
{
	if (!handle)
		handle = new Handle(this);
	return handle;
}

template <typename T>
typename ROSPtr<T>::Handle &ROSPtr<T>::operator ->()
{
	if (!handle)
		handle = new Handle(this);
	return handle;
}

template <typename T>
ROSPtr<T>::operator bool() const
{
	return true;
}

template <typename T>
ROSPtr<T>::Handle::Handle(ROSPtr<T> *parent) : parent(parent)
{
	auto *cs = cnxions.find(this->uid >> 32)->second;
	struct GetHdrMessage *msg = reinterpret_cast<union MessageBuf *>(
			aligned_alloc(CACHE_LINE_SIZE, sizeof(*cs->nextsend)));
	auto req_id = ++cs->next_req_id;
	CHECK_PTR_ERRNO(cs->nextsend);
	cs->nextsend->hdr.version = 0;
	cs->nextsend->hdr.opcode = OPCODE_GETHDR_REQ;
	native_to_big_inplace(cs->nextsend->gethdrreq.hdr.req_id = req_id);
	native_to_big_inplace(cs->nextsend->gethdrreq.hdr.hostid = 0);
	native_to_big_inplace(cs->nextsend->gethdrreq.uid = parent->uid);

	msg_promise send_promise;
	msg_future send_future = send_promise.get_future();
	cs->recv_wc_promises.insert(make_pair(req_id, std::move(send_promise)));
	CHECK_ERRNO(rdma_post_send(cs->id, cs->nextsend, cs->nextsend,
				   sizeof(*cs->nextsend),
				   nullptr, IBV_SEND_SIGNALED|IBV_SEND_INLINE));

	realptr = reinterpret_cast<T *>(aligned_alloc(CACHE_LINE_SIZE, page_size));
	mr = ibv_reg_mr(cs->id->pd, realptr, page_size, IBV_ACCESS_REMOTE_WRITE);
	if (!mr) {
		throw std::system_error(errno, std::system_category());
	}

	auto *response = reinterpret_cast<union MessageBuf *>(send_future.get());
	if (response->hdr.opcode != OPCODE_GETHDR_RESP) {
		throw "???";
	}
	big_to_native_inplace(this->remote_addr = response->gethdrresp.addr);
	uint32_t rkey = big_to_native(this->rkey = response->gethdrresp.rkey);

	pullRange(0, sizeof(T));
}

template <typename T>
ROSPtr<T>::Handle::~Handle()
{
	pushRange(0, sizeof(T));
}

template <typename T>
void ROSPtr<T>::Handle::pullRange(unsigned first_pos, unsigned last_pos)
{
	auto *cs = cnxions.find(this->uid >> 32)->second;
	auto req_id = ++cs->next_req_id;
	msg_promise read_promise;
	msg_future read_future = read_promise.get_future();
	cs->send_wc_promises.insert(make_pair(req_id, read_promise));
	auto startp = reinterpret_cast<uint8_t *>(realptr) + first_pos;
	rdma_post_read(cs->id, startp, startp, last_pos - first_pos, mr, 0,
			this->remote_addr + first_pos, cs->rkey);
	read_future.get();
}

template <typename T>
void ROSPtr<T>::Handle::pushRange(unsigned first_pos, unsigned last_pos) const
{
	auto *cs = cnxions.find(this->uid >> 32)->second;
	auto req_id = ++cs->next_req_id;
	msg_promise write_promise;
	msg_future write_future = write_promise.get_future();
	cs->send_wc_promises.insert(make_pair(req_id, write_future));
	auto startp = reinterpret_cast<uint8_t *>(realptr) + first_pos;
	rdma_post_write(cs->id, startp, startp, last_pos - first_pos, mr, 0,
			this->remote_addr + first_pos, cs->rkey);
	write_future.get();
}

template <typename T>
typename ROSPtr<T>::Handle &ROSPtr<T>::Handle::operator = (const T &other)
{
	*realptr = other;
	return *this;
}

template <typename T>
ROSPtr<T>::Handle::operator T &()
{
	return *realptr;
}

template <typename T>
ROSPtr<T>::Handle::operator const T &() const
{
	return *realptr;
}
