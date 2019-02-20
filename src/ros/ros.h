/** \file src/ros/ros.h
 * \author Patrick MacArthur <patrick@patrickmacarthur.net>
 */

#ifndef ROS_H
#define ROS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
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

struct AnnounceMessage {
	struct MessageHeader hdr;
	uint64_t root_uid;
	uint64_t root_addr;
	uint32_t root_rkey;
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

union MessageBuf {
	struct MessageHeader hdr;
	struct AnnounceMessage announce;
	struct GetHdrRequest gethdrreq;
	struct GetHdrResponse gethdrresp;
	char buf[40];
};

#ifdef __cplusplus
}
#endif

#endif
