/* Copyright (c) 2026 mgba-netplay contributors
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include <mgba/feature/netplay.h>

static unsigned _inputSlot(int64_t frame) {
	return (unsigned) (frame % NETPLAY_INPUT_RING);
}

static unsigned _stateSlot(int64_t frame) {
	return (unsigned) (frame % NETPLAY_STATE_RING);
}

static bool _hasInput(const struct NetplaySession* session, int player, int64_t frame) {
	return frame >= 0 && session->inputTags[_inputSlot(frame)][player] == (uint32_t) frame + 1;
}

static uint16_t _predict(const struct NetplaySession* session, int player, int64_t frame) {
	int64_t f;
	int64_t floor = session->confirmedUntil[player];
	if (floor < frame - NETPLAY_INPUT_RING + 1) {
		floor = frame - NETPLAY_INPUT_RING + 1;
	}
	for (f = frame - 1; f >= floor && f >= 0; --f) {
		if (_hasInput(session, player, f)) {
			return session->inputs[_inputSlot(f)][player];
		}
	}
	return 0;
}

static void _storeInput(struct NetplaySession* session, int player, int64_t frame, uint16_t keys) {
	unsigned slot = _inputSlot(frame);
	session->inputs[slot][player] = keys;
	session->inputTags[slot][player] = (uint32_t) frame + 1;
	while (_hasInput(session, player, session->confirmedUntil[player] + 1)) {
		++session->confirmedUntil[player];
	}
}

bool NetplaySessionInit(struct NetplaySession* session, struct NetplayLink* link, int localPlayer, int inputDelay) {
	memset(session, 0, sizeof(*session));
	if (inputDelay < 0) {
		inputDelay = 0;
	} else if (inputDelay > NETPLAY_MAX_INPUT_DELAY) {
		inputDelay = NETPLAY_MAX_INPUT_DELAY;
	}
	session->link = link;
	session->nPlayers = link->nPlayers;
	session->localPlayer = localPlayer;
	session->inputDelay = inputDelay;
	session->frame = link->state.frame;
	session->rollbackFrom = -1;
	session->lastHashed = -1;
	session->snapshotCapacity = NetplayLinkSnapshotCapacity(link);

	int i;
	for (i = 0; i < NETPLAY_STATE_RING; ++i) {
		session->states[i] = malloc(session->snapshotCapacity);
		if (!session->states[i]) {
			NetplaySessionDeinit(session);
			return false;
		}
		session->stateFrames[i] = -1;
	}

	// Nobody can have input for the first inputDelay frames, so they are implicitly neutral
	int p;
	for (p = 0; p < session->nPlayers; ++p) {
		session->confirmedUntil[p] = (int64_t) session->frame - 1;
		int64_t f;
		for (f = session->frame; f < (int64_t) session->frame + inputDelay; ++f) {
			_storeInput(session, p, f, 0);
		}
	}
	session->localNext = session->frame + inputDelay;
	return true;
}

void NetplaySessionDeinit(struct NetplaySession* session) {
	int i;
	for (i = 0; i < NETPLAY_STATE_RING; ++i) {
		free(session->states[i]);
		session->states[i] = NULL;
	}
}

bool NetplaySessionWantsLocalInput(const struct NetplaySession* session) {
	return session->localNext <= session->frame + session->inputDelay;
}

uint32_t NetplaySessionAddLocalInput(struct NetplaySession* session, uint16_t keys) {
	uint32_t frame = session->localNext;
	_storeInput(session, session->localPlayer, frame, keys);
	++session->localNext;
	return frame;
}

bool NetplaySessionAddRemoteInput(struct NetplaySession* session, int player, uint32_t frame, uint16_t keys) {
	if (player < 0 || player >= session->nPlayers || player == session->localPlayer) {
		return false;
	}
	if ((int64_t) frame <= session->confirmedUntil[player] || _hasInput(session, player, frame)) {
		return true;
	}
	if ((int64_t) frame >= (int64_t) session->frame + NETPLAY_INPUT_RING / 2) {
		return false;
	}
	if ((int64_t) frame < (int64_t) session->frame - NETPLAY_INPUT_RING / 2) {
		return false;
	}
	_storeInput(session, player, frame, keys);
	if (frame < session->frame && session->used[_inputSlot(frame)][player] != keys) {
		if (session->rollbackFrom < 0 || (int64_t) frame < session->rollbackFrom) {
			session->rollbackFrom = frame;
		}
	}
	return true;
}

bool NetplaySessionGetInput(const struct NetplaySession* session, int player, uint32_t frame, uint16_t* keys) {
	if (!_hasInput(session, player, frame)) {
		return false;
	}
	*keys = session->inputs[_inputSlot(frame)][player];
	return true;
}

int64_t NetplaySessionConfirmedFrame(const struct NetplaySession* session) {
	int64_t confirmed = INT64_MAX;
	int p;
	for (p = 0; p < session->nPlayers; ++p) {
		if (session->confirmedUntil[p] < confirmed) {
			confirmed = session->confirmedUntil[p];
		}
	}
	return confirmed;
}

int64_t NetplaySessionPlayerConfirmed(const struct NetplaySession* session, int player) {
	return session->confirmedUntil[player];
}

bool NetplaySessionCanAdvance(const struct NetplaySession* session) {
	if (session->localNext <= session->frame) {
		return false;
	}
	return (int64_t) session->frame - NetplaySessionConfirmedFrame(session) <= NETPLAY_MAX_ROLLBACK;
}

static void _runFrame(struct NetplaySession* session, int64_t frame, bool resimulating) {
	struct NetplayLink* link = session->link;
	unsigned slot = _stateSlot(frame);
	session->stateSizes[slot] = NetplayLinkSnapshot(link, session->states[slot]);
	session->stateFrames[slot] = frame;

	uint16_t keys[NETPLAY_MAX_PLAYERS] = {0};
	unsigned inputSlot = _inputSlot(frame);
	int p;
	for (p = 0; p < session->nPlayers; ++p) {
		if (_hasInput(session, p, frame)) {
			keys[p] = session->inputs[inputSlot][p];
		} else {
			keys[p] = _predict(session, p, frame);
		}
		session->used[inputSlot][p] = keys[p];
	}

	if (session->callbacks.frameStarted) {
		session->callbacks.frameStarted(session->callbacks.context, frame, resimulating);
	}
	NetplayLinkRunFrame(link, keys);
	++session->stats.framesRun;
	if (session->callbacks.frameFinished) {
		session->callbacks.frameFinished(session->callbacks.context, frame, resimulating);
	}
}

static int _resimulate(struct NetplaySession* session, int64_t from) {
	int count = 0;
	int64_t f;
	for (f = from; f < session->frame; ++f) {
		_runFrame(session, f, true);
		++count;
	}
	return count;
}

static void _checkHashes(struct NetplaySession* session) {
	int64_t final = NetplaySessionConfirmedFrame(session) + 1;
	if (final > (int64_t) session->frame - 1) {
		final = (int64_t) session->frame - 1;
	}
	int64_t next = session->lastHashed < 0 ? 0 : session->lastHashed + NETPLAY_HASH_INTERVAL;
	while (next <= final) {
		unsigned slot = _stateSlot(next);
		if (session->stateFrames[slot] == next) {
			uint32_t hash = NetplaySnapshotHash(session->states[slot], session->stateSizes[slot]);
			if (session->callbacks.confirmedState) {
				session->callbacks.confirmedState(session->callbacks.context, next, hash, session->states[slot], session->stateSizes[slot]);
			}
		}
		session->lastHashed = next;
		next += NETPLAY_HASH_INTERVAL;
	}
}

int NetplaySessionAdvance(struct NetplaySession* session) {
	int resimulated = 0;
	if (session->rollbackFrom >= 0) {
		int64_t from = session->rollbackFrom;
		session->rollbackFrom = -1;
		unsigned slot = _stateSlot(from);
		if (session->stateFrames[slot] != from ||
		    !NetplayLinkRestore(session->link, session->states[slot], session->stateSizes[slot])) {
			mLOG(NETPLAY, ERROR, "Lost the snapshot for frame %" PRIi64 "; cannot roll back", from);
		} else {
			resimulated = _resimulate(session, from);
			++session->stats.rollbacks;
			session->stats.framesResimulated += resimulated;
			session->stats.lastRollbackDepth = resimulated;
			if ((uint32_t) resimulated > session->stats.maxRollbackDepth) {
				session->stats.maxRollbackDepth = resimulated;
			}
		}
	}
	_runFrame(session, session->frame, false);
	++session->frame;
	_checkHashes(session);
	return resimulated;
}

bool NetplaySessionLoadSnapshot(struct NetplaySession* session, const void* snapshot, size_t size) {
	int64_t frame = NetplaySnapshotFrame(snapshot, size);
	if (frame < (int64_t) session->frame - NETPLAY_INPUT_RING / 2) {
		mLOG(NETPLAY, WARN, "Resync snapshot for frame %" PRIi64 " is too old", frame);
		return false;
	}
	if (!NetplayLinkRestore(session->link, snapshot, size)) {
		return false;
	}
	session->rollbackFrom = -1;
	if (frame >= session->frame) {
		session->frame = frame;
		if (session->localNext < session->frame) {
			session->localNext = session->frame;
		}
		int p;
		for (p = 0; p < session->nPlayers; ++p) {
			if (session->confirmedUntil[p] < frame - 1) {
				session->confirmedUntil[p] = frame - 1;
			}
		}
	} else {
		_resimulate(session, frame);
	}
	return true;
}
