/* \file src/ros/ROSLock.cc
 * \author Patrick MacArthur <patrick@patrickmacarthur.net>
 */

#include "ROSLock.hpp"
#include "ROSPtr.hpp"

#include "verbs.h"
#include <rdma/rdma_verbs.h>
#include <boost/endian/conversion.hpp>

using boost::endian::native_to_big_inplace;

RPCPollLock::RPCPollLock(RdmaConnection *conn, BufferPool *pool,
		uint64_t lock_id, uint32_t lock_key)
	: conn(conn), held(false), lock_id(lock_id), lock_key(lock_key)
{
}

bool RPCPollLock::tryLock()
{
	std::unique_ptr<struct LockRequest> msg{pool->borrow<>(),
		(x) -> { pool->give(x) }};
	auto req_id = ++conn->next_req_id;
	CHECK_PTR_ERRNO(msg);
	msg->hdr.version = 0;
	msg->hdr.opcode = OPCODE_LOCK_POLL_REQ;
	native_to_big_inplace(msg->hdr.req_id = req_id);
	native_to_big_inplace(msg->hdr.hostid = 0);
	native_to_big_inplace(msg->lock_id = lock_id);
	native_to_big_inplace(msg->lock_key = lock_key);

	conn->getRecvFuture(req_id);
	CHECK_ERRNO(rdma_post_send(conn->id, msg, msg, sizeof(*msg), nullptr,
				   IBV_SEND_SIGNALED|IBV_SEND_INLINE));

	struct LockResponse *resp = &send_future.get()->lockresp;
	if (resp->status != 0) {
		held = true;
	}
	return held;
}

void RPCPollLock::lock()
{
	do {
		tryLock();
	} while (!held);
}

void RPCPollLock::unlock() noexcept
{
	try {
		struct LockRequest *msg = reinterpret_cast<struct LockRequest *>(
				aligned_alloc(CACHE_LINE_SIZE, sizeof(*msg)));
		auto req_id = ++conn->next_req_id;
		CHECK_PTR_ERRNO(msg);
		msg->hdr.version = 0;
		msg->hdr.opcode = OPCODE_UNLOCK_REQ;
		native_to_big_inplace(msg->hdr.req_id = req_id);
		native_to_big_inplace(msg->hdr.hostid = 0);
		native_to_big_inplace(msg->lock_id = lock_id);
		native_to_big_inplace(msg->lock_key = lock_key);
		held = false;
	} catch (...) {
		/* Can't really do anything about it */
	}
}

RPCQueueLock::RPCQueueLock(RdmaConnection *conn, BufferPool *pool,
		uint64_t lock_id, uint32_t lock_key)
	: conn(conn), held(false), lock_id(lock_id), lock_key(lock_key)
{
}

bool RPCQueueLock::tryLock()
{
	throw "not implemented";
}

void RPCQueueLock::lock()
{
	std::unique_ptr<struct LockRequest> msg{pool->borrow<>(),
		(x) -> { pool->give(x) }};
	auto req_id = ++conn->next_req_id;
	CHECK_PTR_ERRNO(msg);
	msg->hdr.version = 0;
	msg->hdr.opcode = OPCODE_LOCK_QUEUE_REQ;
	native_to_big_inplace(msg->hdr.req_id = req_id);
	native_to_big_inplace(msg->hdr.hostid = 0);
	native_to_big_inplace(msg->lock_id = lock_id);
	native_to_big_inplace(msg->lock_key = lock_key);

	struct LockResponse *resp;
	msg_promise send_promise;
	msg_future send_future = send_promise.get_future();
	conn->recv_wc_promises.insert(make_pair(req_id, std::move(send_promise)));
	CHECK_ERRNO(rdma_post_send(conn->id, msg, msg, sizeof(*msg), nullptr,
				   IBV_SEND_SIGNALED|IBV_SEND_INLINE));

	resp = &send_future.get()->lockresp;
	held = true;
}

void RPCQueueLock::unlock() noexcept
{
	try {
		struct LockRequest *msg = reinterpret_cast<struct LockRequest *>(
				aligned_alloc(CACHE_LINE_SIZE, sizeof(*msg)));
		auto req_id = ++conn->next_req_id;
		CHECK_PTR_ERRNO(msg);
		msg->hdr.version = 0;
		msg->hdr.opcode = OPCODE_UNLOCK_REQ;
		native_to_big_inplace(msg->hdr.req_id = req_id);
		native_to_big_inplace(msg->hdr.hostid = 0);
		native_to_big_inplace(msg->lock_id = lock_id);
		native_to_big_inplace(msg->lock_key = lock_key);
		held = false;
	} catch (...) {
		/* Can't really do anything about it */
	}
}

RDMAAtomicLock::RDMAAtomicLock(RdmaConnection *conn, uint64_t lock_id, uint32_t lock_key)
	: conn(conn), held(false), lock_id(lock_id), lock_key(lock_key)
{
}

bool RDMAAtomicLock::tryLock()
{
	struct ibv_send_wr wr, *bad_wr;
	struct ibv_sge sge;
	uint64_t target;
	auto req_id = ++conn->next_req_id;
	wr.wr_id = reinterpret_cast<uint64_t>(this);
	wr.next = NULL;
	wr.sg_list = &sge;
	wr.sg_list[0].addr = reinterpret_cast<uint64_t>(&target);
	wr.sg_list[0].length = sizeof(target);
	wr.sg_list[0].lkey = 0;
	wr.num_sge = 1;
	wr.opcode = IBV_WR_ATOMIC_CMP_AND_SWP;
	wr.send_flags = IBV_SEND_SIGNALED|IBV_SEND_INLINE;
	wr.wr.atomic.remote_addr = lock_id;
	wr.wr.atomic.rkey = lock_key;
	native_to_big_inplace(wr.wr.atomic.compare_add = 0);
	native_to_big_inplace(wr.wr.atomic.swap = 1);

	msg_promise send_promise;
	msg_future send_future = send_promise.get_future();
	conn->send_wc_promises.insert(make_pair(req_id, std::move(send_promise)));
	CHECK_ERRNO(ibv_post_send(conn->id->qp, &wr, &bad_wr));
	send_future.get();
	held = target == 0;
}

void RDMAAtomicLock::lock()
{
	do {
		tryLock();
	} while (!held);
}

void RDMAAtomicLock::unlock() noexcept
{
	try {
		struct ibv_send_wr wr, *bad_wr;
		struct ibv_sge sge;
		uint64_t target;
		auto req_id = ++conn->next_req_id;
		wr.wr_id = reinterpret_cast<uintptr_t>(this);
		wr.next = NULL;
		wr.sg_list = &sge;
		wr.sg_list[0].addr = reinterpret_cast<uintptr_t>(&target);
		wr.sg_list[0].length = sizeof(target);
		wr.sg_list[0].lkey = 0;
		wr.num_sge = 1;
		wr.opcode = IBV_WR_ATOMIC_CMP_AND_SWP;
		wr.send_flags = IBV_SEND_SIGNALED|IBV_SEND_INLINE;
		wr.wr.atomic.remote_addr = lock_id;
		wr.wr.atomic.rkey = lock_key;
		native_to_big_inplace(wr.wr.atomic.compare_add = 1);
		native_to_big_inplace(wr.wr.atomic.swap = 0);

		msg_promise send_promise;
		msg_future send_future = send_promise.get_future();
		conn->send_wc_promises.insert(make_pair(req_id, std::move(send_promise)));
		CHECK_ERRNO(ibv_post_send(conn->id->qp, &wr, &bad_wr));
		send_future.get();
		held = false;
	} catch (...) {
		/* Can't really do anything about it */
	}
}

RDMAVOLTLock::RDMAVOLTLock(RdmaConnection *conn, uint64_t lock_id, uint32_t lock_key)
	: conn(conn), held(false), lock_id(lock_id), lock_key(lock_key)
{
}

bool RDMAVOLTLock::tryLock()
{
	throw "not implemented";
}

void RDMAVOLTLock::lock()
{
	auto req_id = ++conn->next_req_id;
	uint64_t target;
	msg_promise send_promise;
	msg_future send_future = send_promise.get_future();
	conn->send_wc_promises.insert(make_pair(req_id, std::move(send_promise)));
	CHECK_ERRNO(urdma_remote_lock(conn->id->qp, &target, lock_id, lock_key, this));
	send_future.get();
	held = true;
}

void RDMAVOLTLock::unlock() noexcept
{
	try {
		auto req_id = ++conn->next_req_id;
		uint64_t target;
		msg_promise send_promise;
		msg_future send_future = send_promise.get_future();
		conn->send_wc_promises.insert(make_pair(req_id, std::move(send_promise)));
		CHECK_ERRNO(urdma_remote_unlock(conn->id->qp, &target, lock_id, lock_key, this));
		send_future.get();
		held = false;
	} catch (...) {
		/* Can't really do anything about it */
	}
}
