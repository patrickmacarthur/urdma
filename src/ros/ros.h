/** \file src/ros/ros.h
 * \author Patrick MacArthur <patrick@patrickmacarthur.net>
 */

#ifndef ROS_H
#define ROS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

static const char *ros_mcast_addr = "239.255.123.45";
static int ros_mcast_port = 9002;

static const int CACHE_LINE_SIZE = 64;

enum {
	OPCODE_QUERY_SERVERS,
	OPCODE_ANNOUNCE,
	OPCODE_GETHDR_REQ,
	OPCODE_GETHDR_RESP,
};

struct MessageHeader {
	uint8_t version;
	uint8_t opcode;
	uint16_t reserved2;
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
	uint64_t root_uid;
	uint64_t root_addr;
	uint32_t root_rkey;
	uint32_t reserved44;
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

union MessageBuf {
	struct MessageHeader hdr;
	struct QueryServersMessage qsmsg;
	struct AnnounceMessage announce;
	struct GetHdrRequest gethdrreq;
	struct GetHdrResponse gethdrresp;
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
