/* Copyright (c) 2026 mgba-netplay contributors
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include <mgba/core/core.h>
#include <mgba/feature/netplay.h>
#include <mgba/internal/gba/gba.h>
#include <mgba-util/audio-buffer.h>
#include <mgba-util/vfs.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifndef _WIN32
#include <unistd.h>
#endif

#define RTC_EPOCH_MS 1700000000000LL
#define SAVE_SIZE 0x8000
#define MAX_HASHES 4096

struct Options {
	const char* mode;
	const char* rom;
	int players;
	int frames;
	int latency;
	int jitter;
	double loss;
	int delay;
	uint32_t seed;
	double drift;
	int desyncAt;
	uint32_t slice;
	int player;
	uint16_t port;
	int nPeers;
	const char* peerHosts[NETPLAY_MAX_PLAYERS];
	uint16_t peerPorts[NETPLAY_MAX_PLAYERS];
	uint64_t token;
	bool quiet;
};

static void _usage(void) {
	puts("usage: mgba-netplay-test <mode> <rom> [options]\n"
	     "modes:\n"
	     "  determinism   two identical machines must match; save/restore must replay identically\n"
	     "  rollback      simulated clients over a lossy, laggy in-memory network vs. a reference run\n"
	     "  udp           one real client over UDP (run one process per player)\n"
	     "  bench         time frames and snapshots\n"
	     "options:\n"
	     "  --players N  --frames N  --seed N  --slice CYCLES  --delay FRAMES\n"
	     "  --latency MS --jitter MS --loss PERCENT --drift PERCENT --desync-at FRAME   (rollback)\n"
	     "  --player N --port N --peer HOST:PORT [--peer ...] --token N                 (udp)");
}

static bool _parse(int argc, char** argv, struct Options* opts) {
	memset(opts, 0, sizeof(*opts));
	opts->players = 2;
	opts->frames = 1800;
	opts->latency = 60;
	opts->jitter = 20;
	opts->loss = 5;
	opts->delay = 2;
	opts->seed = 1234;
	opts->drift = 0.5;
	opts->desyncAt = -1;
	opts->slice = NETPLAY_DEFAULT_SLICE_CYCLES;
	opts->token = 0x4E50544553540001ULL;
	if (argc < 3) {
		return false;
	}
	opts->mode = argv[1];
	opts->rom = argv[2];
	int i;
	for (i = 3; i < argc; ++i) {
		const char* arg = argv[i];
		const char* value = i + 1 < argc ? argv[i + 1] : NULL;
		if (!strcmp(arg, "--quiet")) {
			opts->quiet = true;
			continue;
		}
		if (!value) {
			return false;
		}
		++i;
		if (!strcmp(arg, "--players")) {
			opts->players = atoi(value);
		} else if (!strcmp(arg, "--frames")) {
			opts->frames = atoi(value);
		} else if (!strcmp(arg, "--latency")) {
			opts->latency = atoi(value);
		} else if (!strcmp(arg, "--jitter")) {
			opts->jitter = atoi(value);
		} else if (!strcmp(arg, "--loss")) {
			opts->loss = atof(value);
		} else if (!strcmp(arg, "--delay")) {
			opts->delay = atoi(value);
		} else if (!strcmp(arg, "--seed")) {
			opts->seed = strtoul(value, NULL, 0);
		} else if (!strcmp(arg, "--drift")) {
			opts->drift = atof(value);
		} else if (!strcmp(arg, "--desync-at")) {
			opts->desyncAt = atoi(value);
		} else if (!strcmp(arg, "--slice")) {
			opts->slice = strtoul(value, NULL, 0);
		} else if (!strcmp(arg, "--player")) {
			opts->player = atoi(value);
		} else if (!strcmp(arg, "--port")) {
			opts->port = atoi(value);
		} else if (!strcmp(arg, "--token")) {
			opts->token = strtoull(value, NULL, 0);
		} else if (!strcmp(arg, "--peer")) {
			const char* colon = strrchr(value, ':');
			if (!colon || opts->nPeers >= NETPLAY_MAX_PLAYERS) {
				return false;
			}
			size_t len = colon - value;
			char* host = malloc(len + 1);
			memcpy(host, value, len);
			host[len] = '\0';
			opts->peerHosts[opts->nPeers] = host;
			opts->peerPorts[opts->nPeers] = atoi(colon + 1);
			++opts->nPeers;
		} else {
			return false;
		}
	}
	return opts->players >= 1 && opts->players <= NETPLAY_MAX_PLAYERS;
}

static uint32_t _mix(uint32_t x) {
	x ^= x >> 16;
	x *= 0x7FEB352D;
	x ^= x >> 15;
	x *= 0x846CA68B;
	x ^= x >> 16;
	return x;
}

// Scripted "player": buttons change every few frames, with periodic SELECT windows that flip the ROM into normal mode
static uint16_t _keysFor(uint32_t seed, int player, uint32_t frame) {
	uint32_t x = _mix(seed ^ (player * 0x9E3779B9u) ^ ((frame / 6) * 0x85EBCA6Bu));
	uint16_t keys = x & 0x3FB;
	if ((frame / 300) % 3 == 1) {
		keys |= 0x4;
	}
	return keys;
}

static void _makeSave(uint32_t seed, int player, uint8_t* out) {
	uint32_t i;
	for (i = 0; i < SAVE_SIZE; ++i) {
		out[i] = _mix(seed * 31 + player * 977 + i);
	}
}

struct Machine {
	int n;
	struct mCore* cores[NETPLAY_MAX_PLAYERS];
	mColor* video[NETPLAY_MAX_PLAYERS];
	struct NetplayLink link;
};

static bool _machineInit(struct Machine* machine, const char* rom, int n, uint8_t* const* saves, uint32_t slice) {
	memset(machine, 0, sizeof(*machine));
	machine->n = n;
	int p;
	for (p = 0; p < n; ++p) {
		struct mCore* core = mCoreFind(rom);
		if (!core || !core->init(core)) {
			fprintf(stderr, "Could not create a core for %s\n", rom);
			return false;
		}
		mCoreInitConfig(core, NULL);
		NetplayPrepareCore(core, RTC_EPOCH_MS);
		machine->video[p] = calloc(GBA_VIDEO_HORIZONTAL_PIXELS * GBA_VIDEO_VERTICAL_PIXELS, sizeof(mColor));
		core->setVideoBuffer(core, machine->video[p], GBA_VIDEO_HORIZONTAL_PIXELS);
		if (!mCoreLoadFile(core, rom)) {
			fprintf(stderr, "Could not load %s\n", rom);
			return false;
		}
		if (saves && saves[p]) {
			core->loadSave(core, VFileMemChunk(saves[p], SAVE_SIZE));
		} else {
			core->loadSave(core, VFileMemChunk(NULL, 0));
		}
		core->reset(core);
		machine->cores[p] = core;
	}
	NetplayLinkInit(&machine->link, machine->cores, n);
	machine->link.sliceCycles = slice;
	return true;
}

static void _machineDeinit(struct Machine* machine) {
	NetplayLinkDeinit(&machine->link);
	int p;
	for (p = 0; p < machine->n; ++p) {
		machine->cores[p]->deinit(machine->cores[p]);
		free(machine->video[p]);
	}
}

static void _clearAudio(struct Machine* machine) {
	int p;
	for (p = 0; p < machine->n; ++p) {
		struct mAudioBuffer* audio = machine->cores[p]->getAudioBuffer(machine->cores[p]);
		if (audio) {
			mAudioBufferClear(audio);
		}
	}
}

static uint32_t _hashMachine(struct Machine* machine, void* scratch) {
	size_t size = NetplayLinkSnapshot(&machine->link, scratch);
	return NetplaySnapshotHash(scratch, size);
}

static uint8_t** _makeSaves(const struct Options* opts) {
	uint8_t** saves = calloc(NETPLAY_MAX_PLAYERS, sizeof(*saves));
	int p;
	for (p = 0; p < opts->players; ++p) {
		saves[p] = malloc(SAVE_SIZE);
		_makeSave(opts->seed, p, saves[p]);
	}
	return saves;
}

// Reference hashes: the start-of-frame state every NETPLAY_HASH_INTERVAL frames with perfect knowledge of inputs
static int _referenceRun(const struct Options* opts, uint8_t* const* saves, uint32_t* hashes, uint64_t* transfers) {
	struct Machine machine;
	if (!_machineInit(&machine, opts->rom, opts->players, saves, opts->slice)) {
		return -1;
	}
	void* scratch = malloc(NetplayLinkSnapshotCapacity(&machine.link));
	int count = 0;
	uint32_t f;
	for (f = 0; f < (uint32_t) opts->frames; ++f) {
		if (f % NETPLAY_HASH_INTERVAL == 0) {
			hashes[count++] = _hashMachine(&machine, scratch);
			if (getenv("NP_DUMP") && f == 60) {
				FILE* out = fopen("/tmp/np-ref-60.bin", "wb");
				fwrite(scratch, 1, NetplayLinkSnapshot(&machine.link, scratch), out);
				fclose(out);
			}
		} else if (getenv("NP_SNAPSHOT_EVERY_FRAME")) {
			NetplayLinkSnapshot(&machine.link, scratch);
		}
		uint16_t keys[NETPLAY_MAX_PLAYERS] = {0};
		int p;
		for (p = 0; p < opts->players; ++p) {
			keys[p] = f < (uint32_t) opts->delay ? 0 : _keysFor(opts->seed, p, f);
		}
		NetplayLinkRunFrame(&machine.link, keys);
		_clearAudio(&machine);
	}
	if (transfers) {
		*transfers = machine.link.transfersStarted;
	}
	free(scratch);
	_machineDeinit(&machine);
	return count;
}

static int _testDeterminism(const struct Options* opts) {
	uint8_t** saves = _makeSaves(opts);
	struct Machine a, b;
	if (!_machineInit(&a, opts->rom, opts->players, saves, opts->slice) || !_machineInit(&b, opts->rom, opts->players, saves, opts->slice)) {
		return 1;
	}
	void* scratch = malloc(NetplayLinkSnapshotCapacity(&a.link));
	void* saved = malloc(NetplayLinkSnapshotCapacity(&a.link));
	size_t savedSize = 0;
	int failures = 0;
	uint32_t replayHashes[256];
	int replayCount = 0;
	uint32_t half = opts->frames / 2;
	uint32_t f;
	for (f = 0; f < (uint32_t) opts->frames; ++f) {
		if (f == half) {
			savedSize = NetplayLinkSnapshot(&a.link, saved);
		}
		if (f >= half && f < half + 240 && f % 16 == 0 && replayCount < 256) {
			replayHashes[replayCount++] = _hashMachine(&a, scratch);
		}
		if (f % NETPLAY_HASH_INTERVAL == 0) {
			uint32_t ha = _hashMachine(&a, scratch);
			uint32_t hb = _hashMachine(&b, scratch);
			if (ha != hb) {
				printf("FAIL frame %u: machine hashes differ (%08X vs %08X)\n", f, ha, hb);
				++failures;
			}
		}
		uint16_t keys[NETPLAY_MAX_PLAYERS] = {0};
		int p;
		for (p = 0; p < opts->players; ++p) {
			keys[p] = _keysFor(opts->seed, p, f);
		}
		NetplayLinkRunFrame(&a.link, keys);
		NetplayLinkRunFrame(&b.link, keys);
		_clearAudio(&a);
		_clearAudio(&b);
	}
	uint32_t finalA = _hashMachine(&a, scratch);
	printf("transfers started: %llu\n", (unsigned long long) a.link.transfersStarted);

	// Restoring mid-run and replaying the same inputs must reproduce the same states
	if (!NetplayLinkRestore(&a.link, saved, savedSize)) {
		printf("FAIL: could not restore snapshot\n");
		++failures;
	} else {
		int i = 0;
		for (f = half; f < half + 240 && f < (uint32_t) opts->frames; ++f) {
			if (f % 16 == 0 && i < replayCount) {
				uint32_t h = _hashMachine(&a, scratch);
				if (h != replayHashes[i]) {
					printf("FAIL frame %u: replay after restore diverged (%08X vs %08X)\n", f, h, replayHashes[i]);
					++failures;
				}
				++i;
			}
			uint16_t keys[NETPLAY_MAX_PLAYERS] = {0};
			int p;
			for (p = 0; p < opts->players; ++p) {
				keys[p] = _keysFor(opts->seed, p, f);
			}
			NetplayLinkRunFrame(&a.link, keys);
			_clearAudio(&a);
		}
	}

	// Sanity: one different button press on one player must change the outcome
	NetplayLinkRestore(&a.link, saved, savedSize);
	NetplayLinkRestore(&b.link, saved, savedSize);
	for (f = half; f < half + 60; ++f) {
		uint16_t keys[NETPLAY_MAX_PLAYERS] = {0};
		int p;
		for (p = 0; p < opts->players; ++p) {
			keys[p] = _keysFor(opts->seed, p, f);
		}
		NetplayLinkRunFrame(&a.link, keys);
		keys[opts->players - 1] ^= f == half + 1 ? 0x1 : 0;
		NetplayLinkRunFrame(&b.link, keys);
	}
	if (_hashMachine(&a, scratch) == _hashMachine(&b, scratch)) {
		printf("FAIL: changing player %i's input did not change the state\n", opts->players - 1);
		++failures;
	}
	// Player 0 only learns about the other player's buttons through link transfers
	if (opts->players > 1) {
		size_t sizeA, sizeB;
		void* iwramA = mCoreGetMemoryBlock(a.cores[0], 0x03000000, &sizeA);
		void* iwramB = mCoreGetMemoryBlock(b.cores[0], 0x03000000, &sizeB);
		if (!iwramA || !iwramB || !memcmp(iwramA, iwramB, sizeA < 0x8000 ? sizeA : 0x8000)) {
			printf("FAIL: player 0 never saw player %i's input over the link\n", opts->players - 1);
			++failures;
		} else {
			printf("link carries data: player 0's memory reflects player %i's input\n", opts->players - 1);
		}
	}

	printf("final hash %08X\n", finalA);
	printf("%s: determinism (%i players, %i frames)\n", failures ? "FAIL" : "PASS", opts->players, opts->frames);
	free(scratch);
	free(saved);
	_machineDeinit(&a);
	_machineDeinit(&b);
	return failures ? 1 : 0;
}

/* ---- Simulated network ---- */

struct SimPacket {
	uint64_t deliverAt;
	int to;
	size_t size;
	struct SimPacket* next;
	uint8_t data[NETPLAY_PACKET_MAX + 64];
};

struct SimNet {
	struct SimPacket* head;
	uint64_t now;
	uint32_t rng;
	int n;
	int latency;
	int jitter;
	double loss;
	uint64_t sent;
	uint64_t dropped;
};

struct SimEndpoint {
	struct SimNet* net;
	int id;
};

static uint32_t _simRand(struct SimNet* net) {
	net->rng ^= net->rng << 13;
	net->rng ^= net->rng >> 17;
	net->rng ^= net->rng << 5;
	return net->rng;
}

static bool _simSend(struct NetplayTransport* transport, const void* data, size_t size) {
	struct SimEndpoint* endpoint = transport->context;
	struct SimNet* net = endpoint->net;
	int to;
	for (to = 0; to < net->n; ++to) {
		if (to == endpoint->id) {
			continue;
		}
		++net->sent;
		if ((_simRand(net) % 10000) < net->loss * 100) {
			++net->dropped;
			continue;
		}
		struct SimPacket* packet = malloc(sizeof(*packet));
		packet->deliverAt = net->now + net->latency / 2 + (net->jitter ? _simRand(net) % (net->jitter + 1) : 0);
		packet->to = to;
		packet->size = size;
		memcpy(packet->data, data, size);
		packet->next = net->head;
		net->head = packet;
	}
	return true;
}

static int _simRecv(struct NetplayTransport* transport, void* buffer, size_t size) {
	struct SimEndpoint* endpoint = transport->context;
	struct SimNet* net = endpoint->net;
	struct SimPacket** best = NULL;
	struct SimPacket** cursor;
	for (cursor = &net->head; *cursor; cursor = &(*cursor)->next) {
		if ((*cursor)->to == endpoint->id && (*cursor)->deliverAt <= net->now && (!best || (*cursor)->deliverAt < (*best)->deliverAt)) {
			best = cursor;
		}
	}
	if (!best) {
		return -1;
	}
	struct SimPacket* packet = *best;
	*best = packet->next;
	int length = packet->size < size ? (int) packet->size : (int) size;
	memcpy(buffer, packet->data, length);
	free(packet);
	return length;
}

struct SimClient {
	int player;
	struct SimEndpoint endpoint;
	struct NetplayTransport transport;
	struct NetplayClient client;
	struct Machine machine;
	struct NetplaySession session;
	bool started;
	bool injected;
	double nextTick;
	double interval;
	uint8_t* saves[NETPLAY_MAX_PLAYERS];
	uint32_t hashes[MAX_HASHES];
	bool haveHash[MAX_HASHES];
	uint64_t stalls;
};

static void _simConfirmed(void* context, uint32_t frame, uint32_t hash, const void* snapshot, size_t size) {
	struct SimClient* sim = context;
	if (getenv("NP_DUMP") && frame == 60) {
		char path[64];
		snprintf(path, sizeof(path), "/tmp/np-sim%i-60.bin", sim->player);
		FILE* out = fopen(path, "wb");
		fwrite(snapshot, 1, size, out);
		fclose(out);
	}
	uint32_t index = frame / NETPLAY_HASH_INTERVAL;
	if (index < MAX_HASHES) {
		sim->hashes[index] = hash;
		sim->haveHash[index] = true;
	}
	NetplayClientOnConfirmedState(&sim->client, frame, hash, snapshot, size);
}

static const struct Options* _simOpts;

static void _simFrameFinished(void* context, uint32_t frame, bool resimulating) {
	struct SimClient* sim = context;
	_clearAudio(&sim->machine);
	if (getenv("NP_DEBUG") && !resimulating && frame < 200) {
		int p;
		for (p = 0; p < sim->machine.n; ++p) {
			uint16_t expected = frame < (uint32_t) _simOpts->delay ? 0 : _keysFor(_simOpts->seed, p, frame);
			uint16_t used = sim->session.used[frame % NETPLAY_INPUT_RING][p];
			if (used != expected && NetplaySessionPlayerConfirmed(&sim->session, p) >= frame) {
				printf("client %i frame %u: player %i used %03X, script says %03X\n", sim->player, frame, p, used, expected);
			}
		}
	}
}

static bool _simStart(struct SimClient* sim, const struct Options* opts) {
	if (!_machineInit(&sim->machine, opts->rom, opts->players, sim->saves, opts->slice)) {
		return false;
	}
	if (getenv("NP_DEBUG")) {
		int q;
		for (q = 0; q < opts->players; ++q) {
			struct GBASavedata* savedata = &((struct GBA*) sim->machine.cores[q]->board)->memory.savedata;
			uint8_t* sram = savedata->data;
			printf("client %i core %i: type %i data %p byte100 %02X expected %02X\n", sim->player, q, savedata->type,
			       (void*) sram, sram ? sram[100] : 0, sim->saves[q] ? sim->saves[q][100] : 0);
		}
	}
	if (!NetplaySessionInit(&sim->session, &sim->machine.link, sim->player, opts->delay)) {
		return false;
	}
	sim->session.callbacks.context = sim;
	sim->session.callbacks.confirmedState = _simConfirmed;
	sim->session.callbacks.frameFinished = _simFrameFinished;
	NetplayClientAttachSession(&sim->client, &sim->session);
	sim->started = true;
	return true;
}

static int _testRollback(const struct Options* opts) {
	_simOpts = opts;
	uint8_t** saves = _makeSaves(opts);
	uint32_t* reference = calloc(MAX_HASHES, sizeof(uint32_t));
	uint64_t transfers = 0;
	int referenceCount = _referenceRun(opts, saves, reference, &transfers);
	if (referenceCount < 0) {
		return 1;
	}
	printf("reference run: %i hashes, %llu link transfers\n", referenceCount, (unsigned long long) transfers);

	struct SimNet net = {
		.rng = opts->seed | 1,
		.n = opts->players,
		.latency = opts->latency,
		.jitter = opts->jitter,
		.loss = opts->loss,
	};
	struct SimClient* sims = calloc(opts->players, sizeof(*sims));
	int p;
	for (p = 0; p < opts->players; ++p) {
		struct SimClient* sim = &sims[p];
		sim->player = p;
		sim->endpoint.net = &net;
		sim->endpoint.id = p;
		sim->transport.context = &sim->endpoint;
		sim->transport.send = _simSend;
		sim->transport.recv = _simRecv;
		NetplayClientInit(&sim->client, &sim->transport, opts->token, p, opts->players, 0xC0FFEE);
		sim->client.startMs = 0;
		// Each machine's clock runs slightly fast or slow, and they start at different times
		double drift = (p % 2 ? 1 : -1) * opts->drift * p / 100.0;
		sim->interval = 16.7427 * (1.0 + drift);
		sim->nextTick = 7.0 * p;
		sim->saves[p] = saves[p];
		NetplayClientShareBlob(&sim->client, NETPLAY_BLOB_SAVE, 0xFF, 0, saves[p], SAVE_SIZE);
	}

	int failures = 0;
	uint64_t limitMs = (uint64_t) (opts->frames * 16.7427 * 4) + 30000;
	bool done = false;
	for (net.now = 0; net.now < limitMs && !done; ++net.now) {
		done = true;
		for (p = 0; p < opts->players; ++p) {
			struct SimClient* sim = &sims[p];
			if (net.now < sim->nextTick) {
				done = done && sim->started && NetplaySessionConfirmedFrame(&sim->session) >= opts->frames;
				continue;
			}
			sim->nextTick += sim->interval;
			NetplayClientPoll(&sim->client, net.now);
			if (!sim->started) {
				int q;
				bool ready = NetplayClientAllPeersSeen(&sim->client) && NetplayClientBlobSent(&sim->client);
				for (q = 0; q < opts->players && ready; ++q) {
					if (sim->saves[q]) {
						continue;
					}
					void* data;
					size_t size;
					if (NetplayClientTakeBlob(&sim->client, q, NETPLAY_BLOB_SAVE, &data, &size, NULL)) {
						if (size != SAVE_SIZE || memcmp(data, saves[q], SAVE_SIZE)) {
							printf("FAIL: client %i received a corrupt save from player %i\n", p, q);
							++failures;
						}
						sim->saves[q] = data;
					} else {
						ready = false;
					}
				}
				if (ready) {
					if (!_simStart(sim, opts)) {
						return 1;
					}
					if (!opts->quiet) {
						printf("client %i started at %llu ms\n", p, (unsigned long long) net.now);
					}
				}
			} else if (NetplaySessionConfirmedFrame(&sim->session) < opts->frames + NETPLAY_MAX_ROLLBACK) {
				if (opts->desyncAt >= 0 && !sim->injected && p == opts->players - 1 && (int) sim->session.frame >= opts->desyncAt) {
					// Corrupt one byte of work RAM to prove desyncs are detected and repaired
					struct mCore* victim = sim->machine.cores[0];
					victim->busWrite8(victim, 0x02000100, victim->busRead8(victim, 0x02000100) ^ 0x5A);
					sim->injected = true;
					printf("client %i: injected a desync at frame %u\n", p, sim->session.frame);
				}
				if (NetplayClientShouldStall(&sim->client, net.now)) {
					++sim->stalls;
				} else {
					if (NetplaySessionWantsLocalInput(&sim->session)) {
						uint32_t frame = sim->session.localNext;
						NetplaySessionAddLocalInput(&sim->session, _keysFor(opts->seed, p, frame));
					}
					if (NetplaySessionCanAdvance(&sim->session)) {
						NetplaySessionAdvance(&sim->session);
					} else {
						++sim->stalls;
					}
				}
			}
			NetplayClientFlush(&sim->client, net.now);
			done = done && sim->started && NetplaySessionConfirmedFrame(&sim->session) >= opts->frames;
		}
	}
	if (!done) {
		printf("FAIL: simulation did not finish within %llu ms of simulated time\n", (unsigned long long) limitMs);
		++failures;
	}

	for (p = 0; p < opts->players; ++p) {
		struct SimClient* sim = &sims[p];
		int compared = 0;
		int mismatched = 0;
		int firstBad = -1;
		int lastBad = -1;
		int i;
		for (i = 0; i < referenceCount; ++i) {
			if (!sim->haveHash[i]) {
				continue;
			}
			++compared;
			if (sim->hashes[i] != reference[i]) {
				++mismatched;
				if (firstBad < 0) {
					firstBad = i;
				}
				lastBad = i;
			}
		}
		const struct NetplaySessionStats* stats = &sim->session.stats;
		printf("client %i: frame %u, %i/%i hashes match reference, rollbacks %llu (resimulated %llu frames, max depth %u), "
		       "stalls %llu, time-sync stalls %llu, rtt %u ms, desyncs %u, resyncs %u\n",
		       p, sim->session.frame, compared - mismatched, compared,
		       (unsigned long long) stats->rollbacks, (unsigned long long) stats->framesResimulated, stats->maxRollbackDepth,
		       (unsigned long long) sim->stalls, (unsigned long long) sim->client.stats.timeSyncStalls,
		       NetplayClientMaxRtt(&sim->client), sim->client.stats.desyncs, sim->client.stats.resyncs);
		if (compared < referenceCount - 1) {
			printf("FAIL: client %i only produced %i of %i hashes\n", p, compared, referenceCount);
			++failures;
		}
		if (mismatched) {
			if (opts->desyncAt >= 0 && lastBad >= 0 && lastBad < referenceCount - 3 && sim->client.stats.resyncs) {
				printf("client %i: desynced from frame %i to %i, then recovered\n", p, firstBad * NETPLAY_HASH_INTERVAL, lastBad * NETPLAY_HASH_INTERVAL);
			} else {
				printf("FAIL: client %i diverged from the reference (frames %i..%i)\n", p, firstBad * NETPLAY_HASH_INTERVAL, lastBad * NETPLAY_HASH_INTERVAL);
				++failures;
			}
		}
	}
	if (opts->desyncAt >= 0 && !sims[opts->players - 1].client.stats.resyncs) {
		printf("FAIL: the injected desync was never repaired\n");
		++failures;
	}
	printf("network: %llu packets sent, %llu dropped\n", (unsigned long long) net.sent, (unsigned long long) net.dropped);
	printf("%s: rollback (%i players, %i frames, %i ms latency, %i ms jitter, %.1f%% loss, delay %i)\n",
	       failures ? "FAIL" : "PASS", opts->players, opts->frames, opts->latency, opts->jitter, opts->loss, opts->delay);
	return failures ? 1 : 0;
}

/* ---- Real UDP client ---- */

static void _sleepMs(int ms) {
#ifdef _WIN32
	Sleep(ms);
#else
	usleep(ms * 1000);
#endif
}

struct UdpRun {
	struct NetplayClient* client;
	struct Machine* machine;
	bool quiet;
};

static void _udpConfirmed(void* context, uint32_t frame, uint32_t hash, const void* snapshot, size_t size) {
	struct UdpRun* run = context;
	if (!run->quiet) {
		printf("hash frame=%u crc=%08X\n", frame, hash);
		fflush(stdout);
	}
	NetplayClientOnConfirmedState(run->client, frame, hash, snapshot, size);
}

static void _udpFrameFinished(void* context, uint32_t frame, bool resimulating) {
	UNUSED(frame);
	UNUSED(resimulating);
	struct UdpRun* run = context;
	_clearAudio(run->machine);
}

static int _testUdp(const struct Options* opts) {
	if (!opts->nPeers) {
		fprintf(stderr, "udp mode needs at least one --peer (another player, or the relay)\n");
		return 1;
	}
	struct NetplayTransport transport;
	if (!NetplayUDPTransportCreate(&transport, opts->port, opts->nPeers, opts->peerHosts, opts->peerPorts)) {
		fprintf(stderr, "could not open UDP transport\n");
		return 1;
	}
	struct NetplayClient client;
	NetplayClientInit(&client, &transport, opts->token, opts->player, opts->players, 0xC0FFEE);
	uint8_t** saves = _makeSaves(opts);
	NetplayClientShareBlob(&client, NETPLAY_BLOB_SAVE, 0xFF, 0, saves[opts->player], SAVE_SIZE);

	uint64_t start = NetplayNowMs();
	bool haveSave[NETPLAY_MAX_PLAYERS] = {0};
	haveSave[opts->player] = true;
	bool ready = false;
	while (!ready) {
		uint64_t now = NetplayNowMs();
		if (now - start > 30000) {
			fprintf(stderr, "timed out waiting for peers\n");
			return 1;
		}
		NetplayClientPoll(&client, now);
		NetplayClientFlush(&client, now);
		ready = NetplayClientAllPeersSeen(&client) && NetplayClientBlobSent(&client);
		int p;
		for (p = 0; p < opts->players; ++p) {
			void* data;
			size_t size;
			if (!haveSave[p] && NetplayClientTakeBlob(&client, p, NETPLAY_BLOB_SAVE, &data, &size, NULL)) {
				// Every player's save is generated from the shared seed, so we can verify what arrived
				if (size != SAVE_SIZE || memcmp(data, saves[p], SAVE_SIZE)) {
					fprintf(stderr, "corrupt save from player %i\n", p);
					return 1;
				}
				free(data);
				haveSave[p] = true;
			}
			ready = ready && haveSave[p];
		}
		_sleepMs(1);
	}
	printf("all %i players connected after %llu ms\n", opts->players, (unsigned long long) (NetplayNowMs() - start));

	struct Machine machine;
	if (!_machineInit(&machine, opts->rom, opts->players, saves, opts->slice)) {
		return 1;
	}
	struct NetplaySession session;
	NetplaySessionInit(&session, &machine.link, opts->player, opts->delay);
	struct UdpRun run = { .client = &client, .machine = &machine, .quiet = opts->quiet };
	session.callbacks.context = &run;
	session.callbacks.confirmedState = _udpConfirmed;
	session.callbacks.frameFinished = _udpFrameFinished;
	NetplayClientAttachSession(&client, &session);

	uint64_t stalls = 0;
	double nextTick = NetplayNowMs();
	uint64_t lastReport = NetplayNowMs();
	while (NetplaySessionConfirmedFrame(&session) < opts->frames) {
		uint64_t now = NetplayNowMs();
		if (now - start > (uint64_t) opts->frames * 17 * 4 + 60000) {
			fprintf(stderr, "timed out\n");
			return 1;
		}
		if (now < nextTick) {
			NetplayClientPoll(&client, now);
			_sleepMs(1);
			continue;
		}
		nextTick += 16.7427;
		NetplayClientPoll(&client, now);
		if (!NetplayClientShouldStall(&client, now)) {
			if (NetplaySessionWantsLocalInput(&session)) {
				NetplaySessionAddLocalInput(&session, _keysFor(opts->seed, opts->player, session.localNext));
			}
			if (NetplaySessionCanAdvance(&session)) {
				NetplaySessionAdvance(&session);
			} else {
				++stalls;
			}
		}
		NetplayClientFlush(&client, now);
		if (now - lastReport > 2000 && !opts->quiet) {
			lastReport = now;
			fprintf(stderr, "frame %u, rtt %u ms, advantage %.1f, rollbacks %llu, stalls %llu\n",
			        session.frame, NetplayClientMaxRtt(&client), NetplayClientFrameAdvantage(&client, now),
			        (unsigned long long) session.stats.rollbacks, (unsigned long long) stalls);
		}
	}
	// Keep answering for a moment so the slower peer can finish too
	uint64_t linger = NetplayNowMs() + 1500;
	while (NetplayNowMs() < linger) {
		NetplayClientPoll(&client, NetplayNowMs());
		NetplayClientFlush(&client, NetplayNowMs());
		_sleepMs(5);
	}
	NetplayClientSendBye(&client, NetplayNowMs());
	printf("done: frame %u, rollbacks %llu (max depth %u), stalls %llu, desyncs %u, resyncs %u, rtt %u ms\n",
	       session.frame, (unsigned long long) session.stats.rollbacks, session.stats.maxRollbackDepth,
	       (unsigned long long) stalls, client.stats.desyncs, client.stats.resyncs, NetplayClientMaxRtt(&client));
	NetplaySessionDeinit(&session);
	NetplayClientDeinit(&client);
	transport.deinit(&transport);
	_machineDeinit(&machine);
	return client.stats.desyncs ? 1 : 0;
}

static double _seconds(void) {
	return NetplayNowMs() / 1000.0;
}

static int _bench(const struct Options* opts) {
	struct Machine machine;
	if (!_machineInit(&machine, opts->rom, opts->players, NULL, opts->slice)) {
		return 1;
	}
	void* scratch = malloc(NetplayLinkSnapshotCapacity(&machine.link));
	int frames = opts->frames;
	double t0 = _seconds();
	int f;
	for (f = 0; f < frames; ++f) {
		uint16_t keys[NETPLAY_MAX_PLAYERS] = {0};
		NetplayLinkRunFrame(&machine.link, keys);
		_clearAudio(&machine);
	}
	double t1 = _seconds();
	size_t size = 0;
	for (f = 0; f < frames; ++f) {
		size = NetplayLinkSnapshot(&machine.link, scratch);
	}
	double t2 = _seconds();
	for (f = 0; f < frames; ++f) {
		NetplayLinkRestore(&machine.link, scratch, size);
	}
	double t3 = _seconds();
	double frameMs = (t1 - t0) * 1000 / frames;
	double snapMs = (t2 - t1) * 1000 / frames;
	double restoreMs = (t3 - t2) * 1000 / frames;
	printf("%i players: %.3f ms/frame, snapshot %.3f ms (%zu bytes), restore %.3f ms\n", opts->players, frameMs, snapMs, size, restoreMs);
	// Worst case per displayed frame: roll back NETPLAY_MAX_ROLLBACK frames and re-run them, each with a snapshot
	double worst = restoreMs + (NETPLAY_MAX_ROLLBACK + 1) * (frameMs + snapMs);
	printf("worst-case rollback frame: %.2f ms (budget 16.74 ms)\n", worst);
	free(scratch);
	_machineDeinit(&machine);
	return 0;
}

static void _log(struct mLogger* logger, int category, enum mLogLevel level, const char* format, va_list args) {
	UNUSED(logger);
	bool netplay = !strcmp(mLogCategoryId(category), "feature.netplay");
	if (!(level & (mLOG_FATAL | mLOG_ERROR | mLOG_WARN)) && !(netplay && level == mLOG_INFO)) {
		return;
	}
	fprintf(stderr, "[%s] ", mLogCategoryName(category));
	vfprintf(stderr, format, args);
	fputc('\n', stderr);
}

int main(int argc, char** argv) {
	struct Options opts;
	if (!_parse(argc, argv, &opts)) {
		_usage();
		return 2;
	}
	struct mLogger logger = { .log = _log };
	mLogSetDefaultLogger(&logger);
	if (!strcmp(opts.mode, "determinism")) {
		return _testDeterminism(&opts);
	}
	if (!strcmp(opts.mode, "rollback")) {
		return _testRollback(&opts);
	}
	if (!strcmp(opts.mode, "udp")) {
		return _testUdp(&opts);
	}
	if (!strcmp(opts.mode, "bench")) {
		return _bench(&opts);
	}
	_usage();
	return 2;
}
