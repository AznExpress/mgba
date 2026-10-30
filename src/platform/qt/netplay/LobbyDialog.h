/* Copyright (c) 2026 mgba-netplay contributors
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#pragma once

#include "netplay/NetplayController.h"

#include <QDialog>
#include <QJsonObject>
#include <QWebSocket>

class QCheckBox;
class QGroupBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QSpinBox;

namespace QGBA {

class ConfigController;

// Creates or joins a room on the lobby server. Emits sessionReady once the host starts the game.
class LobbyDialog : public QDialog {
Q_OBJECT

public:
	LobbyDialog(ConfigController* config, const QString& romPath, QWidget* parent = nullptr);

	static QString defaultServer() { return QStringLiteral("ws://localhost:7777/ws"); }

signals:
	void sessionReady(const NetplaySessionParams& params);

private slots:
	void browseRom();
	void browseSave();
	void romChanged();
	void createRoom();
	void joinRoom();
	void startGame();
	void leaveRoom();
	void connected();
	void disconnected();
	void messageReceived(const QString& message);

private:
	bool prepareRom();
	void send(const QJsonObject& message);
	void setStatus(const QString& status, bool error = false);
	void showRoom(const QJsonObject& state);
	void resetRoom();
	void updateButtons();
	void saveSettings();
	QString savePathFor(const QString& romPath) const;
	void fillDefaultSave(bool force);

	ConfigController* m_config;
	QWebSocket m_socket;
	QJsonObject m_pending;
	bool m_inRoom = false;
	bool m_isHost = false;
	int m_playerCount = 0;
	QString m_romSha1;

	QLineEdit* m_server;
	QLineEdit* m_name;
	QLineEdit* m_rom;
	QLineEdit* m_save;
	QString m_autoSavePath;
	QSpinBox* m_maxPlayers;
	QSpinBox* m_inputDelay;
	QCheckBox* m_writeBack;
	QPushButton* m_create;
	QLineEdit* m_code;
	QPushButton* m_join;
	QGroupBox* m_roomBox;
	QLabel* m_roomCode;
	QLabel* m_roomRom;
	QListWidget* m_players;
	QPushButton* m_start;
	QPushButton* m_leave;
	QLabel* m_status;
};

}
