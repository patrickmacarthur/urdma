/** \file src/ros/ros.h
 * \author Patrick MacArthur <patrick@patrickmacarthur.net>
 */

#ifndef ROS_H
#define ROS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

const size_t page_size = 4096;

static const char *ros_mcast_addr = "239.255.123.45";
static int ros_mcast_port = 9002;

static const int CACHE_LINE_SIZE = 64;

enum {
	OPCODE_QUERY_SERVERS,
	OPCODE_ANNOUNCE,
	OPCODE_GETHDR_REQ,
	OPCODE_GETHDR_RESP,
	OPCODE_ALLOC_REQ,
	OPCODE_ALLOC_RESP,
	OPCODE_FREE_REQ,
	OPCODE_FREE_RESP,
};

struct MessageHeader {
	uint8_t version;
	uint8_t opcode;
	uint16_t req_id;
	uint32_t hostid;
};

/* only to be used with multicast service */
struct QueryServersMessage {
	struct MessageHeader hdr;
	uint32_t reserved8;
	uint64_t cluster_id;
};

struct AnnounceMessage {
	struct MessageHeader hdr;
	uint32_t rdma_ipv4_addr;
	uint64_t cluster_id;
	uint32_t pool_rkey;
	uint32_t reserved28;
};

struct GetHdrRequest {
	struct MessageHeader hdr;
	uint64_t uid;
};

struct GetHdrResponse {
	struct MessageHeader hdr;
	uint64_t uid;
	uint32_t replica_hostid1;
	uint32_t replica_hostid2;
	uint64_t addr;
	uint32_t rkey;
	uint32_t reserved36;
};

struct AllocRequest {
	struct MessageHeader hdr;
	uint64_t uid;
};

struct AllocResponse {
	struct MessageHeader hdr;
	uint32_t status;
	uint32_t reserved12;
	uint64_t uid;
	uint32_t replica_hostid1;
	uint32_t replica_hostid2;
	uint64_t addr;
};

union MessageBuf {
	struct MessageHeader hdr;
	struct QueryServersMessage qsmsg;
	struct AnnounceMessage announce;
	struct GetHdrRequest gethdrreq;
	struct GetHdrResponse gethdrresp;
	struct AllocRequest allocreq;
	struct AllocResponse allocresp;
	char buf[40];
};

#define CHECK_ERRNO(x) \
	if ((x) < 0) { \
		throw std::system_error(errno, std::system_category()); \
	}

#define CHECK_PTR_ERRNO(x) \
	if (x == nullptr) { \
		throw std::system_error(errno, std::system_category()); \
	}

#ifdef __cplusplus
}
#endif

#endif
