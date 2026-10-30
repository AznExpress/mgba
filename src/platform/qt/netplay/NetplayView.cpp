/* Copyright (c) 2026 mgba-netplay contributors
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "netplay/NetplayView.h"

#include "InputController.h"

#include <QCloseEvent>
#include <QFileInfo>
#include <QKeyEvent>
#include <QMessageBox>
#include <QPainter>

#if defined(BUILD_QT_MULTIMEDIA) && (QT_VERSION >= QT_VERSION_CHECK(6, 0, 0))
#define NETPLAY_AUDIO
#include <QAudioDevice>
#include <QAudioSink>
#include <QMediaDevices>
#endif

#include <mgba/feature/netplay.h>
#include <mgba/internal/gba/input.h>

using namespace QGBA;

namespace {

constexpr int AUDIO_BUFFER_MS = 60;

}

NetplayView::NetplayView(const NetplaySessionParams& params, InputController* input, QWidget* parent)
	: QWidget(parent)
	, m_controller(params)
	, m_input(input)
{
	setWindowTitle(tr("Netplay: %1 (player %2 of %3)")
	                   .arg(QFileInfo(params.romPath).completeBaseName())
	                   .arg(params.slot + 1)
	                   .arg(params.players));
	setFocusPolicy(Qt::StrongFocus);
	setAttribute(Qt::WA_OpaquePaintEvent);
	setMinimumSize(240, 160);
	resize(720, 480);

	connect(&m_controller, &NetplayController::statusChanged, this, [this](const QString& status) {
		m_status = status;
		update();
	});
	connect(&m_controller, &NetplayController::frameAvailable, this, qOverload<>(&QWidget::update));
	connect(&m_controller, &NetplayController::finished, this, &NetplayView::sessionFinished);

	m_inputTimer.setInterval(InputController::POLL_INTERVAL_MS);
	connect(&m_inputTimer, &QTimer::timeout, this, &NetplayView::pollInput);
	m_audioTimer.setInterval(5);
	connect(&m_audioTimer, &QTimer::timeout, this, &NetplayView::pumpAudio);
	m_statsTimer.setInterval(250);
	connect(&m_statsTimer, &QTimer::timeout, this, qOverload<>(&QWidget::update));
}

NetplayView::~NetplayView() {
	m_controller.stop();
}

QString NetplayView::start() {
	setupAudio();
	QString error = m_controller.start();
	if (!error.isEmpty()) {
		return error;
	}
	m_status = tr("Connecting...");
	m_inputTimer.start();
	m_audioTimer.start();
	m_statsTimer.start();
	return {};
}

void NetplayView::stopSession() {
	m_controller.stop();
}

NetplayStats NetplayView::stats() {
	return m_controller.stats();
}

bool NetplayView::hasFinished() const {
	return m_finished;
}

void NetplayView::setupAudio() {
#ifdef NETPLAY_AUDIO
	QAudioDevice device = QMediaDevices::defaultAudioOutput();
	if (device.isNull()) {
		return;
	}
	QAudioFormat format;
	format.setSampleRate(48000);
	format.setChannelCount(2);
	format.setSampleFormat(QAudioFormat::Int16);
	if (!device.isFormatSupported(format)) {
		format.setSampleRate(device.preferredFormat().sampleRate());
	}
	m_controller.setAudioRate(format.sampleRate());
	m_sink = new QAudioSink(device, format, this);
	m_sink->setBufferSize(format.bytesForDuration(AUDIO_BUFFER_MS * 1000));
	m_audioDevice = m_sink->start();
#endif
}

void NetplayView::pumpAudio() {
#ifdef NETPLAY_AUDIO
	if (!m_audioDevice || !m_sink) {
		return;
	}
	int16_t samples[2048 * 2];
	size_t room = std::min<size_t>(m_sink->bytesFree() / (2 * sizeof(int16_t)), 2048);
	// Keep latency bounded if the game produces audio faster than the device plays it
	size_t queued = m_controller.audioAvailable();
	size_t limit = m_sink->format().sampleRate() * AUDIO_BUFFER_MS * 2 / 1000;
	while (queued > limit + room) {
		size_t dropped = m_controller.readAudio(samples, std::min<size_t>(queued - limit - room, 2048));
		if (!dropped) {
			break;
		}
		queued -= dropped;
	}
	size_t frames = m_controller.readAudio(samples, room);
	if (frames) {
		m_audioDevice->write(reinterpret_cast<const char*>(samples), frames * 2 * sizeof(int16_t));
	}
#endif
}

void NetplayView::pollInput() {
	// Two windows on one PC share a controller; only the focused window should read it
	int gamepad = (m_input && hasFocus()) ? m_input->pollEvents() : 0;
	m_controller.setKeys((m_keyboard | gamepad) & 0x3FF);
}

int NetplayView::mapKey(int qtKey) const {
	// On-screen "Press A" means the GBA A button. mGBA's default keyboard map uses X for A and A for L.
	if (qtKey == Qt::Key_A) {
		return GBA_KEY_A;
	}
	if (qtKey == Qt::Key_B) {
		return GBA_KEY_B;
	}
	return m_input ? m_input->mapKeyboard(qtKey) : -1;
}

void NetplayView::keyPressEvent(QKeyEvent* event) {
	if (event->key() == Qt::Key_F3) {
		m_showStats = !m_showStats;
		update();
		return;
	}
	int key = mapKey(event->key());
	if (key < 0) {
		QWidget::keyPressEvent(event);
		return;
	}
	if (!event->isAutoRepeat()) {
		m_keyboard |= 1 << key;
		pollInput();
	}
	event->accept();
}

void NetplayView::keyReleaseEvent(QKeyEvent* event) {
	int key = mapKey(event->key());
	if (key < 0) {
		QWidget::keyReleaseEvent(event);
		return;
	}
	if (!event->isAutoRepeat()) {
		m_keyboard &= ~(1 << key);
		pollInput();
	}
	event->accept();
}

void NetplayView::focusInEvent(QFocusEvent* event) {
	QWidget::focusInEvent(event);
	update();
}

void NetplayView::focusOutEvent(QFocusEvent* event) {
	m_keyboard = 0;
	pollInput();
	QWidget::focusOutEvent(event);
	update();
}

void NetplayView::closeEvent(QCloseEvent* event) {
	if (!m_finished && m_controller.stats().running) {
		auto answer = QMessageBox::question(this, tr("Leave netplay"),
		                                    tr("Leave the session? The other players won't be able to continue without you."));
		if (answer != QMessageBox::Yes) {
			event->ignore();
			return;
		}
	}
	m_controller.stop();
	event->accept();
}

void NetplayView::sessionFinished(const QString& message, bool error) {
	m_finished = true;
	m_finishedWithError = error;
	m_inputTimer.stop();
	m_audioTimer.stop();
	m_status = message + QStringLiteral("\n\n") + tr("Close this window to return to mGBA.");
	update();
}

void NetplayView::paintEvent(QPaintEvent*) {
	QPainter painter(this);
	painter.fillRect(rect(), Qt::black);
	QImage frame = m_controller.frame();
	if (!frame.isNull()) {
		QSize size = frame.size().scaled(this->size(), Qt::KeepAspectRatio);
		int scale = std::min(width() / frame.width(), height() / frame.height());
		if (scale >= 1) {
			// Integer scaling keeps pixels even
			size = frame.size() * scale;
		}
		QRect target(QPoint((width() - size.width()) / 2, (height() - size.height()) / 2), size);
		painter.drawImage(target, frame);
	}
	if (!m_status.isEmpty()) {
		QFont font = painter.font();
		font.setPointSize(12);
		painter.setFont(font);
		QRect box = rect().adjusted(20, 20, -20, -20);
		QRect bounds = painter.boundingRect(box, Qt::AlignCenter | Qt::TextWordWrap, m_status).adjusted(-12, -8, 12, 8);
		painter.fillRect(bounds, QColor(0, 0, 0, 200));
		painter.setPen(m_finishedWithError ? QColor(255, 140, 120) : QColor(Qt::white));
		painter.drawText(box, Qt::AlignCenter | Qt::TextWordWrap, m_status);
	}
	if (m_showStats) {
		drawStats(painter);
	}
	if (!hasFocus() && !m_finished && m_controller.stats().running) {
		QString hint = tr("Click this window to control player %1\nA = GBA A, B = GBA B, X = A, Z = B")
		                   .arg(m_controller.params().slot + 1);
		QFont font = painter.font();
		font.setPointSize(11);
		painter.setFont(font);
		QRect box = rect().adjusted(16, 16, -16, -16);
		QRect bounds = painter.boundingRect(box, Qt::AlignBottom | Qt::AlignHCenter | Qt::TextWordWrap, hint).adjusted(-12, -8, 12, 8);
		painter.fillRect(bounds, QColor(0, 0, 0, 180));
		painter.setPen(QColor(255, 220, 120));
		painter.drawText(box, Qt::AlignBottom | Qt::AlignHCenter | Qt::TextWordWrap, hint);
	}
}

void NetplayView::drawStats(QPainter& painter) {
	NetplayStats stats = m_controller.stats();
	if (!stats.running) {
		return;
	}
	const NetplaySessionParams& params = m_controller.params();
	QStringList lines;
	lines << tr("Ping %1 ms   Delay %2 frames").arg(stats.rttMs).arg(params.inputDelay);
	lines << tr("Rollbacks %1 (last %2, max %3)").arg(stats.rollbacks).arg(stats.lastRollbackDepth).arg(stats.maxRollbackDepth);
	lines << tr("Frame advantage %1   Waits %2").arg(stats.advantage, 0, 'f', 1).arg(stats.stalls);
	lines << tr("Emulation %1 ms/frame").arg(stats.emulationMs, 0, 'f', 1);
	QString sio;
	switch (stats.sioMode) {
	case 0: sio = QStringLiteral("NORMAL8"); break;
	case 1: sio = QStringLiteral("NORMAL32"); break;
	case 2: sio = QStringLiteral("MULTI"); break;
	case 3: sio = QStringLiteral("UART"); break;
	case 8: sio = QStringLiteral("GPIO"); break;
	case 12: sio = QStringLiteral("JOYBUS"); break;
	default: sio = tr("off"); break;
	}
	if (stats.sioReady) {
		sio += QStringLiteral(" ready");
	}
	lines << tr("Link %1   transfers %2").arg(sio).arg(stats.sioTransfers);
	if (stats.resyncing) {
		lines << tr("Desync detected, resynchronizing...");
	} else if (stats.desyncs) {
		lines << tr("Desyncs %1, recovered %2").arg(stats.desyncs).arg(stats.resyncs);
	}
	QStringList held;
	quint16 keys = stats.keys;
	if (keys & (1 << GBA_KEY_A)) held << QStringLiteral("A");
	if (keys & (1 << GBA_KEY_B)) held << QStringLiteral("B");
	if (keys & (1 << GBA_KEY_START)) held << QStringLiteral("Start");
	if (keys & (1 << GBA_KEY_SELECT)) held << QStringLiteral("Select");
	if (keys & (1 << GBA_KEY_L)) held << QStringLiteral("L");
	if (keys & (1 << GBA_KEY_R)) held << QStringLiteral("R");
	if (keys & (1 << GBA_KEY_UP)) held << QStringLiteral("U");
	if (keys & (1 << GBA_KEY_DOWN)) held << QStringLiteral("D");
	if (keys & (1 << GBA_KEY_LEFT)) held << QStringLiteral("Lf");
	if (keys & (1 << GBA_KEY_RIGHT)) held << QStringLiteral("Rt");
	lines << tr("Held %1").arg(held.isEmpty() ? tr("none") : held.join(QStringLiteral(" ")));
	lines << tr("F3 hides this");

	QFont font = painter.font();
	font.setPointSize(9);
	painter.setFont(font);
	QString text = lines.join('\n');
	QRect bounds = painter.boundingRect(QRect(8, 8, width() - 16, height() - 16), Qt::AlignLeft | Qt::AlignTop, text);
	painter.fillRect(bounds.adjusted(-6, -4, 6, 4), QColor(0, 0, 0, 160));
	painter.setPen(stats.resyncing ? QColor(255, 180, 80) : QColor(220, 255, 220));
	painter.drawText(bounds, Qt::AlignLeft | Qt::AlignTop, text);
}
