/* Copyright (c) 2026 mgba-netplay contributors
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "protocol.h"

#include <zlib.h>

#define HASH_RECORDS (sizeof(((struct NetplayClient*) 0)->localHashes) / sizeof(struct NetplayHashRecord))

static void _freeTx(struct NetplayBlobTx* tx) {
	free(tx->data);
	int p;
	for (p = 0; p < NETPLAY_MAX_PLAYERS; ++p) {
		free(tx->acked[p]);
	}
	memset(tx, 0, sizeof(*tx));
}

static void _freeRx(struct NetplayBlobRx* rx) {
	free(rx->data);
	free(rx->have);
	memset(rx, 0, sizeof(*rx));
}

void NetplaySyncDeinit(struct NetplayClient* client) {
	_freeTx(&client->tx);
	int p;
	for (p = 0; p < NETPLAY_MAX_PLAYERS; ++p) {
		_freeRx(&client->peers[p].blob);
	}
	free(client->resyncSnapshot);
	client->resyncSnapshot = NULL;
	client->resyncSize = 0;
}

bool NetplayClientShareBlob(struct NetplayClient* client, enum NetplayBlobKind kind, uint8_t targets, uint32_t frame, const void* data, size_t size) {
	struct NetplayBlobTx* tx = &client->tx;
	_freeTx(tx);
	targets &= ((1 << client->nPlayers) - 1) & ~(1 << client->localPlayer);

	uLongf compressedSize = compressBound(size);
	uint8_t* compressed = malloc(compressedSize ? compressedSize : 1);
	if (!compressed) {
		return false;
	}
	if (compress2(compressed, &compressedSize, data, size, 6) != Z_OK) {
		free(compressed);
		return false;
	}
	tx->active = true;
	tx->id = client->nextBlobId++;
	tx->kind = kind;
	tx->targets = targets;
	tx->frame = frame;
	tx->rawSize = size;
	tx->data = compressed;
	tx->size = compressedSize;
	tx->nChunks = (compressedSize + NETPLAY_CHUNK_SIZE - 1) / NETPLAY_CHUNK_SIZE;
	if (!tx->nChunks) {
		tx->nChunks = 1;
	}
	int p;
	for (p = 0; p < client->nPlayers; ++p) {
		if (targets & (1 << p)) {
			tx->acked[p] = calloc(tx->nChunks, 1);
		}
	}
	return true;
}

bool NetplayClientBlobSent(const struct NetplayClient* client) {
	const struct NetplayBlobTx* tx = &client->tx;
	if (!tx->active) {
		return true;
	}
	int p;
	for (p = 0; p < client->nPlayers; ++p) {
		if ((tx->targets & (1 << p)) && tx->ackedCount[p] < tx->nChunks) {
			return false;
		}
	}
	return true;
}

static bool _chunkNeeded(const struct NetplayClient* client, uint32_t chunk) {
	const struct NetplayBlobTx* tx = &client->tx;
	int p;
	for (p = 0; p < client->nPlayers; ++p) {
		if ((tx->targets & (1 << p)) && !client->peers[p].gone && !tx->acked[p][chunk]) {
			return true;
		}
	}
	return false;
}

static void _sendChunk(struct NetplayClient* client, uint32_t chunk, uint64_t nowMs) {
	const struct NetplayBlobTx* tx = &client->tx;
	uint8_t buffer[NETPLAY_HEADER_SIZE + NETPLAY_BLOB_CHUNK_HEADER + NETPLAY_CHUNK_SIZE];
	size_t size = NetplayClientWriteHeader(client, buffer, NETPLAY_PKT_BLOB_CHUNK, nowMs);
	uint32_t offset = chunk * NETPLAY_CHUNK_SIZE;
	uint32_t length = tx->size - offset;
	if (length > NETPLAY_CHUNK_SIZE) {
		length = NETPLAY_CHUNK_SIZE;
	}
	uint8_t* payload = &buffer[size];
	NetplayPut16(&payload[0], tx->id);
	payload[2] = tx->kind;
	payload[3] = tx->targets;
	NetplayPut32(&payload[4], tx->frame);
	NetplayPut32(&payload[8], tx->size);
	NetplayPut32(&payload[12], tx->rawSize);
	NetplayPut32(&payload[16], chunk);
	NetplayPut16(&payload[20], length);
	memcpy(&payload[NETPLAY_BLOB_CHUNK_HEADER], &tx->data[offset], length);
	NetplayClientSendPacket(client, buffer, size + NETPLAY_BLOB_CHUNK_HEADER + length);
}

static void _sendAck(struct NetplayClient* client, int sender, uint64_t nowMs) {
	struct NetplayBlobRx* rx = &client->peers[sender].blob;
	uint8_t buffer[NETPLAY_HEADER_SIZE + 8 + NETPLAY_BLOB_ACK_BITMAP];
	size_t size = NetplayClientWriteHeader(client, buffer, NETPLAY_PKT_BLOB_ACK, nowMs);
	uint8_t* payload = &buffer[size];
	uint32_t firstMissing = 0;
	if (rx->complete) {
		firstMissing = rx->nChunks;
	} else {
		while (firstMissing < rx->nChunks && rx->have[firstMissing]) {
			++firstMissing;
		}
	}
	payload[0] = sender;
	payload[1] = 0;
	NetplayPut16(&payload[2], rx->id);
	NetplayPut32(&payload[4], firstMissing);
	memset(&payload[8], 0, NETPLAY_BLOB_ACK_BITMAP);
	if (!rx->complete) {
		uint32_t i;
		for (i = 0; i < NETPLAY_BLOB_ACK_BITMAP * 8 && firstMissing + i < rx->nChunks; ++i) {
			if (rx->have[firstMissing + i]) {
				payload[8 + i / 8] |= 1 << (i % 8);
			}
		}
	}
	rx->lastAckMs = nowMs;
	NetplayClientSendPacket(client, buffer, size + 8 + NETPLAY_BLOB_ACK_BITMAP);
}

void NetplaySyncFlush(struct NetplayClient* client, uint64_t nowMs) {
	struct NetplayBlobTx* tx = &client->tx;
	if (tx->active && !NetplayClientBlobSent(client)) {
		unsigned sent = 0;
		uint32_t scanned;
		for (scanned = 0; scanned < tx->nChunks && sent < NETPLAY_CHUNKS_PER_FLUSH; ++scanned) {
			uint32_t chunk = tx->cursor;
			tx->cursor = (tx->cursor + 1) % tx->nChunks;
			if (_chunkNeeded(client, chunk)) {
				_sendChunk(client, chunk, nowMs);
				++sent;
			}
		}
	}

	int p;
	for (p = 0; p < client->nPlayers; ++p) {
		struct NetplayBlobRx* rx = &client->peers[p].blob;
		if (rx->active && !rx->complete && nowMs - rx->lastAckMs >= NETPLAY_BLOB_ACK_INTERVAL_MS) {
			_sendAck(client, p, nowMs);
		}
	}
}

void NetplaySyncHandleChunk(struct NetplayClient* client, int sender, const uint8_t* payload, size_t size, uint64_t nowMs) {
	if (size < NETPLAY_BLOB_CHUNK_HEADER) {
		return;
	}
	uint16_t id = NetplayGet16(&payload[0]);
	uint8_t kind = payload[2];
	uint8_t targets = payload[3];
	uint32_t frame = NetplayGet32(&payload[4]);
	uint32_t total = NetplayGet32(&payload[8]);
	uint32_t rawSize = NetplayGet32(&payload[12]);
	uint32_t chunk = NetplayGet32(&payload[16]);
	uint16_t length = NetplayGet16(&payload[20]);
	if (!(targets & (1 << client->localPlayer)) || size < NETPLAY_BLOB_CHUNK_HEADER + (size_t) length) {
		return;
	}
	if (total > 64 * 1024 * 1024 || rawSize > 64 * 1024 * 1024) {
		return;
	}

	struct NetplayBlobRx* rx = &client->peers[sender].blob;
	if (!rx->active || rx->id != id) {
		if (rx->active && (int16_t) (id - rx->id) < 0) {
			return;
		}
		_freeRx(rx);
		rx->active = true;
		rx->id = id;
		rx->kind = kind;
		rx->frame = frame;
		rx->rawSize = rawSize;
		rx->size = total;
		rx->nChunks = (total + NETPLAY_CHUNK_SIZE - 1) / NETPLAY_CHUNK_SIZE;
		if (!rx->nChunks) {
			rx->nChunks = 1;
		}
		rx->data = malloc(total ? total : 1);
		rx->have = calloc(rx->nChunks, 1);
		if (!rx->data || !rx->have) {
			_freeRx(rx);
			return;
		}
	}

	if (!rx->complete && chunk < rx->nChunks && !rx->have[chunk]) {
		uint32_t offset = chunk * NETPLAY_CHUNK_SIZE;
		if (offset + length > rx->size) {
			return;
		}
		memcpy(&rx->data[offset], &payload[NETPLAY_BLOB_CHUNK_HEADER], length);
		rx->have[chunk] = 1;
		++rx->received;
		if (rx->received == rx->nChunks) {
			uint8_t* raw = malloc(rx->rawSize ? rx->rawSize : 1);
			uLongf rawSize = rx->rawSize;
			if (!raw || uncompress(raw, &rawSize, rx->data, rx->size) != Z_OK || rawSize != rx->rawSize) {
				mLOG(NETPLAY, ERROR, "Corrupt blob %u from player %i", id, sender);
				free(raw);
				_freeRx(rx);
				return;
			}
			free(rx->data);
			rx->data = raw;
			rx->size = rx->rawSize;
			rx->complete = true;
			_sendAck(client, sender, nowMs);
			return;
		}
	}
	if (rx->complete || nowMs - rx->lastAckMs >= NETPLAY_BLOB_ACK_INTERVAL_MS) {
		_sendAck(client, sender, nowMs);
	}
}

void NetplaySyncHandleAck(struct NetplayClient* client, int sender, const uint8_t* payload, size_t size) {
	struct NetplayBlobTx* tx = &client->tx;
	if (size < 8 + NETPLAY_BLOB_ACK_BITMAP || payload[0] != client->localPlayer) {
		return;
	}
	if (!tx->active || NetplayGet16(&payload[2]) != tx->id || !tx->acked[sender]) {
		return;
	}
	uint32_t firstMissing = NetplayGet32(&payload[4]);
	if (firstMissing > tx->nChunks) {
		firstMissing = tx->nChunks;
	}
	uint8_t* acked = tx->acked[sender];
	uint32_t i;
	for (i = 0; i < firstMissing; ++i) {
		acked[i] = 1;
	}
	for (i = 0; i < NETPLAY_BLOB_ACK_BITMAP * 8 && firstMissing + i < tx->nChunks; ++i) {
		if (payload[8 + i / 8] & (1 << (i % 8))) {
			acked[firstMissing + i] = 1;
		}
	}
	uint32_t count = 0;
	for (i = 0; i < tx->nChunks; ++i) {
		count += acked[i];
	}
	tx->ackedCount[sender] = count;
}

bool NetplayClientTakeBlob(struct NetplayClient* client, int player, enum NetplayBlobKind kind, void** data, size_t* size, uint32_t* frame) {
	struct NetplayBlobRx* rx = &client->peers[player].blob;
	if (!rx->active || !rx->complete || rx->delivered || rx->kind != kind) {
		return false;
	}
	*data = rx->data;
	*size = rx->size;
	if (frame) {
		*frame = rx->frame;
	}
	rx->data = NULL;
	rx->delivered = true;
	return true;
}

static struct NetplayHashRecord* _record(struct NetplayHashRecord* records, uint32_t frame) {
	return &records[(frame / NETPLAY_HASH_INTERVAL) % HASH_RECORDS];
}

static void _sendResync(struct NetplayClient* client, int target);

// Player 0 is the authority: it compares against every peer, everyone else compares against player 0
static void _compare(struct NetplayClient* client, int peer, uint32_t frame) {
	const struct NetplayHashRecord* local = _record(client->localHashes, frame);
	const struct NetplayHashRecord* remote = _record(client->peerHashes[peer], frame);
	if (!local->valid || !remote->valid || local->frame != frame || remote->frame != frame) {
		return;
	}
	if (client->localPlayer == 0) {
		if (local->hash != remote->hash) {
			mLOG(NETPLAY, WARN, "Player %i desynced at frame %u (host %08X, peer %08X)", peer, frame, local->hash, remote->hash);
			++client->stats.desyncs;
			_sendResync(client, peer);
		}
		return;
	}
	if (local->hash != remote->hash) {
		if (!client->desynced) {
			mLOG(NETPLAY, WARN, "Desync detected at frame %u (local %08X, host %08X)", frame, local->hash, remote->hash);
			++client->stats.desyncs;
		}
		client->desynced = true;
		client->awaitingResync = true;
		client->desyncFrame = frame;
	} else if (!client->awaitingResync) {
		client->desynced = false;
	}
}

static void _sendResync(struct NetplayClient* client, int target) {
	if (!client->resyncSnapshot) {
		return;
	}
	if (client->tx.active && !NetplayClientBlobSent(client)) {
		if (client->tx.kind == NETPLAY_BLOB_STATE) {
			return;
		}
	}
	mLOG(NETPLAY, INFO, "Player %i desynced; sending snapshot of frame %u", target, client->resyncFrame);
	NetplayClientShareBlob(client, NETPLAY_BLOB_STATE, 1 << target, client->resyncFrame, client->resyncSnapshot, client->resyncSize);
}

void NetplaySyncHandleHash(struct NetplayClient* client, int sender, const uint8_t* payload, size_t size) {
	if (size < 8) {
		return;
	}
	uint32_t frame = NetplayGet32(&payload[0]);
	uint32_t hash = NetplayGet32(&payload[4]);
	struct NetplayHashRecord* remote = _record(client->peerHashes[sender], frame);
	remote->frame = frame;
	remote->hash = hash;
	remote->valid = true;
	if (client->localPlayer == 0 || sender == 0) {
		_compare(client, sender, frame);
	}
}

void NetplayClientOnConfirmedState(struct NetplayClient* client, uint32_t frame, uint32_t hash, const void* snapshot, size_t size) {
	struct NetplayHashRecord* local = _record(client->localHashes, frame);
	local->frame = frame;
	local->hash = hash;
	local->valid = true;

	if (client->localPlayer == 0) {
		if (client->resyncSize < size || !client->resyncSnapshot) {
			free(client->resyncSnapshot);
			client->resyncSnapshot = malloc(size);
		}
		if (client->resyncSnapshot) {
			memcpy(client->resyncSnapshot, snapshot, size);
			client->resyncSize = size;
			client->resyncFrame = frame;
		}
		int p;
		for (p = 1; p < client->nPlayers; ++p) {
			_compare(client, p, frame);
		}
	} else {
		_compare(client, 0, frame);
	}

	uint8_t buffer[NETPLAY_HEADER_SIZE + 8];
	size_t headerSize = NetplayClientWriteHeader(client, buffer, NETPLAY_PKT_HASH, client->nowMs);
	NetplayPut32(&buffer[headerSize], frame);
	NetplayPut32(&buffer[headerSize + 4], hash);
	NetplayClientSendPacket(client, buffer, headerSize + 8);
}

void NetplaySyncPoll(struct NetplayClient* client) {
	if (!client->session || client->localPlayer == 0) {
		return;
	}
	void* data;
	size_t size;
	uint32_t frame;
	if (NetplayClientTakeBlob(client, 0, NETPLAY_BLOB_STATE, &data, &size, &frame)) {
		if (NetplaySessionLoadSnapshot(client->session, data, size)) {
			mLOG(NETPLAY, INFO, "Resynced to host snapshot of frame %u", frame);
			++client->stats.resyncs;
			client->desynced = false;
			client->awaitingResync = false;
		}
		free(data);
	}
}
