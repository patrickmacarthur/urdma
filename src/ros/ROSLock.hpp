/* \file src/ros/ROSLock.cc
 * \author Patrick MacArthur <patrick@patrickmacarthur.net>
 */

#include "ros.h"

#include <future>
#include <thread>

class ROSLock {
public:
	virtual ~ROSLock() = default;
	virtual void lock() = 0;
	virtual bool tryLock() = 0;
	virtual void unlock() = 0;
};

class RPCPollLock : public ROSLock {
public:
	using lock_id_type = uint64_t;
	using lock_key_type = uint32_t;

	RPCPollLock(struct ClientConnState *conn, uint64_t lock_id, uint32_t lock_key);
	virtual void lock();
	virtual bool tryLock();
	virtual void unlock();

private:
	struct ClientConnState *conn;
	lock_id_type lock_id;
	lock_key_type lock_key;
	bool held;
};

class RPCQueueLock : public ROSLock {
public:
	using lock_id_type = uint64_t;
	using lock_key_type = uint32_t;

	RPCQueueLock(struct ClientConnState *conn, uint64_t lock_id, uint32_t lock_key);
	virtual void lock();
	virtual bool tryLock();
	virtual void unlock();

private:
	struct ClientConnState *conn;
	lock_id_type lock_id;
	lock_key_type lock_key;
	bool held;
};

class RDMAAtomicLock : public ROSLock {
public:
	using lock_id_type = uint64_t;
	using lock_key_type = uint32_t;

	RDMAAtomicLock(struct ClientConnState *conn, uint64_t lock_id, uint32_t lock_key);
	virtual void lock();
	virtual bool tryLock();
	virtual void unlock();

private:
	struct ClientConnState *conn;
	lock_id_type lock_id;
	lock_key_type lock_key;
	bool held;
};

class RDMAVOLTLock : public ROSLock {
public:
	using lock_id_type = uint64_t;
	using lock_key_type = uint32_t;

	RDMAVOLTLock(struct ClientConnState *conn, uint64_t lock_id, uint32_t lock_key);
	virtual void lock();
	virtual bool tryLock();
	virtual void unlock();

private:
	struct ClientConnState *conn;
	lock_id_type lock_id;
	lock_key_type lock_key;
	bool held;
};
