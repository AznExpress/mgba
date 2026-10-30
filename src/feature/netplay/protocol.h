/* Copyright (c) 2026 mgba-netplay contributors
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#ifndef M_FEATURE_NETPLAY_PROTOCOL_H
#define M_FEATURE_NETPLAY_PROTOCOL_H

#include <mgba/feature/netplay.h>

// Wire format: see docs/protocol.md. All integers are little-endian.
#define NETPLAY_MAGIC 0x504E4247
#define NETPLAY_HEADER_SIZE 44
#define NETPLAY_MAX_INPUTS_PER_PACKET 64
#define NETPLAY_BLOB_CHUNK_HEADER 22
#define NETPLAY_BLOB_ACK_BITMAP 32
#define NETPLAY_CHUNKS_PER_FLUSH 24
#define NETPLAY_BLOB_ACK_INTERVAL_MS 40
#define NETPLAY_HELLO_INTERVAL_MS 100

struct NetplayPacketHeader {
	uint8_t version;
	uint8_t type;
	uint8_t sender;
	uint8_t nPlayers;
	uint64_t token;
	uint32_t sendTs;
	uint32_t echoTs[NETPLAY_MAX_PLAYERS];
	uint16_t echoHold[NETPLAY_MAX_PLAYERS];
};

static inline void NetplayPut16(uint8_t* p, uint16_t v) {
	p[0] = v;
	p[1] = v >> 8;
}

static inline void NetplayPut32(uint8_t* p, uint32_t v) {
	p[0] = v;
	p[1] = v >> 8;
	p[2] = v >> 16;
	p[3] = v >> 24;
}

static inline void NetplayPut64(uint8_t* p, uint64_t v) {
	NetplayPut32(p, (uint32_t) v);
	NetplayPut32(p + 4, (uint32_t) (v >> 32));
}

static inline uint16_t NetplayGet16(const uint8_t* p) {
	return p[0] | (p[1] << 8);
}

static inline uint32_t NetplayGet32(const uint8_t* p) {
	return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t) p[3] << 24);
}

static inline uint64_t NetplayGet64(const uint8_t* p) {
	return NetplayGet32(p) | ((uint64_t) NetplayGet32(p + 4) << 32);
}

size_t NetplayClientWriteHeader(struct NetplayClient*, uint8_t* buffer, enum NetplayPacketType, uint64_t nowMs);
bool NetplayClientSendPacket(struct NetplayClient*, const uint8_t* buffer, size_t size);

void NetplaySyncFlush(struct NetplayClient*, uint64_t nowMs);
void NetplaySyncHandleChunk(struct NetplayClient*, int sender, const uint8_t* payload, size_t size, uint64_t nowMs);
void NetplaySyncHandleAck(struct NetplayClient*, int sender, const uint8_t* payload, size_t size);
void NetplaySyncHandleHash(struct NetplayClient*, int sender, const uint8_t* payload, size_t size);
void NetplaySyncPoll(struct NetplayClient*);
void NetplaySyncDeinit(struct NetplayClient*);

#endif
