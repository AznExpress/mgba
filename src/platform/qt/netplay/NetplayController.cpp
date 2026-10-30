/* Copyright (c) 2026 mgba-netplay contributors
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "netplay/NetplayController.h"

#include "LogController.h"

#include <QFile>
#include <QFileInfo>
#include <QVector>

#include <chrono>
#include <memory>
#include <vector>

#include <mgba/core/config.h>
#include <mgba/core/core.h>
#include <mgba/feature/netplay.h>
#include <mgba/internal/gba/savedata.h>
#include <mgba-util/crc32.h>
#include <mgba-util/vfs.h>

#ifdef _WIN32
#include <windows.h>
#include <timeapi.h>
#endif

using namespace QGBA;

namespace {

constexpr double FRAME_MS = 1000.0 * NETPLAY_FRAME_CYCLES / (16 * 1024 * 1024);
constexpr uint64_t HANDSHAKE_TIMEOUT_MS = 60000;
constexpr uint64_t STATS_INTERVAL_MS = 250;
constexpr size_t AUDIO_OUT_FRAMES = 0x4000;

void sleepMs(int ms) {
	std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

}

struct NetplayController::Machine {
	int n = 0;
	mCore* cores[NETPLAY_MAX_PLAYERS] = {};
	std::vector<mColor> video[NETPLAY_MAX_PLAYERS];
	unsigned width = 0;
	unsigned height = 0;
	NetplayLink link;
	NetplaySession session;
	NetplayController* controller = nullptr;
	bool linked = false;
	bool sessionInit = false;

	~Machine() {
		if (sessionInit) {
			NetplaySessionDeinit(&session);
		}
		if (linked) {
			NetplayLinkDeinit(&link);
		}
		for (int p = 0; p < n; ++p) {
			if (cores[p]) {
				cores[p]->deinit(cores[p]);
			}
		}
	}
};

NetplayController::NetplayController(const NetplaySessionParams& params, QObject* parent)
	: QObject(parent)
	, m_params(params)
{
	mAudioBufferInit(&m_audioOut, AUDIO_OUT_FRAMES, 2);
	mAudioResamplerInit(&m_resampler, mINTERPOLATOR_SINC);
}

NetplayController::~NetplayController() {
	stop();
	mAudioResamplerDeinit(&m_resampler);
	mAudioBufferDeinit(&m_audioOut);
}

QString NetplayController::start() {
	if (m_thread.joinable()) {
		return tr("The session is already running");
	}
	QFile rom(m_params.romPath);
	if (!rom.open(QIODevice::ReadOnly)) {
		return tr("Could not open the ROM %1").arg(m_params.romPath);
	}
	QByteArray romData = rom.readAll();
	m_romCrc = doCrc32(romData.constData(), romData.size());

	m_localSave.clear();
	QFile save(m_params.savePath);
	if (!m_params.savePath.isEmpty() && save.exists()) {
		if (!save.open(QIODevice::ReadOnly)) {
			return tr("Could not read your save file %1").arg(m_params.savePath);
		}
		// Saves from games with a clock (Pokémon Ruby/Sapphire/Emerald) carry mGBA's RTC footer after the data
		const qint64 maxSave = NETPLAY_MAX_SAVEDATA + sizeof(GBASavedataRTCBuffer);
		m_localSave = save.read(maxSave + 1);
		if (m_localSave.size() > maxSave) {
			return tr("Your save file is too large to be a GBA save");
		}
	}

	m_stop = false;
	m_thread = std::thread([this]() { run(); });
	return {};
}

void NetplayController::stop() {
	m_stop = true;
	if (m_thread.joinable()) {
		m_thread.join();
	}
}

size_t NetplayController::readAudio(int16_t* samples, size_t maxFrames) {
	QMutexLocker locker(&m_audioMutex);
	size_t available = std::min<size_t>(mAudioBufferAvailable(&m_audioOut), maxFrames);
	return mAudioBufferRead(&m_audioOut, samples, available);
}

size_t NetplayController::audioAvailable() {
	QMutexLocker locker(&m_audioMutex);
	return mAudioBufferAvailable(&m_audioOut);
}

QImage NetplayController::frame() {
	QMutexLocker locker(&m_frameMutex);
	return m_frame;
}

NetplayStats NetplayController::stats() {
	QMutexLocker locker(&m_statsMutex);
	return m_stats;
}

void NetplayController::run() {
	// Without a logger, every core message (including debug-level SIO traffic) is formatted to stdout
	LogController::installGlobalLogger();
#ifdef _WIN32
	// Frame pacing relies on 1 ms sleeps
	timeBeginPeriod(1);
#endif
	QString error = runSession();
#ifdef _WIN32
	timeEndPeriod(1);
#endif
	{
		QMutexLocker locker(&m_statsMutex);
		m_stats.running = false;
	}
	if (error.startsWith('!')) {
		emit finished(error.mid(1), false);
	} else {
		emit finished(error.isEmpty() ? tr("The session ended") : error, !error.isEmpty());
	}
}

QString NetplayController::handshake(NetplayClient* client, QVector<QByteArray>& saves) {
	QVector<bool> have(m_params.players, false);
	have[m_params.slot] = true;
	saves[m_params.slot] = m_localSave;
	NetplayClientShareBlob(client, NETPLAY_BLOB_SAVE, 0xFF, 0, m_localSave.constData(), m_localSave.size());

	emit statusChanged(tr("Waiting for the other players to connect..."));
	uint64_t start = NetplayNowMs();
	bool announcedSaves = false;
	while (!m_stop) {
		uint64_t now = NetplayNowMs();
		if (now - start > HANDSHAKE_TIMEOUT_MS) {
			return tr("Timed out waiting for the other players. If this keeps happening, check that UDP port %1 on the server is reachable.")
			    .arg(m_params.relayPort);
		}
		NetplayClientPoll(client, now);
		NetplayClientFlush(client, now);
		int mismatched;
		if (NetplayClientRomMismatch(client, &mismatched)) {
			return tr("%1 has a different ROM").arg(m_params.names.value(mismatched, tr("Player %1").arg(mismatched + 1)));
		}
		bool seen = NetplayClientAllPeersSeen(client);
		if (seen && !announcedSaves) {
			announcedSaves = true;
			emit statusChanged(tr("Exchanging save files..."));
		}
		bool ready = seen && NetplayClientBlobSent(client);
		for (int p = 0; p < m_params.players; ++p) {
			void* data;
			size_t size;
			if (!have[p] && NetplayClientTakeBlob(client, p, NETPLAY_BLOB_SAVE, &data, &size, nullptr)) {
				saves[p] = QByteArray(static_cast<const char*>(data), size);
				free(data);
				have[p] = true;
			}
			ready = ready && have[p];
		}
		if (ready) {
			return {};
		}
		sleepMs(1);
	}
	return QStringLiteral("!") + tr("Cancelled");
}

QString NetplayController::runSession() {
	QByteArray host = m_params.relayHost.toUtf8();
	const char* hosts[] = { host.constData() };
	uint16_t ports[] = { m_params.relayPort };
	NetplayTransport transport;
	if (!NetplayUDPTransportCreate(&transport, 0, 1, hosts, ports)) {
		return tr("Could not open a UDP connection to %1:%2").arg(m_params.relayHost).arg(m_params.relayPort);
	}
	std::unique_ptr<NetplayClient> client(new NetplayClient);
	NetplayClientInit(client.get(), &transport, m_params.token, m_params.slot, m_params.players, m_romCrc);
	m_client = client.get();

	QVector<QByteArray> saves(m_params.players);
	QString error = handshake(client.get(), saves);
	std::unique_ptr<Machine> machine;
	if (error.isEmpty()) {
		emit statusChanged(tr("Starting..."));
		machine.reset(new Machine);
		machine->controller = this;
		machine->n = m_params.players;
		QByteArray romPath = m_params.romPath.toUtf8();
		for (int p = 0; p < machine->n && error.isEmpty(); ++p) {
			mCore* core = mCoreFind(romPath.constData());
			if (!core || !core->init(core)) {
				error = tr("Could not create an emulator core for this ROM");
				break;
			}
			machine->cores[p] = core;
			mCoreInitConfig(core, nullptr);
			NetplayPrepareCore(core, m_params.rtcMs);
			core->baseVideoSize(core, &machine->width, &machine->height);
			machine->video[p].resize(machine->width * machine->height);
			core->setVideoBuffer(core, machine->video[p].data(), machine->width);
			if (!mCoreLoadFile(core, romPath.constData())) {
				error = tr("Could not load the ROM");
				break;
			}
			const QByteArray& save = saves[p];
			core->loadSave(core, VFileMemChunk(save.isEmpty() ? nullptr : save.constData(), save.size()));
			core->reset(core);
		}
		if (error.isEmpty() && machine->cores[0]->platform(machine->cores[0]) != mPLATFORM_GBA) {
			error = tr("Netplay only supports Game Boy Advance games");
		}
	}

	if (error.isEmpty()) {
		NetplayLinkInit(&machine->link, machine->cores, machine->n);
		machine->linked = true;
		if (!NetplaySessionInit(&machine->session, &machine->link, m_params.slot, m_params.inputDelay)) {
			error = tr("Could not allocate the rollback buffers");
		} else {
			machine->sessionInit = true;
		}
	}

	if (error.isEmpty()) {
		NetplaySession* session = &machine->session;
		session->callbacks.context = machine.get();
		session->callbacks.frameFinished = &NetplayController::frameFinished;
		session->callbacks.confirmedState = &NetplayController::confirmedState;
		NetplayClientAttachSession(client.get(), session);
		m_machine = machine.get();

		mCore* local = machine->cores[m_params.slot];
		{
			QMutexLocker locker(&m_audioMutex);
			mAudioBufferClear(&m_audioOut);
			mAudioResamplerSetSource(&m_resampler, local->getAudioBuffer(local), local->audioSampleRate(local), true);
			mAudioResamplerSetDestination(&m_resampler, &m_audioOut, m_audioRate);
		}
		{
			QMutexLocker locker(&m_statsMutex);
			m_stats = NetplayStats();
			m_stats.running = true;
		}
		emit sessionStarted();
		emit statusChanged(QString());

		uint64_t stalls = 0;
		double nextTick = NetplayNowMs();
		uint64_t lastStats = 0;
		double emulationMs = 0;
		while (!m_stop) {
			uint64_t now = NetplayNowMs();
			if (now < nextTick) {
				NetplayClientPoll(client.get(), now);
				NetplayClientFlush(client.get(), now);
				sleepMs(1);
				continue;
			}
			// Don't try to catch up after a long hitch (e.g. the window being dragged)
			if (now - nextTick > 250) {
				nextTick = now;
			}
			nextTick += FRAME_MS;
			NetplayClientPoll(client.get(), now);

			int gone = -1;
			for (int p = 0; p < m_params.players; ++p) {
				if (p != m_params.slot && client->peers[p].gone) {
					gone = p;
				}
			}
			if (gone >= 0) {
				error = tr("%1 disconnected").arg(m_params.names.value(gone, tr("Player %1").arg(gone + 1)));
				break;
			}

			if (!NetplayClientShouldStall(client.get(), now)) {
				if (NetplaySessionWantsLocalInput(session)) {
					NetplaySessionAddLocalInput(session, m_keys.load());
				}
				if (NetplaySessionCanAdvance(session)) {
					auto begin = std::chrono::steady_clock::now();
					NetplaySessionAdvance(session);
					double elapsed = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();
					emulationMs = emulationMs * 0.9 + elapsed * 0.1;
					publishFrame(machine.get());
				} else {
					++stalls;
				}
			}
			NetplayClientFlush(client.get(), now);

			if (now - lastStats >= STATS_INTERVAL_MS) {
				lastStats = now;
				QMutexLocker locker(&m_statsMutex);
				m_stats.frame = session->frame;
				m_stats.rttMs = NetplayClientMaxRtt(client.get());
				m_stats.advantage = NetplayClientFrameAdvantage(client.get(), now);
				m_stats.rollbacks = session->stats.rollbacks;
				m_stats.lastRollbackDepth = session->stats.lastRollbackDepth;
				m_stats.maxRollbackDepth = session->stats.maxRollbackDepth;
				m_stats.stalls = stalls + client->stats.timeSyncStalls;
				m_stats.desyncs = client->stats.desyncs;
				m_stats.resyncs = client->stats.resyncs;
				m_stats.resyncing = client->awaitingResync || client->desynced;
				m_stats.emulationMs = emulationMs;
				int sioMode = -1;
				int sioReady = 0;
				uint64_t sioTransfers = 0;
				NetplayLinkSioInfo(&machine->link, m_params.slot, &sioMode, &sioReady, &sioTransfers);
				m_stats.sioMode = sioMode;
				m_stats.sioReady = sioReady;
				m_stats.sioTransfers = sioTransfers;
			}
		}
		NetplayClientSendBye(client.get(), NetplayNowMs());
		m_machine = nullptr;

		QString saveMessage = writeBackSave(machine.get());
		if (error.isEmpty() && !saveMessage.isEmpty()) {
			error = QStringLiteral("!") + saveMessage;
		} else if (!saveMessage.isEmpty()) {
			error += QStringLiteral("\n") + saveMessage;
		}
	}

	m_client = nullptr;
	NetplayClientDeinit(client.get());
	machine.reset();
	transport.deinit(&transport);
	return error;
}

void NetplayController::publishFrame(Machine* machine) {
	const mColor* video = machine->video[m_params.slot].data();
	// Same conversion DisplayQt uses: native pixels are XBGR, not RGBX
	QImage image(reinterpret_cast<const uchar*>(video), machine->width, machine->height,
	             machine->width * sizeof(mColor), QImage::Format_ARGB32);
	image = image.convertToFormat(QImage::Format_RGB32).rgbSwapped();
	{
		QMutexLocker locker(&m_frameMutex);
		m_frame = image;
	}
	emit frameAvailable();
}

QString NetplayController::writeBackSave(Machine* machine) {
	if (!m_params.writeBackSave || m_params.savePath.isEmpty()) {
		return {};
	}
	mCore* local = machine->cores[m_params.slot];
	void* sram = nullptr;
	size_t size = local->savedataClone(local, &sram);
	QByteArray data(static_cast<const char*>(sram), size);
	free(sram);
	if (!size || data == m_localSave.left(size)) {
		return {};
	}
	// The clone holds only the save data; keep the file's RTC footer, if it had one
	data += m_localSave.mid(size);
	QString backup = m_params.savePath + QStringLiteral(".netplay-backup");
	if (QFile::exists(m_params.savePath)) {
		QFile::remove(backup);
		if (!QFile::copy(m_params.savePath, backup)) {
			return tr("Your save changed, but it was not written because %1 could not be backed up").arg(m_params.savePath);
		}
	}
	QFile out(m_params.savePath);
	if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate) || out.write(data) != data.size()) {
		return tr("Could not write your save to %1").arg(m_params.savePath);
	}
	return tr("Your save was updated (the previous one is in %1)").arg(QFileInfo(backup).fileName());
}

void NetplayController::frameFinished(void* context, uint32_t, bool resimulating) {
	Machine* machine = static_cast<Machine*>(context);
	NetplayController* self = machine->controller;
	if (!resimulating) {
		QMutexLocker locker(&self->m_audioMutex);
		mAudioResamplerProcess(&self->m_resampler);
	}
	// Re-simulated frames were already heard with the predicted inputs, and other players' audio is never played
	for (int p = 0; p < machine->n; ++p) {
		mAudioBufferClear(machine->cores[p]->getAudioBuffer(machine->cores[p]));
	}
}

void NetplayController::confirmedState(void* context, uint32_t frame, uint32_t hash, const void* snapshot, size_t size) {
	Machine* machine = static_cast<Machine*>(context);
	NetplayClientOnConfirmedState(machine->controller->m_client, frame, hash, snapshot, size);
}
