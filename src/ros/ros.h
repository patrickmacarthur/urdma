/** \file src/ros/ros.h
 * \author Patrick MacArthur <patrick@patrickmacarthur.net>
 */

#ifndef ROS_H
#define ROS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct RDMALock {
	char lock[8];
};

struct RDMAObjectID {
	uint64_t nodeid;
	uint64_t uid;
};

struct ROSObjectHeader {
	struct RDMALock lock;
	uint64_t uid;
	uint32_t replica_hostid1;
	uint32_t replica_hostid2;
	uint32_t refcnt;
	uint32_t version;
};

struct TreeRoot {
	struct ROSObjectHeader objhdr;
	uint64_t rootnode_uid;
};
struct TreeRoot *root_obj;
struct ibv_mr *root_obj_mr;

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

struct ConnState {
	struct rdma_cm_id *id;
	struct AnnounceMessage *announce;
	struct ibv_mr *announce_mr;
	struct ibv_mr *recv_mr;
	union MessageBuf recv_bufs[32];
};

#ifdef __cplusplus
}
#endif

#endif
