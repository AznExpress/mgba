/* Copyright (c) 2026 mgba-netplay contributors
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#pragma once

#include "netplay/NetplayController.h"

#include <QTimer>
#include <QWidget>

#include <memory>

class QAudioSink;
class QIODevice;

namespace QGBA {

class InputController;

// Shows the local player's screen during a netplay session, plays its audio and feeds it input
class NetplayView : public QWidget {
Q_OBJECT

public:
	NetplayView(const NetplaySessionParams& params, InputController* input, QWidget* parent = nullptr);
	~NetplayView();

	// Returns an error message if the session couldn't start
	QString start();
	void stopSession();
	NetplayStats stats();
	bool hasFinished() const;

protected:
	void paintEvent(QPaintEvent*) override;
	void keyPressEvent(QKeyEvent*) override;
	void keyReleaseEvent(QKeyEvent*) override;
	void focusOutEvent(QFocusEvent*) override;
	void closeEvent(QCloseEvent*) override;

private slots:
	void pollInput();
	void pumpAudio();
	void sessionFinished(const QString& message, bool error);

private:
	void setupAudio();
	void drawStats(QPainter&);

	NetplayController m_controller;
	InputController* m_input;
	QTimer m_inputTimer;
	QTimer m_audioTimer;
	QTimer m_statsTimer;
	quint16 m_keyboard = 0;
	QString m_status;
	bool m_showStats = true;
	bool m_finished = false;
	bool m_finishedWithError = false;
	QAudioSink* m_sink = nullptr;
	QIODevice* m_audioDevice = nullptr;
};

}
