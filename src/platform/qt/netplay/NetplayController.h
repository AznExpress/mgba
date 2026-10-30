/* Copyright (c) 2026 mgba-netplay contributors
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#pragma once

#include <QByteArray>
#include <QImage>
#include <QMutex>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVector>

#include <atomic>
#include <thread>

#include <mgba-util/audio-buffer.h>
#include <mgba-util/audio-resampler.h>

struct NetplayClient;

namespace QGBA {

struct NetplaySessionParams {
	QString romPath;
	QString savePath;
	QString relayHost;
	quint16 relayPort = 0;
	quint64 token = 0;
	int slot = 0;
	int players = 2;
	int inputDelay = 2;
	qint64 rtcMs = 0;
	QStringList names;
	bool writeBackSave = true;
};

struct NetplayStats {
	bool running = false;
	quint32 frame = 0;
	unsigned rttMs = 0;
	float advantage = 0;
	quint64 rollbacks = 0;
	unsigned lastRollbackDepth = 0;
	unsigned maxRollbackDepth = 0;
	quint64 stalls = 0;
	unsigned desyncs = 0;
	unsigned resyncs = 0;
	bool resyncing = false;
	double emulationMs = 0;
};

// Runs a netplay session on a worker thread: all players' cores, the rollback session and the network
// client. Only the local player's video and audio are exposed.
class NetplayController : public QObject {
Q_OBJECT

public:
	NetplayController(const NetplaySessionParams& params, QObject* parent = nullptr);
	~NetplayController();

	// Reads the ROM and local save; returns an error message, or an empty string once the session thread starts
	QString start();
	void stop();

	const NetplaySessionParams& params() const { return m_params; }

	void setKeys(quint16 keys) { m_keys = keys; }
	void setAudioRate(int rate) { m_audioRate = rate; }
	// Reads up to maxFrames interleaved stereo frames; returns the number read
	size_t readAudio(int16_t* samples, size_t maxFrames);
	size_t audioAvailable();

	QImage frame();
	NetplayStats stats();

signals:
	void statusChanged(const QString& status);
	void sessionStarted();
	void frameAvailable();
	void finished(const QString& message, bool error);

private:
	struct Machine;

	void run();
	QString runSession();
	QString handshake(::NetplayClient*, QVector<QByteArray>& saves);
	void publishFrame(Machine*);
	QString writeBackSave(Machine*);

	static void frameFinished(void* context, uint32_t frame, bool resimulating);
	static void confirmedState(void* context, uint32_t frame, uint32_t hash, const void* snapshot, size_t size);

	NetplaySessionParams m_params;
	QByteArray m_localSave;
	quint32 m_romCrc = 0;

	std::thread m_thread;
	std::atomic<bool> m_stop{false};
	std::atomic<quint16> m_keys{0};
	std::atomic<int> m_audioRate{48000};

	Machine* m_machine = nullptr;
	::NetplayClient* m_client = nullptr;

	QMutex m_frameMutex;
	QImage m_frame;

	QMutex m_audioMutex;
	mAudioBuffer m_audioOut;
	mAudioResampler m_resampler;

	QMutex m_statsMutex;
	NetplayStats m_stats;
};

}
