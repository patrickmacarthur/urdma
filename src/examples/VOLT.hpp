/* \file src/ros/ROSLock.cc
 * \author Patrick MacArthur <patrick@patrickmacarthur.net>
 */

#include "ros.h"

#include <future>
#include <thread>

namespace volt {

enum {
	OPCODE_LOCK_POLL_REQ,
	OPCODE_LOCK_QUEUE_REQ,
	OPCODE_UNLOCK_REQ,
	OPCODE_LOCK_RESP,
};

struct MessageHeader {
	uint8_t version;
	uint8_t opcode;
	uint16_t req_id;
	uint32_t hostid;
};

struct LockRequest {
	struct MessageHeader hdr;
	uint64_t lock_id;
	uint32_t lock_key;
};

struct LockResponse {
	struct MessageHeader hdr;
	uint64_t lock_id;
	uint32_t status;
};

union MessageBuf {
	struct LockRequest lockreq;
	struct LockResponse lockresp;
};

class AbstractLock {
public:
	virtual ~ROSLock() = default;
	virtual void lock() = 0;
	virtual bool tryLock() = 0;
	virtual void unlock() nothrow = 0;
};

class RPCPollLock : public AbstractLock {
public:
	using lock_id_type = uint64_t;
	using lock_key_type = uint32_t;

	RPCPollLock(struct ClientConnState *conn, uint64_t lock_id, uint32_t lock_key);
	virtual void lock();
	virtual bool tryLock();
	virtual void unlock() nothrow;

private:
	struct ClientConnState *conn;
	lock_id_type lock_id;
	lock_key_type lock_key;
	bool held;
};

class RPCQueueLock : public AbstractLock {
public:
	using lock_id_type = uint64_t;
	using lock_key_type = uint32_t;

	RPCQueueLock(struct ClientConnState *conn, uint64_t lock_id, uint32_t lock_key);
	virtual void lock();
	virtual bool tryLock();
	virtual void unlock() nothrow;

private:
	struct ClientConnState *conn;
	lock_id_type lock_id;
	lock_key_type lock_key;
	bool held;
};

class RDMAAtomicLock : public AbstractLock {
public:
	using lock_id_type = uint64_t;
	using lock_key_type = uint32_t;

	RDMAAtomicLock(struct ClientConnState *conn, uint64_t lock_id, uint32_t lock_key);
	virtual void lock();
	virtual bool tryLock();
	virtual void unlock() nothrow;

private:
	struct ClientConnState *conn;
	lock_id_type lock_id;
	lock_key_type lock_key;
	bool held;
};

class RDMAVOLTLock : public AbstractLock {
public:
	using lock_id_type = uint64_t;
	using lock_key_type = uint32_t;

	RDMAVOLTLock(struct ClientConnState *conn, uint64_t lock_id, uint32_t lock_key);
	virtual void lock();
	virtual bool tryLock();
	virtual void unlock() nothrow;

private:
	struct ClientConnState *conn;
	lock_id_type lock_id;
	lock_key_type lock_key;
	bool held;
};

}
