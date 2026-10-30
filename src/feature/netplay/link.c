/* Copyright (c) 2026 mgba-netplay contributors
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include <mgba/feature/netplay.h>

#include <mgba/core/config.h>
#include <mgba/core/core.h>
#include <mgba/internal/gba/gba.h>
#include <mgba/internal/gba/io.h>
#include <mgba/internal/gba/savedata.h>
#include <mgba/internal/gba/serialize.h>
#include <mgba/internal/gba/sio.h>
#include <mgba-util/crc32.h>

mLOG_DEFINE_CATEGORY(NETPLAY, "Netplay", "feature.netplay");

#define LINK_DRIVER_ID 0x4B4C504E
#define SNAPSHOT_MAGIC 0x534E504E
#define SNAPSHOT_VERSION 1

struct NetplaySnapshotHeader {
	uint32_t magic;
	uint32_t version;
	uint32_t nPlayers;
	uint32_t frame;
	uint32_t coreStateSize;
	uint32_t saveSize[NETPLAY_MAX_PLAYERS];
};

static struct GBA* _gba(const struct NetplayLink* link, int player) {
	return link->cores[player]->board;
}

static uint64_t _now(struct NetplayLink* link, int player) {
	uint32_t raw = (uint32_t) mTimingCurrentTime(link->cores[player]->timing);
	link->state.time[player] += (uint32_t) (raw - link->state.lastRaw[player]);
	link->state.lastRaw[player] = raw;
	return link->state.time[player];
}

static void _refreshReady(struct NetplayLink* link) {
	bool allMulti = true;
	int p;
	for (p = 0; p < link->nPlayers; ++p) {
		if (_gba(link, p)->sio.mode != GBA_SIO_MULTI) {
			allMulti = false;
		}
	}
	for (p = 0; p < link->nPlayers; ++p) {
		struct GBASIO* sio = &_gba(link, p)->sio;
		if (sio->mode == GBA_SIO_MULTI) {
			sio->siocnt = GBASIOMultiplayerSetReady(sio->siocnt, allMulti);
			sio->rcnt = GBASIORegisterRCNTSetSd(sio->rcnt, allMulti);
		}
	}
}

static void _startPlayer(struct NetplayLink* link, int player) {
	struct NetplayLinkState* state = &link->state;
	struct GBA* gba = _gba(link, player);
	state->started[player] = 1;
	state->ready[player] = 0;
	if ((int32_t) gba->sio.mode != state->transferMode) {
		return;
	}
	bool isInitiator = player == state->initiator;
	switch (gba->sio.mode) {
	case GBA_SIO_MULTI:
		state->multiData[player] = gba->memory.io[GBA_REG(SIOMLT_SEND)];
		state->ready[player] = 1;
		if (!isInitiator) {
			gba->sio.siocnt = GBASIOMultiplayerFillBusy(gba->sio.siocnt);
		}
		break;
	case GBA_SIO_NORMAL_8:
	case GBA_SIO_NORMAL_32:
		if (isInitiator || (GBASIONormalIsStart(gba->sio.siocnt) && !GBASIONormalIsInternalSc(gba->sio.siocnt))) {
			state->ready[player] = 1;
			if (gba->sio.mode == GBA_SIO_NORMAL_8) {
				state->normalData[player] = gba->memory.io[GBA_REG(SIODATA8)] & 0xFF;
			} else {
				state->normalData[player] = gba->memory.io[GBA_REG(SIODATA32_LO)];
				state->normalData[player] |= (uint32_t) gba->memory.io[GBA_REG(SIODATA32_HI)] << 16;
			}
		}
		break;
	default:
		break;
	}
}

static bool _allLatched(const struct NetplayLink* link) {
	int p;
	for (p = 0; p < link->nPlayers; ++p) {
		if (!link->state.started[p]) {
			return false;
		}
	}
	return true;
}

static void _yieldRunning(struct NetplayLink* link, int player) {
	if (link->running != player) {
		return;
	}
	link->stops[player].stopped = true;
	GBAInterrupt(_gba(link, player));
}

static void _finishPlayer(struct NetplayLink* link, int player, uint32_t cyclesLate) {
	struct NetplayLinkState* state = &link->state;
	int p;
	if (!_allLatched(link)) {
		return;
	}
	state->finished[player] = 1;

	struct GBA* gba = _gba(link, player);
	if ((int32_t) gba->sio.mode == state->transferMode && state->ready[player]) {
		switch (gba->sio.mode) {
		case GBA_SIO_MULTI: {
			uint16_t data[4];
			memcpy(data, state->multiData, sizeof(data));
			GBASIOMultiplayerFinishTransfer(&gba->sio, data, cyclesLate);
			break;
		}
		case GBA_SIO_NORMAL_8:
		case GBA_SIO_NORMAL_32: {
			int source = (player + link->nPlayers - 1) % link->nPlayers;
			uint32_t data = state->ready[source] ? state->normalData[source] : 0xFFFFFFFF;
			if (gba->sio.mode == GBA_SIO_NORMAL_8) {
				GBASIONormal8FinishTransfer(&gba->sio, data & 0xFF, cyclesLate);
			} else {
				GBASIONormal32FinishTransfer(&gba->sio, data, cyclesLate);
			}
			break;
		}
		default:
			break;
		}
	}

	for (p = 0; p < link->nPlayers; ++p) {
		if (!state->finished[p]) {
			return;
		}
	}
	state->transferActive = 0;
}

static void _completeTransfer(struct NetplayLink* link) {
	int p;
	for (p = 0; p < link->nPlayers && link->state.transferActive; ++p) {
		if (!link->state.finished[p]) {
			_finishPlayer(link, p, 0);
		}
	}
	link->state.transferActive = 0;
}

static void _beginTransfer(struct NetplayLink* link, int player, uint16_t siocnt) {
	struct NetplayLinkState* state = &link->state;
	struct GBA* gba = _gba(link, player);
	enum GBASIOMode mode = gba->sio.mode;
	switch (mode) {
	case GBA_SIO_MULTI:
		if (player != 0) {
			return;
		}
		break;
	case GBA_SIO_NORMAL_8:
	case GBA_SIO_NORMAL_32:
		if (!GBASIONormalIsInternalSc(siocnt)) {
			// External clock: wait for the partner to drive the transfer
			return;
		}
		break;
	default:
		return;
	}
	if (state->transferActive) {
		mLOG(NETPLAY, DEBUG, "Player %i started a transfer while one was still in flight", player);
		_completeTransfer(link);
	}

	uint64_t now = _now(link, player);
	int32_t cycles = GBASIOTransferCycles(mode, siocnt, link->nPlayers - 1);
	if (cycles <= 0) {
		cycles = 1;
	}
	++link->transfersStarted;
	state->transferActive = 1;
	state->transferMode = mode;
	state->initiator = player;
	state->transferStart = now;
	state->transferEnd = now + cycles;
	memset(state->started, 0, sizeof(state->started));
	memset(state->finished, 0, sizeof(state->finished));
	memset(state->ready, 0, sizeof(state->ready));
	memset(state->multiData, 0xFF, sizeof(state->multiData));
	memset(state->normalData, 0xFF, sizeof(state->normalData));
	_startPlayer(link, player);
	// Stop the initiator here so the other GBAs can latch SIOMLT_SEND at transferStart,
	// matching mGBA lockstep. Finishing first sent 0xFFFF for anyone who had not run yet.
	_yieldRunning(link, player);
}

static uint64_t _nextEvent(const struct NetplayLink* link, int player) {
	const struct NetplayLinkState* state = &link->state;
	if (!state->transferActive) {
		return UINT64_MAX;
	}
	if (!state->started[player]) {
		return state->transferStart;
	}
	if (!_allLatched(link)) {
		return UINT64_MAX;
	}
	if (!state->finished[player]) {
		return state->transferEnd;
	}
	return UINT64_MAX;
}

static void _stopCallback(struct mTiming* timing, void* context, uint32_t cyclesLate) {
	UNUSED(timing);
	UNUSED(cyclesLate);
	struct NetplayLinkStop* stop = context;
	stop->stopped = true;
}

static void _runRaw(struct NetplayLink* link, int player, int32_t cycles) {
	struct mCore* core = link->cores[player];
	struct NetplayLinkStop* stop = &link->stops[player];
	stop->stopped = false;
	link->running = player;
	mTimingDeschedule(core->timing, &stop->event);
	mTimingSchedule(core->timing, &stop->event, cycles);
	while (!stop->stopped) {
		core->runLoop(core);
	}
	link->running = -1;
}

static void _runCore(struct NetplayLink* link, int player, uint64_t target) {
	while (true) {
		uint64_t now = _now(link, player);
		if (now >= target) {
			return;
		}
		if (link->state.transferActive && link->state.started[player] && !_allLatched(link)) {
			return;
		}
		uint64_t event = _nextEvent(link, player);
		if (event <= now) {
			uint32_t late = (uint32_t) (now - event);
			if (!link->state.started[player]) {
				_startPlayer(link, player);
				if (!_allLatched(link)) {
					return;
				}
			} else {
				_finishPlayer(link, player, late);
			}
			continue;
		}
		uint64_t stopAt = event < target ? event : target;
		_runRaw(link, player, (int32_t) (stopAt - now));
	}
}

static struct NetplayLink* _driverLink(struct GBASIODriver* driver) {
	return ((struct NetplayLinkDriver*) driver)->link;
}

static int _driverPlayer(const struct GBASIODriver* driver) {
	return ((const struct NetplayLinkDriver*) driver)->player;
}

static uint32_t _driverId(const struct GBASIODriver* driver) {
	UNUSED(driver);
	return LINK_DRIVER_ID;
}

static void _driverSetMode(struct GBASIODriver* driver, enum GBASIOMode mode) {
	UNUSED(mode);
	struct NetplayLink* link = _driverLink(driver);
	if (link->restoring) {
		return;
	}
	if (link->state.transferActive && link->state.transferMode != (int32_t) mode && link->state.initiator == _driverPlayer(driver)) {
		mLOG(NETPLAY, DEBUG, "Initiator %i changed modes mid-transfer", _driverPlayer(driver));
	}
	_refreshReady(link);
}

static bool _driverHandlesMode(struct GBASIODriver* driver, enum GBASIOMode mode) {
	UNUSED(driver);
	return mode == GBA_SIO_MULTI || mode == GBA_SIO_NORMAL_8 || mode == GBA_SIO_NORMAL_32;
}

static int _driverConnectedDevices(struct GBASIODriver* driver) {
	return _driverLink(driver)->nPlayers - 1;
}

static int _driverDeviceId(struct GBASIODriver* driver) {
	return _driverPlayer(driver);
}

static bool _driverStart(struct GBASIODriver* driver) {
	struct NetplayLink* link = _driverLink(driver);
	// Loading a state replays SIOCNT writes, which must not start transfers
	if (!link->restoring) {
		// SIOCNT's final value (baud, clock source) is only known in writeSIOCNT, which sio.c calls right after this
		link->state.pendingStart = _driverPlayer(driver);
	}
	return false;
}

static uint16_t _driverWriteSIOCNT(struct GBASIODriver* driver, uint16_t value) {
	struct NetplayLink* link = _driverLink(driver);
	int player = _driverPlayer(driver);
	if (!link->restoring && link->state.pendingStart == player) {
		link->state.pendingStart = NETPLAY_NO_PLAYER;
		_beginTransfer(link, player, value);
	}
	return value;
}

static uint16_t _driverWriteRCNT(struct GBASIODriver* driver, uint16_t value) {
	UNUSED(driver);
	return value;
}

void NetplayPrepareCore(struct mCore* core, int64_t rtcStartMs) {
	mCoreConfigSetOverrideValue(&core->config, "idleOptimization", "ignore");
	mCoreConfigSetOverrideIntValue(&core->config, "threadedVideo", 0);
	mCoreConfigSetOverrideIntValue(&core->config, "hwaccelVideo", 0);
	mCoreConfigSetOverrideIntValue(&core->config, "useBios", 0);
	mCoreConfigSetOverrideIntValue(&core->config, "skipBios", 1);
	mCoreConfigSetOverrideIntValue(&core->config, "rewindEnable", 0);
	// Deliberately not reading the user's config file: every client must run with identical settings
	mCoreLoadForeignConfig(core, &core->config);
	core->rtc.override = RTC_FAKE_EPOCH;
	core->rtc.value = rtcStartMs;
}

void NetplayLinkInit(struct NetplayLink* link, struct mCore* const* cores, int nPlayers) {
	memset(link, 0, sizeof(*link));
	if (nPlayers > NETPLAY_MAX_PLAYERS) {
		nPlayers = NETPLAY_MAX_PLAYERS;
	}
	link->nPlayers = nPlayers;
	link->running = -1;
	link->sliceCycles = NETPLAY_DEFAULT_SLICE_CYCLES;
	link->state.pendingStart = NETPLAY_NO_PLAYER;
	link->state.initiator = NETPLAY_NO_PLAYER;
	link->state.transferMode = -1;

	int p;
	for (p = 0; p < nPlayers; ++p) {
		struct mCore* core = cores[p];
		link->cores[p] = core;
		size_t size = core->stateSize(core);
		if (size > link->coreStateSize) {
			link->coreStateSize = size;
		}

		struct NetplayLinkStop* stop = &link->stops[p];
		stop->event.context = stop;
		stop->event.callback = _stopCallback;
		stop->event.name = "Netplay slice end";
		stop->event.priority = 0xFF;

		struct NetplayLinkDriver* driver = &link->drivers[p];
		driver->link = link;
		driver->player = p;
		driver->d.driverId = _driverId;
		driver->d.setMode = _driverSetMode;
		driver->d.handlesMode = _driverHandlesMode;
		driver->d.connectedDevices = _driverConnectedDevices;
		driver->d.deviceId = _driverDeviceId;
		driver->d.writeSIOCNT = _driverWriteSIOCNT;
		driver->d.writeRCNT = _driverWriteRCNT;
		driver->d.start = _driverStart;
		core->setPeripheral(core, mPERIPH_GBA_LINK_PORT, &driver->d);

		link->state.lastRaw[p] = (uint32_t) mTimingCurrentTime(core->timing);
		link->state.time[p] = 0;
	}
	_refreshReady(link);
}

void NetplayLinkDeinit(struct NetplayLink* link) {
	int p;
	for (p = 0; p < link->nPlayers; ++p) {
		struct mCore* core = link->cores[p];
		mTimingDeschedule(core->timing, &link->stops[p].event);
		core->setPeripheral(core, mPERIPH_GBA_LINK_PORT, NULL);
	}
	link->nPlayers = 0;
}

void NetplayLinkSioInfo(const struct NetplayLink* link, int player, int* mode, int* ready, uint64_t* transfers) {
	if (mode) {
		*mode = (player >= 0 && player < link->nPlayers) ? (int) _gba(link, player)->sio.mode : -1;
	}
	if (ready) {
		*ready = 0;
		if (player >= 0 && player < link->nPlayers) {
			struct GBASIO* sio = &_gba(link, player)->sio;
			*ready = sio->mode == GBA_SIO_MULTI && GBASIOMultiplayerIsReady(sio->siocnt);
		}
	}
	if (transfers) {
		*transfers = link->transfersStarted;
	}
}

void NetplayLinkRunFrame(struct NetplayLink* link, const uint16_t* keys) {
	int p;
	for (p = 0; p < link->nPlayers; ++p) {
		link->cores[p]->setKeys(link->cores[p], keys[p]);
	}
	uint64_t t = (uint64_t) link->state.frame * NETPLAY_FRAME_CYCLES;
	uint64_t frameEnd = t + NETPLAY_FRAME_CYCLES;
	while (t < frameEnd) {
		t += link->sliceCycles;
		if (t > frameEnd) {
			t = frameEnd;
		}
		for (p = 0; p < link->nPlayers; ++p) {
			_runCore(link, p, t);
		}
	}
	++link->state.frame;
}

size_t NetplayLinkSnapshotCapacity(const struct NetplayLink* link) {
	return sizeof(struct NetplaySnapshotHeader) + sizeof(struct NetplayLinkState) +
	       (size_t) link->nPlayers * (link->coreStateSize + NETPLAY_MAX_SAVEDATA);
}

size_t NetplayLinkSnapshot(struct NetplayLink* link, void* out) {
	struct NetplaySnapshotHeader header = {
		.magic = SNAPSHOT_MAGIC,
		.version = SNAPSHOT_VERSION,
		.nPlayers = link->nPlayers,
		.frame = link->state.frame,
		.coreStateSize = link->coreStateSize,
	};
	uint8_t* cursor = (uint8_t*) out + sizeof(header);
	memcpy(cursor, &link->state, sizeof(link->state));
	cursor += sizeof(link->state);
	int p;
	for (p = 0; p < link->nPlayers; ++p) {
		struct mCore* core = link->cores[p];
		// Zero first so unused fields in the state struct hash identically on every machine
		memset(cursor, 0, link->coreStateSize);
		core->saveState(core, cursor);
		cursor += link->coreStateSize;

		struct GBASavedata* savedata = &_gba(link, p)->memory.savedata;
		size_t saveSize = savedata->data ? GBASavedataSize(savedata) : 0;
		if (saveSize > NETPLAY_MAX_SAVEDATA) {
			saveSize = 0;
		}
		header.saveSize[p] = saveSize;
		if (saveSize) {
			memcpy(cursor, savedata->data, saveSize);
			cursor += saveSize;
		}
	}
	memcpy(out, &header, sizeof(header));
	return cursor - (uint8_t*) out;
}

// GBAIODeserialize replays SIOCNT through GBASIOWriteSIOCNT, which keeps the pre-load status bits and
// clobbers SIOMULTI0-3 when the saved value has Busy set, so reapply the saved values verbatim
static void _restoreSerialRegisters(struct GBA* gba, const void* coreState) {
	const struct GBASerializedState* state = coreState;
	static const uint32_t regs[] = {
		GBA_REG_SIOMULTI0, GBA_REG_SIOMULTI1, GBA_REG_SIOMULTI2, GBA_REG_SIOMULTI3,
		GBA_REG_SIOCNT, GBA_REG_SIOMLT_SEND, GBA_REG_RCNT
	};
	size_t i;
	for (i = 0; i < sizeof(regs) / sizeof(*regs); ++i) {
		LOAD_16LE(gba->memory.io[regs[i] >> 1], regs[i], state->io);
	}
	gba->sio.siocnt = gba->memory.io[GBA_REG(SIOCNT)];
	gba->sio.rcnt = gba->memory.io[GBA_REG(RCNT)];
}

bool NetplayLinkRestore(struct NetplayLink* link, const void* in, size_t size) {
	struct NetplaySnapshotHeader header;
	if (size < sizeof(header) + sizeof(link->state)) {
		return false;
	}
	memcpy(&header, in, sizeof(header));
	if (header.magic != SNAPSHOT_MAGIC || header.version != SNAPSHOT_VERSION ||
	    header.nPlayers != (uint32_t) link->nPlayers || header.coreStateSize != link->coreStateSize) {
		mLOG(NETPLAY, ERROR, "Refusing to restore an incompatible snapshot");
		return false;
	}
	size_t expected = sizeof(header) + sizeof(link->state);
	int p;
	for (p = 0; p < link->nPlayers; ++p) {
		expected += link->coreStateSize + header.saveSize[p];
	}
	if (expected != size) {
		mLOG(NETPLAY, ERROR, "Snapshot size mismatch: expected %" PRIz "u, got %" PRIz "u", expected, size);
		return false;
	}

	const uint8_t* cursor = (const uint8_t*) in + sizeof(header);
	const uint8_t* linkState = cursor;
	cursor += sizeof(link->state);
	link->restoring = true;
	for (p = 0; p < link->nPlayers; ++p) {
		struct mCore* core = link->cores[p];
		mTimingDeschedule(core->timing, &link->stops[p].event);
		if (!core->loadState(core, cursor)) {
			link->restoring = false;
			return false;
		}
		_restoreSerialRegisters(_gba(link, p), cursor);
		cursor += link->coreStateSize;

		struct GBASavedata* savedata = &_gba(link, p)->memory.savedata;
		if (header.saveSize[p]) {
			if (savedata->data && GBASavedataSize(savedata) == header.saveSize[p]) {
				memcpy(savedata->data, cursor, header.saveSize[p]);
			} else {
				mLOG(NETPLAY, WARN, "Player %i save memory layout changed; cannot restore it", p);
			}
			cursor += header.saveSize[p];
		}
	}
	link->restoring = false;
	memcpy(&link->state, linkState, sizeof(link->state));
	_refreshReady(link);
	return true;
}

uint32_t NetplaySnapshotFrame(const void* snapshot, size_t size) {
	struct NetplaySnapshotHeader header;
	if (size < sizeof(header)) {
		return 0;
	}
	memcpy(&header, snapshot, sizeof(header));
	return header.frame;
}

uint32_t NetplaySnapshotHash(const void* snapshot, size_t size) {
	return doCrc32(snapshot, size);
}
