/* Copyright (c) 2026 mgba-netplay contributors
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#ifndef M_FEATURE_NETPLAY_H
#define M_FEATURE_NETPLAY_H

#include <mgba-util/common.h>

CXX_GUARD_START

#include <mgba/core/log.h>
#include <mgba/core/timing.h>
#include <mgba/gba/interface.h>

mLOG_DECLARE_CATEGORY(NETPLAY);

#define NETPLAY_MAX_PLAYERS 4
#define NETPLAY_FRAME_CYCLES 280896
#define NETPLAY_DEFAULT_SLICE_CYCLES 1024
#define NETPLAY_MAX_ROLLBACK 8
#define NETPLAY_MAX_INPUT_DELAY 6
// Must exceed NETPLAY_MAX_ROLLBACK so the state at the start of every unconfirmed frame is still available
#define NETPLAY_STATE_RING 16
#define NETPLAY_INPUT_RING 1024
#define NETPLAY_HASH_INTERVAL 60
#define NETPLAY_MAX_SAVEDATA 0x20000
#define NETPLAY_NO_PLAYER 0xFF

struct mCore;
struct NetplayLink;

/* ---- Deterministic link cable ----
 * All players' cores run in one thread, stepped round-robin in fixed cycle slices.
 * Every cross-core interaction happens at a point that depends only on emulated
 * state, so two machines given the same inputs produce bit-identical results. */

struct NetplayLinkDriver {
	struct GBASIODriver d;
	struct NetplayLink* link;
	int player;
};

struct NetplayLinkStop {
	struct mTimingEvent event;
	bool stopped;
};

// Plain data, copied verbatim into snapshots; keep every field fixed-width
struct NetplayLinkState {
	uint64_t time[NETPLAY_MAX_PLAYERS];
	uint64_t transferStart;
	uint64_t transferEnd;
	uint32_t lastRaw[NETPLAY_MAX_PLAYERS];
	uint32_t normalData[NETPLAY_MAX_PLAYERS];
	uint32_t frame;
	int32_t transferMode;
	uint16_t multiData[NETPLAY_MAX_PLAYERS];
	uint8_t started[NETPLAY_MAX_PLAYERS];
	uint8_t finished[NETPLAY_MAX_PLAYERS];
	uint8_t ready[NETPLAY_MAX_PLAYERS];
	uint8_t transferActive;
	uint8_t initiator;
	uint8_t pendingStart;
	uint8_t reserved;
};

struct NetplayLink {
	int nPlayers;
	int running;
	bool restoring;
	uint32_t sliceCycles;
	size_t coreStateSize;
	struct mCore* cores[NETPLAY_MAX_PLAYERS];
	struct NetplayLinkDriver drivers[NETPLAY_MAX_PLAYERS];
	struct NetplayLinkStop stops[NETPLAY_MAX_PLAYERS];
	struct NetplayLinkState state;
	// Informational only; not part of snapshots
	uint64_t transfersStarted;
};

// Applies the settings every netplay core needs to stay deterministic. Call before loading the ROM.
MGBA_EXPORT void NetplayPrepareCore(struct mCore* core, int64_t rtcStartMs);

MGBA_EXPORT void NetplayLinkInit(struct NetplayLink*, struct mCore* const* cores, int nPlayers);
MGBA_EXPORT void NetplayLinkDeinit(struct NetplayLink*);
MGBA_EXPORT void NetplayLinkRunFrame(struct NetplayLink*, const uint16_t* keys);
MGBA_EXPORT size_t NetplayLinkSnapshotCapacity(const struct NetplayLink*);
MGBA_EXPORT size_t NetplayLinkSnapshot(struct NetplayLink*, void* out);
MGBA_EXPORT bool NetplayLinkRestore(struct NetplayLink*, const void* in, size_t size);
MGBA_EXPORT uint32_t NetplaySnapshotFrame(const void* snapshot, size_t size);
MGBA_EXPORT uint32_t NetplaySnapshotHash(const void* snapshot, size_t size);

/* ---- Rollback session ---- */

struct NetplaySessionCallbacks {
	void* context;
	void (*frameStarted)(void* context, uint32_t frame, bool resimulating);
	void (*frameFinished)(void* context, uint32_t frame, bool resimulating);
	// Called once per NETPLAY_HASH_INTERVAL frames for the start-of-frame state once all of its inputs are confirmed
	void (*confirmedState)(void* context, uint32_t frame, uint32_t hash, const void* snapshot, size_t size);
};

struct NetplaySessionStats {
	uint64_t framesRun;
	uint64_t rollbacks;
	uint64_t framesResimulated;
	uint32_t lastRollbackDepth;
	uint32_t maxRollbackDepth;
};

struct NetplaySession {
	struct NetplayLink* link;
	int nPlayers;
	int localPlayer;
	int inputDelay;
	uint32_t frame;
	uint32_t localNext;
	int64_t rollbackFrom;
	int64_t lastHashed;
	int64_t confirmedUntil[NETPLAY_MAX_PLAYERS];
	uint16_t inputs[NETPLAY_INPUT_RING][NETPLAY_MAX_PLAYERS];
	uint32_t inputTags[NETPLAY_INPUT_RING][NETPLAY_MAX_PLAYERS];
	uint16_t used[NETPLAY_INPUT_RING][NETPLAY_MAX_PLAYERS];
	size_t snapshotCapacity;
	void* states[NETPLAY_STATE_RING];
	size_t stateSizes[NETPLAY_STATE_RING];
	int64_t stateFrames[NETPLAY_STATE_RING];
	struct NetplaySessionCallbacks callbacks;
	struct NetplaySessionStats stats;
};

MGBA_EXPORT bool NetplaySessionInit(struct NetplaySession*, struct NetplayLink*, int localPlayer, int inputDelay);
MGBA_EXPORT void NetplaySessionDeinit(struct NetplaySession*);
MGBA_EXPORT bool NetplaySessionWantsLocalInput(const struct NetplaySession*);
MGBA_EXPORT uint32_t NetplaySessionAddLocalInput(struct NetplaySession*, uint16_t keys);
MGBA_EXPORT bool NetplaySessionAddRemoteInput(struct NetplaySession*, int player, uint32_t frame, uint16_t keys);
MGBA_EXPORT bool NetplaySessionGetInput(const struct NetplaySession*, int player, uint32_t frame, uint16_t* keys);
MGBA_EXPORT int64_t NetplaySessionConfirmedFrame(const struct NetplaySession*);
MGBA_EXPORT int64_t NetplaySessionPlayerConfirmed(const struct NetplaySession*, int player);
MGBA_EXPORT bool NetplaySessionCanAdvance(const struct NetplaySession*);
MGBA_EXPORT int NetplaySessionAdvance(struct NetplaySession*);
MGBA_EXPORT bool NetplaySessionLoadSnapshot(struct NetplaySession*, const void* snapshot, size_t size);

/* ---- Network protocol ---- */

#define NETPLAY_PROTOCOL_VERSION 1
#define NETPLAY_PACKET_MAX 1200
#define NETPLAY_CHUNK_SIZE 1024
#define NETPLAY_PEER_TIMEOUT_MS 10000

enum NetplayPacketType {
	NETPLAY_PKT_HELLO = 1,
	NETPLAY_PKT_INPUT = 2,
	NETPLAY_PKT_HASH = 3,
	NETPLAY_PKT_BLOB_CHUNK = 4,
	NETPLAY_PKT_BLOB_ACK = 5,
	NETPLAY_PKT_BYE = 6,
};

enum NetplayBlobKind {
	NETPLAY_BLOB_SAVE = 1,
	NETPLAY_BLOB_STATE = 2,
};

// Datagram transport: send broadcasts to every peer (directly, or via the relay)
struct NetplayTransport {
	void* context;
	bool (*send)(struct NetplayTransport*, const void* data, size_t size);
	int (*recv)(struct NetplayTransport*, void* buffer, size_t size);
	void (*deinit)(struct NetplayTransport*);
};

MGBA_EXPORT bool NetplayUDPTransportCreate(struct NetplayTransport*, uint16_t localPort, int nHosts, const char* const* hosts, const uint16_t* ports);
MGBA_EXPORT uint64_t NetplayNowMs(void);

struct NetplayBlobTx {
	bool active;
	uint16_t id;
	uint8_t kind;
	uint8_t targets;
	uint32_t frame;
	uint32_t rawSize;
	uint8_t* data;
	uint32_t size;
	uint32_t nChunks;
	uint32_t cursor;
	uint8_t* acked[NETPLAY_MAX_PLAYERS];
	uint32_t ackedCount[NETPLAY_MAX_PLAYERS];
};

struct NetplayBlobRx {
	bool active;
	bool complete;
	bool delivered;
	uint16_t id;
	uint8_t kind;
	uint32_t frame;
	uint32_t rawSize;
	uint8_t* data;
	uint32_t size;
	uint32_t nChunks;
	uint32_t received;
	uint8_t* have;
	uint64_t lastAckMs;
};

struct NetplayPeer {
	bool seen;
	bool gone;
	uint32_t romCrc;
	int64_t remoteFrame;
	int64_t ackedLocal;
	uint32_t echoTs;
	uint64_t echoRecvMs;
	uint64_t lastHeardMs;
	uint32_t rttMs;
	struct NetplayBlobRx blob;
};

struct NetplayHashRecord {
	uint32_t frame;
	uint32_t hash;
	bool valid;
};

struct NetplayClientStats {
	uint64_t packetsSent;
	uint64_t packetsReceived;
	uint64_t packetsRejected;
	uint32_t desyncs;
	uint32_t resyncs;
	uint64_t timeSyncStalls;
};

struct NetplayClient {
	struct NetplayTransport* transport;
	struct NetplaySession* session;
	uint64_t token;
	int localPlayer;
	int nPlayers;
	uint32_t romCrc;
	uint64_t startMs;
	uint64_t lastSendMs;
	uint16_t nextBlobId;
	struct NetplayPeer peers[NETPLAY_MAX_PLAYERS];
	struct NetplayBlobTx tx;
	uint64_t nowMs;
	struct NetplayHashRecord localHashes[16];
	struct NetplayHashRecord peerHashes[NETPLAY_MAX_PLAYERS][16];
	void* resyncSnapshot;
	size_t resyncSize;
	uint32_t resyncFrame;
	bool desynced;
	bool awaitingResync;
	uint32_t desyncFrame;
	int stallCooldown;
	struct NetplayClientStats stats;
};

MGBA_EXPORT void NetplayClientInit(struct NetplayClient*, struct NetplayTransport*, uint64_t token, int localPlayer, int nPlayers, uint32_t romCrc);
MGBA_EXPORT void NetplayClientDeinit(struct NetplayClient*);
MGBA_EXPORT void NetplayClientAttachSession(struct NetplayClient*, struct NetplaySession*);
MGBA_EXPORT void NetplayClientPoll(struct NetplayClient*, uint64_t nowMs);
MGBA_EXPORT void NetplayClientFlush(struct NetplayClient*, uint64_t nowMs);
MGBA_EXPORT void NetplayClientSendBye(struct NetplayClient*, uint64_t nowMs);
MGBA_EXPORT bool NetplayClientAllPeersSeen(const struct NetplayClient*);
MGBA_EXPORT bool NetplayClientRomMismatch(const struct NetplayClient*, int* player);
MGBA_EXPORT bool NetplayClientShareBlob(struct NetplayClient*, enum NetplayBlobKind, uint8_t targets, uint32_t frame, const void* data, size_t size);
MGBA_EXPORT bool NetplayClientBlobSent(const struct NetplayClient*);
MGBA_EXPORT bool NetplayClientTakeBlob(struct NetplayClient*, int player, enum NetplayBlobKind, void** data, size_t* size, uint32_t* frame);
MGBA_EXPORT float NetplayClientFrameAdvantage(const struct NetplayClient*, uint64_t nowMs);
MGBA_EXPORT bool NetplayClientShouldStall(struct NetplayClient*, uint64_t nowMs);
MGBA_EXPORT uint32_t NetplayClientMaxRtt(const struct NetplayClient*);
MGBA_EXPORT void NetplayClientOnConfirmedState(struct NetplayClient*, uint32_t frame, uint32_t hash, const void* snapshot, size_t size);

CXX_GUARD_END

#endif
