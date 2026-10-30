/* Copyright (c) 2026 mgba-netplay contributors
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "protocol.h"

#include <mgba-util/socket.h>

#ifndef _WIN32
#include <time.h>
#endif

// 280896 cycles at 16.78 MHz
#define FRAME_MS 16.7427f

uint64_t NetplayNowMs(void) {
#ifdef _WIN32
	return GetTickCount64();
#else
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t) ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
#endif
}

/* ---- UDP transport ---- */

struct NetplayUDP {
	Socket socket;
	int nDest;
	struct sockaddr_storage dest[NETPLAY_MAX_PLAYERS];
	socklen_t destLen[NETPLAY_MAX_PLAYERS];
};

static bool _udpSend(struct NetplayTransport* transport, const void* data, size_t size) {
	struct NetplayUDP* udp = transport->context;
	bool ok = true;
	int i;
	for (i = 0; i < udp->nDest; ++i) {
		if (sendto(udp->socket, (const char*) data, (int) size, 0, (const struct sockaddr*) &udp->dest[i], udp->destLen[i]) < 0) {
			ok = false;
		}
	}
	return ok;
}

static int _udpRecv(struct NetplayTransport* transport, void* buffer, size_t size) {
	struct NetplayUDP* udp = transport->context;
	struct sockaddr_storage from;
	socklen_t fromLen = sizeof(from);
	int received = recvfrom(udp->socket, (char*) buffer, (int) size, 0, (struct sockaddr*) &from, &fromLen);
	if (received < 0) {
		return -1;
	}
	return received;
}

static void _udpDeinit(struct NetplayTransport* transport) {
	struct NetplayUDP* udp = transport->context;
	if (!udp) {
		return;
	}
	SocketCloseQuiet(udp->socket);
	free(udp);
	transport->context = NULL;
	SocketSubsystemDeinit();
}

bool NetplayUDPTransportCreate(struct NetplayTransport* transport, uint16_t localPort, int nHosts, const char* const* hosts, const uint16_t* ports) {
	if (nHosts < 1 || nHosts > NETPLAY_MAX_PLAYERS) {
		return false;
	}
	SocketSubsystemInit();
	struct NetplayUDP* udp = calloc(1, sizeof(*udp));
	int family = AF_UNSPEC;
	int i;
	for (i = 0; i < nHosts; ++i) {
		struct addrinfo hints = {0};
		struct addrinfo* result = NULL;
		char port[8];
		snprintf(port, sizeof(port), "%u", ports[i]);
		hints.ai_family = family;
		hints.ai_socktype = SOCK_DGRAM;
		if (getaddrinfo(hosts[i], port, &hints, &result) != 0 || !result) {
			mLOG(NETPLAY, ERROR, "Could not resolve %s:%u", hosts[i], ports[i]);
			free(udp);
			SocketSubsystemDeinit();
			return false;
		}
		// Prefer IPv4: some relay hosts (Fly.io) publish AAAA records but only route UDP over IPv4
		const struct addrinfo* chosen = result;
		const struct addrinfo* candidate;
		for (candidate = result; candidate; candidate = candidate->ai_next) {
			if (candidate->ai_family == AF_INET) {
				chosen = candidate;
				break;
			}
		}
		family = chosen->ai_family;
		memcpy(&udp->dest[i], chosen->ai_addr, chosen->ai_addrlen);
		udp->destLen[i] = chosen->ai_addrlen;
		freeaddrinfo(result);
	}
	udp->nDest = nHosts;

	udp->socket = socket(family, SOCK_DGRAM, IPPROTO_UDP);
	if (SOCKET_FAILED(udp->socket)) {
		free(udp);
		SocketSubsystemDeinit();
		return false;
	}
	int bound;
	if (family == AF_INET6) {
		struct sockaddr_in6 bindAddr = {0};
		bindAddr.sin6_family = AF_INET6;
		bindAddr.sin6_port = htons(localPort);
		bindAddr.sin6_addr = in6addr_any;
		bound = bind(udp->socket, (const struct sockaddr*) &bindAddr, sizeof(bindAddr));
	} else {
		struct sockaddr_in bindAddr = {0};
		bindAddr.sin_family = AF_INET;
		bindAddr.sin_port = htons(localPort);
		bindAddr.sin_addr.s_addr = htonl(INADDR_ANY);
		bound = bind(udp->socket, (const struct sockaddr*) &bindAddr, sizeof(bindAddr));
	}
	if (bound < 0) {
		mLOG(NETPLAY, ERROR, "Could not bind UDP port %u", localPort);
		SocketCloseQuiet(udp->socket);
		free(udp);
		SocketSubsystemDeinit();
		return false;
	}
	SocketSetBlocking(udp->socket, false);

	transport->context = udp;
	transport->send = _udpSend;
	transport->recv = _udpRecv;
	transport->deinit = _udpDeinit;
	return true;
}

/* ---- Client protocol ---- */

void NetplayClientInit(struct NetplayClient* client, struct NetplayTransport* transport, uint64_t token, int localPlayer, int nPlayers, uint32_t romCrc) {
	memset(client, 0, sizeof(*client));
	client->transport = transport;
	client->token = token;
	client->localPlayer = localPlayer;
	client->nPlayers = nPlayers;
	client->romCrc = romCrc;
	client->startMs = NetplayNowMs();
	client->nextBlobId = 1;
	int p;
	for (p = 0; p < NETPLAY_MAX_PLAYERS; ++p) {
		client->peers[p].remoteFrame = -1;
		client->peers[p].ackedLocal = -1;
	}
}

void NetplayClientDeinit(struct NetplayClient* client) {
	NetplaySyncDeinit(client);
}

void NetplayClientAttachSession(struct NetplayClient* client, struct NetplaySession* session) {
	client->session = session;
}

size_t NetplayClientWriteHeader(struct NetplayClient* client, uint8_t* buffer, enum NetplayPacketType type, uint64_t nowMs) {
	uint32_t now32 = (uint32_t) (nowMs - client->startMs) + 1;
	NetplayPut32(&buffer[0], NETPLAY_MAGIC);
	buffer[4] = NETPLAY_PROTOCOL_VERSION;
	buffer[5] = type;
	buffer[6] = client->localPlayer;
	buffer[7] = client->nPlayers;
	NetplayPut64(&buffer[8], client->token);
	NetplayPut32(&buffer[16], now32);
	int p;
	for (p = 0; p < NETPLAY_MAX_PLAYERS; ++p) {
		const struct NetplayPeer* peer = &client->peers[p];
		uint64_t hold = peer->echoTs ? nowMs - peer->echoRecvMs : 0;
		if (hold > 0xFFFF) {
			hold = 0xFFFF;
		}
		NetplayPut32(&buffer[20 + p * 4], peer->echoTs);
		NetplayPut16(&buffer[36 + p * 2], (uint16_t) hold);
	}
	return NETPLAY_HEADER_SIZE;
}

bool NetplayClientSendPacket(struct NetplayClient* client, const uint8_t* buffer, size_t size) {
	++client->stats.packetsSent;
	return client->transport->send(client->transport, buffer, size);
}

static bool _parseHeader(const struct NetplayClient* client, const uint8_t* buffer, size_t size, struct NetplayPacketHeader* header) {
	if (size < NETPLAY_HEADER_SIZE || NetplayGet32(&buffer[0]) != NETPLAY_MAGIC) {
		return false;
	}
	header->version = buffer[4];
	header->type = buffer[5];
	header->sender = buffer[6];
	header->nPlayers = buffer[7];
	header->token = NetplayGet64(&buffer[8]);
	header->sendTs = NetplayGet32(&buffer[16]);
	int p;
	for (p = 0; p < NETPLAY_MAX_PLAYERS; ++p) {
		header->echoTs[p] = NetplayGet32(&buffer[20 + p * 4]);
		header->echoHold[p] = NetplayGet16(&buffer[36 + p * 2]);
	}
	if (header->version != NETPLAY_PROTOCOL_VERSION || header->token != client->token) {
		return false;
	}
	if (header->sender >= client->nPlayers || header->sender == client->localPlayer || header->nPlayers != client->nPlayers) {
		return false;
	}
	return true;
}

static void _handleInput(struct NetplayClient* client, int sender, const uint8_t* payload, size_t size) {
	if (size < 4 + 4 * NETPLAY_MAX_PLAYERS + 5) {
		return;
	}
	struct NetplayPeer* peer = &client->peers[sender];
	int64_t remoteFrame = NetplayGet32(&payload[0]);
	if (remoteFrame > peer->remoteFrame) {
		peer->remoteFrame = remoteFrame;
	}
	int64_t acked = (int64_t) NetplayGet32(&payload[4 + 4 * client->localPlayer]) - 1;
	if (acked > peer->ackedLocal) {
		peer->ackedLocal = acked;
	}
	size_t offset = 4 + 4 * NETPLAY_MAX_PLAYERS;
	uint32_t start = NetplayGet32(&payload[offset]);
	unsigned count = payload[offset + 4];
	offset += 5;
	if (size < offset + count * 2 || !client->session) {
		return;
	}
	unsigned i;
	for (i = 0; i < count; ++i) {
		NetplaySessionAddRemoteInput(client->session, sender, start + i, NetplayGet16(&payload[offset + i * 2]));
	}
}

void NetplayClientPoll(struct NetplayClient* client, uint64_t nowMs) {
	uint8_t buffer[2048];
	int received;
	client->nowMs = nowMs;
	while ((received = client->transport->recv(client->transport, buffer, sizeof(buffer))) > 0) {
		struct NetplayPacketHeader header;
		if (!_parseHeader(client, buffer, received, &header)) {
			++client->stats.packetsRejected;
			continue;
		}
		++client->stats.packetsReceived;
		int sender = header.sender;
		struct NetplayPeer* peer = &client->peers[sender];
		peer->seen = true;
		peer->lastHeardMs = nowMs;
		peer->echoTs = header.sendTs;
		peer->echoRecvMs = nowMs;
		uint32_t echo = header.echoTs[client->localPlayer];
		if (echo) {
			uint32_t now32 = (uint32_t) (nowMs - client->startMs) + 1;
			int32_t rtt = (int32_t) (now32 - echo) - header.echoHold[client->localPlayer];
			if (rtt >= 0) {
				peer->rttMs = peer->rttMs ? (peer->rttMs * 7 + rtt) / 8 : (uint32_t) rtt;
			}
		}

		const uint8_t* payload = &buffer[NETPLAY_HEADER_SIZE];
		size_t payloadSize = received - NETPLAY_HEADER_SIZE;
		switch (header.type) {
		case NETPLAY_PKT_HELLO:
			if (payloadSize >= 4) {
				peer->romCrc = NetplayGet32(payload);
			}
			break;
		case NETPLAY_PKT_INPUT:
			_handleInput(client, sender, payload, payloadSize);
			break;
		case NETPLAY_PKT_HASH:
			NetplaySyncHandleHash(client, sender, payload, payloadSize);
			break;
		case NETPLAY_PKT_BLOB_CHUNK:
			NetplaySyncHandleChunk(client, sender, payload, payloadSize, nowMs);
			break;
		case NETPLAY_PKT_BLOB_ACK:
			NetplaySyncHandleAck(client, sender, payload, payloadSize);
			break;
		case NETPLAY_PKT_BYE:
			peer->gone = true;
			break;
		default:
			break;
		}
	}
	NetplaySyncPoll(client);
}

static void _sendHello(struct NetplayClient* client, uint64_t nowMs) {
	uint8_t buffer[NETPLAY_HEADER_SIZE + 8];
	size_t size = NetplayClientWriteHeader(client, buffer, NETPLAY_PKT_HELLO, nowMs);
	NetplayPut32(&buffer[size], client->romCrc);
	NetplayPut32(&buffer[size + 4], 0);
	NetplayClientSendPacket(client, buffer, size + 8);
}

static void _sendInputs(struct NetplayClient* client, uint64_t nowMs) {
	struct NetplaySession* session = client->session;
	uint8_t buffer[NETPLAY_PACKET_MAX];
	size_t size = NetplayClientWriteHeader(client, buffer, NETPLAY_PKT_INPUT, nowMs);
	NetplayPut32(&buffer[size], session->frame);
	size += 4;
	int p;
	for (p = 0; p < NETPLAY_MAX_PLAYERS; ++p) {
		uint32_t next = 0;
		if (p < client->nPlayers) {
			next = (uint32_t) (NetplaySessionPlayerConfirmed(session, p) + 1);
		}
		NetplayPut32(&buffer[size], next);
		size += 4;
	}

	// Resend everything any peer still lacks, so a lost packet costs nothing but a little latency
	int64_t start = session->localNext;
	for (p = 0; p < client->nPlayers; ++p) {
		if (p == client->localPlayer || client->peers[p].gone) {
			continue;
		}
		if (client->peers[p].ackedLocal + 1 < start) {
			start = client->peers[p].ackedLocal + 1;
		}
	}
	if (start < (int64_t) session->localNext - NETPLAY_INPUT_RING / 2) {
		start = (int64_t) session->localNext - NETPLAY_INPUT_RING / 2;
	}
	if (start < 0) {
		start = 0;
	}
	unsigned count = 0;
	int64_t f;
	for (f = start; f < session->localNext && count < NETPLAY_MAX_INPUTS_PER_PACKET; ++f) {
		uint16_t keys;
		if (!NetplaySessionGetInput(session, client->localPlayer, f, &keys)) {
			if (count == 0) {
				start = f + 1;
				continue;
			}
			break;
		}
		NetplayPut16(&buffer[size + 5 + count * 2], keys);
		++count;
	}
	NetplayPut32(&buffer[size], (uint32_t) start);
	buffer[size + 4] = count;
	size += 5 + count * 2;
	NetplayClientSendPacket(client, buffer, size);
}

void NetplayClientFlush(struct NetplayClient* client, uint64_t nowMs) {
	client->nowMs = nowMs;
	if (client->session) {
		_sendInputs(client, nowMs);
		client->lastSendMs = nowMs;
	} else if (nowMs - client->lastSendMs >= NETPLAY_HELLO_INTERVAL_MS || !client->lastSendMs) {
		_sendHello(client, nowMs);
		client->lastSendMs = nowMs;
	}
	NetplaySyncFlush(client, nowMs);
}

void NetplayClientSendBye(struct NetplayClient* client, uint64_t nowMs) {
	uint8_t buffer[NETPLAY_HEADER_SIZE];
	size_t size = NetplayClientWriteHeader(client, buffer, NETPLAY_PKT_BYE, nowMs);
	int i;
	for (i = 0; i < 3; ++i) {
		NetplayClientSendPacket(client, buffer, size);
	}
}

bool NetplayClientAllPeersSeen(const struct NetplayClient* client) {
	int p;
	for (p = 0; p < client->nPlayers; ++p) {
		if (p != client->localPlayer && !client->peers[p].seen) {
			return false;
		}
	}
	return true;
}

bool NetplayClientRomMismatch(const struct NetplayClient* client, int* player) {
	int p;
	for (p = 0; p < client->nPlayers; ++p) {
		if (p != client->localPlayer && client->peers[p].seen && client->peers[p].romCrc && client->peers[p].romCrc != client->romCrc) {
			if (player) {
				*player = p;
			}
			return true;
		}
	}
	return false;
}

float NetplayClientFrameAdvantage(const struct NetplayClient* client, uint64_t nowMs) {
	if (!client->session) {
		return 0;
	}
	float slowest = -1;
	int p;
	for (p = 0; p < client->nPlayers; ++p) {
		const struct NetplayPeer* peer = &client->peers[p];
		if (p == client->localPlayer || peer->gone || peer->remoteFrame < 0) {
			continue;
		}
		// Estimate where the peer is now: its last report, plus the one-way delay and time since we heard it
		float elapsed = (float) (nowMs - peer->lastHeardMs) + peer->rttMs / 2.0f;
		float estimate = peer->remoteFrame + elapsed / FRAME_MS;
		if (slowest < 0 || estimate < slowest) {
			slowest = estimate;
		}
	}
	if (slowest < 0) {
		return 0;
	}
	return client->session->frame - slowest;
}

bool NetplayClientShouldStall(struct NetplayClient* client, uint64_t nowMs) {
	// Skipping the occasional tick lets the peer that is behind catch up without visible hitches
	float advantage = NetplayClientFrameAdvantage(client, nowMs);
	if (client->stallCooldown > 0) {
		--client->stallCooldown;
	}
	bool stall = false;
	if (advantage >= 4.0f) {
		stall = true;
	} else if (advantage >= 1.5f && !client->stallCooldown) {
		stall = true;
		client->stallCooldown = 6;
	}
	if (stall) {
		++client->stats.timeSyncStalls;
	}
	return stall;
}

uint32_t NetplayClientMaxRtt(const struct NetplayClient* client) {
	uint32_t rtt = 0;
	int p;
	for (p = 0; p < client->nPlayers; ++p) {
		if (p != client->localPlayer && client->peers[p].rttMs > rtt) {
			rtt = client->peers[p].rttMs;
		}
	}
	return rtt;
}
