/* Copyright (c) 2026 mgba-netplay contributors
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "netplay/LobbyDialog.h"

#include "ConfigController.h"
#include "GBAApp.h"

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QRegularExpressionValidator>
#include <QSpinBox>
#include <QUrl>
#include <QVBoxLayout>

#include <mgba/feature/netplay.h>

using namespace QGBA;

namespace {

const QString kSettingsGroup = QStringLiteral("netplay");

QString romTitle(const QString& path) {
	QFile file(path);
	if (path.endsWith(QStringLiteral(".gba"), Qt::CaseInsensitive) && file.open(QIODevice::ReadOnly) && file.seek(0xA0)) {
		QByteArray title = file.read(12);
		title.replace('\0', ' ');
		QString text = QString::fromLatin1(title).trimmed();
		if (!text.isEmpty()) {
			return text;
		}
	}
	return QFileInfo(path).completeBaseName();
}

}

LobbyDialog::LobbyDialog(ConfigController* config, const QString& romPath, QWidget* parent)
	: QDialog(parent)
	, m_config(config)
{
	setWindowTitle(tr("Netplay lobby"));
	setAttribute(Qt::WA_DeleteOnClose);

	auto* layout = new QVBoxLayout(this);
	auto* form = new QFormLayout;
	layout->addLayout(form);

	m_server = new QLineEdit(m_config->getQtOption("server", kSettingsGroup).toString());
	if (m_server->text().isEmpty()) {
		m_server->setText(defaultServer());
	}
	m_server->setPlaceholderText(defaultServer());
	form->addRow(tr("Server:"), m_server);

	m_name = new QLineEdit(m_config->getQtOption("name", kSettingsGroup).toString());
	if (m_name->text().isEmpty()) {
		m_name->setText(qEnvironmentVariable("USERNAME", qEnvironmentVariable("USER", tr("Player"))));
	}
	m_name->setMaxLength(24);
	form->addRow(tr("Your name:"), m_name);

	auto* romRow = new QHBoxLayout;
	m_rom = new QLineEdit(romPath.isEmpty() ? m_config->getQtOption("rom", kSettingsGroup).toString() : romPath);
	auto* browse = new QPushButton(tr("Browse..."));
	connect(browse, &QPushButton::clicked, this, &LobbyDialog::browseRom);
	romRow->addWidget(m_rom);
	romRow->addWidget(browse);
	form->addRow(tr("ROM:"), romRow);

	m_writeBack = new QCheckBox(tr("Write my save back when the session ends (keeps a backup)"));
	m_writeBack->setChecked(m_config->getQtOption("writeBackSave", kSettingsGroup).isNull() || m_config->getQtOption("writeBackSave", kSettingsGroup).toBool());
	form->addRow(QString(), m_writeBack);

	auto* hostBox = new QGroupBox(tr("Host a room"));
	auto* hostLayout = new QHBoxLayout(hostBox);
	m_maxPlayers = new QSpinBox;
	m_maxPlayers->setRange(2, NETPLAY_MAX_PLAYERS);
	m_maxPlayers->setValue(std::max(2, m_config->getQtOption("maxPlayers", kSettingsGroup).toInt()));
	m_inputDelay = new QSpinBox;
	m_inputDelay->setRange(0, NETPLAY_MAX_INPUT_DELAY);
	QVariant delay = m_config->getQtOption("inputDelay", kSettingsGroup);
	m_inputDelay->setValue(delay.isNull() ? 2 : delay.toInt());
	m_inputDelay->setToolTip(tr("Frames of input delay. Higher values mean fewer rollbacks on slow connections."));
	m_create = new QPushButton(tr("Create room"));
	connect(m_create, &QPushButton::clicked, this, &LobbyDialog::createRoom);
	hostLayout->addWidget(new QLabel(tr("Players:")));
	hostLayout->addWidget(m_maxPlayers);
	hostLayout->addWidget(new QLabel(tr("Input delay:")));
	hostLayout->addWidget(m_inputDelay);
	hostLayout->addStretch();
	hostLayout->addWidget(m_create);
	layout->addWidget(hostBox);

	auto* joinBox = new QGroupBox(tr("Join a room"));
	auto* joinLayout = new QHBoxLayout(joinBox);
	m_code = new QLineEdit;
	m_code->setPlaceholderText(tr("Room code"));
	m_code->setMaxLength(6);
	m_code->setValidator(new QRegularExpressionValidator(QRegularExpression(QStringLiteral("[A-Za-z0-9]{0,6}")), m_code));
	connect(m_code, &QLineEdit::textEdited, this, [this](const QString& text) {
		int cursor = m_code->cursorPosition();
		m_code->setText(text.toUpper());
		m_code->setCursorPosition(cursor);
		updateButtons();
	});
	connect(m_code, &QLineEdit::returnPressed, this, &LobbyDialog::joinRoom);
	m_join = new QPushButton(tr("Join"));
	connect(m_join, &QPushButton::clicked, this, &LobbyDialog::joinRoom);
	joinLayout->addWidget(m_code);
	joinLayout->addWidget(m_join);
	layout->addWidget(joinBox);

	m_roomBox = new QGroupBox(tr("Room"));
	auto* roomLayout = new QVBoxLayout(m_roomBox);
	auto* codeRow = new QHBoxLayout;
	m_roomCode = new QLabel;
	QFont codeFont = m_roomCode->font();
	codeFont.setPointSize(codeFont.pointSize() * 2);
	codeFont.setBold(true);
	codeFont.setLetterSpacing(QFont::AbsoluteSpacing, 4);
	m_roomCode->setFont(codeFont);
	m_roomCode->setTextInteractionFlags(Qt::TextSelectableByMouse);
	auto* copy = new QPushButton(tr("Copy code"));
	connect(copy, &QPushButton::clicked, this, [this]() {
		QApplication::clipboard()->setText(m_roomCode->text());
		setStatus(tr("Copied the room code"));
	});
	codeRow->addWidget(m_roomCode);
	codeRow->addStretch();
	codeRow->addWidget(copy);
	roomLayout->addLayout(codeRow);
	m_roomRom = new QLabel;
	roomLayout->addWidget(m_roomRom);
	m_players = new QListWidget;
	m_players->setMaximumHeight(110);
	roomLayout->addWidget(m_players);
	auto* roomButtons = new QHBoxLayout;
	m_leave = new QPushButton(tr("Leave"));
	connect(m_leave, &QPushButton::clicked, this, &LobbyDialog::leaveRoom);
	m_start = new QPushButton(tr("Start"));
	m_start->setDefault(true);
	connect(m_start, &QPushButton::clicked, this, &LobbyDialog::startGame);
	roomButtons->addWidget(m_leave);
	roomButtons->addStretch();
	roomButtons->addWidget(m_start);
	roomLayout->addLayout(roomButtons);
	layout->addWidget(m_roomBox);

	m_status = new QLabel;
	m_status->setWordWrap(true);
	layout->addWidget(m_status);

	connect(&m_socket, &QWebSocket::connected, this, &LobbyDialog::connected);
	connect(&m_socket, &QWebSocket::disconnected, this, &LobbyDialog::disconnected);
	connect(&m_socket, &QWebSocket::textMessageReceived, this, &LobbyDialog::messageReceived);
	connect(m_rom, &QLineEdit::textChanged, this, &LobbyDialog::updateButtons);

	m_server->setObjectName("server");
	m_name->setObjectName("name");
	m_rom->setObjectName("rom");
	m_maxPlayers->setObjectName("maxPlayers");
	m_inputDelay->setObjectName("inputDelay");
	m_create->setObjectName("create");
	m_code->setObjectName("code");
	m_join->setObjectName("join");
	m_roomCode->setObjectName("roomCode");
	m_players->setObjectName("players");
	m_start->setObjectName("start");
	m_status->setObjectName("status");

	resetRoom();
	resize(520, sizeHint().height());
}

void LobbyDialog::browseRom() {
	QString path = GBAApp::app()->getOpenFileName(this, tr("Select ROM"), tr("Game Boy Advance ROMs (*.gba *.zip *.agb *.mb);;All files (*)"));
	if (!path.isEmpty()) {
		m_rom->setText(path);
	}
}

bool LobbyDialog::prepareRom() {
	QFile rom(m_rom->text());
	if (m_rom->text().isEmpty() || !rom.open(QIODevice::ReadOnly)) {
		setStatus(tr("Choose a ROM first"), true);
		return false;
	}
	QCryptographicHash hash(QCryptographicHash::Sha1);
	hash.addData(&rom);
	m_romSha1 = QString::fromLatin1(hash.result().toHex());
	return true;
}

void LobbyDialog::createRoom() {
	if (!prepareRom()) {
		return;
	}
	send(QJsonObject{
		{"type", "create_room"},
		{"name", m_name->text().trimmed()},
		{"rom_sha1", m_romSha1},
		{"rom_title", romTitle(m_rom->text())},
		{"max_players", m_maxPlayers->value()},
	});
}

void LobbyDialog::joinRoom() {
	if (m_code->text().size() != 6) {
		setStatus(tr("Room codes have 6 characters"), true);
		return;
	}
	if (!prepareRom()) {
		return;
	}
	send(QJsonObject{
		{"type", "join_room"},
		{"code", m_code->text()},
		{"name", m_name->text().trimmed()},
		{"rom_sha1", m_romSha1},
	});
}

void LobbyDialog::startGame() {
	send(QJsonObject{{"type", "start"}, {"input_delay", m_inputDelay->value()}});
}

void LobbyDialog::leaveRoom() {
	send(QJsonObject{{"type", "leave"}});
	resetRoom();
}

void LobbyDialog::send(const QJsonObject& message) {
	saveSettings();
	if (m_socket.state() == QAbstractSocket::ConnectedState) {
		m_socket.sendTextMessage(QString::fromUtf8(QJsonDocument(message).toJson(QJsonDocument::Compact)));
		return;
	}
	QUrl url(m_server->text().trimmed());
	if (!url.isValid() || (url.scheme() != "ws" && url.scheme() != "wss")) {
		setStatus(tr("The server address should look like %1").arg(defaultServer()), true);
		return;
	}
	m_pending = message;
	setStatus(tr("Connecting to %1...").arg(url.host()));
	if (m_socket.state() == QAbstractSocket::UnconnectedState) {
		m_socket.open(url);
	}
}

void LobbyDialog::connected() {
	setStatus(tr("Connected"));
	if (!m_pending.isEmpty()) {
		QJsonObject message = m_pending;
		m_pending = QJsonObject();
		send(message);
	}
}

void LobbyDialog::disconnected() {
	if (!m_pending.isEmpty() || m_inRoom) {
		setStatus(tr("Lost the connection to the lobby server: %1").arg(m_socket.errorString()), true);
	}
	m_pending = QJsonObject();
	resetRoom();
}

void LobbyDialog::messageReceived(const QString& text) {
	QJsonObject message = QJsonDocument::fromJson(text.toUtf8()).object();
	QString type = message.value("type").toString();
	if (type == "room_state") {
		showRoom(message);
	} else if (type == "started") {
		NetplaySessionParams params;
		params.romPath = m_rom->text();
		params.savePath = savePathFor(params.romPath);
		params.relayHost = QUrl(m_server->text().trimmed()).host();
		params.relayPort = message.value("relay_port").toInt();
		params.token = static_cast<quint64>(message.value("token").toDouble());
		params.slot = message.value("slot").toInt();
		params.players = message.value("players").toInt();
		params.inputDelay = message.value("input_delay").toInt();
		params.rtcMs = static_cast<qint64>(message.value("rtc_ms").toDouble());
		for (const QJsonValue& name : message.value("names").toArray()) {
			params.names.append(name.toString());
		}
		params.writeBackSave = m_writeBack->isChecked();
		if (params.players < 2 || params.players > NETPLAY_MAX_PLAYERS || params.slot < 0 || params.slot >= params.players) {
			setStatus(tr("The server sent an invalid session"), true);
			return;
		}
		m_inRoom = false;
		m_socket.close();
		emit sessionReady(params);
		accept();
	} else if (type == "room_closed") {
		resetRoom();
		setStatus(message.value("reason").toString(), true);
	} else if (type == "error") {
		setStatus(message.value("message").toString(), true);
	}
}

void LobbyDialog::showRoom(const QJsonObject& state) {
	m_inRoom = true;
	m_isHost = state.value("you").toInt() == state.value("host").toInt();
	m_roomCode->setText(state.value("code").toString());
	m_roomRom->setText(tr("Game: %1").arg(state.value("rom_title").toString()));
	m_players->clear();
	QJsonArray players = state.value("players").toArray();
	m_playerCount = players.size();
	for (const QJsonValue& value : players) {
		QJsonObject player = value.toObject();
		QString label = player.value("name").toString();
		if (player.value("seat").toInt() == state.value("host").toInt()) {
			label += tr(" (host)");
		}
		if (player.value("seat").toInt() == state.value("you").toInt()) {
			label += tr(" (you)");
		}
		m_players->addItem(label);
	}
	int maxPlayers = state.value("max_players").toInt();
	if (m_isHost) {
		setStatus(m_playerCount < 2 ? tr("Share the room code with the other players") : tr("Press Start when everyone is here"));
	} else {
		setStatus(tr("Waiting for the host to start (%1 of %2 players)").arg(m_playerCount).arg(maxPlayers));
	}
	updateButtons();
}

void LobbyDialog::resetRoom() {
	m_inRoom = false;
	m_isHost = false;
	m_playerCount = 0;
	m_roomCode->clear();
	m_players->clear();
	updateButtons();
}

void LobbyDialog::updateButtons() {
	bool idle = !m_inRoom;
	m_create->setEnabled(idle);
	m_join->setEnabled(idle && m_code->text().size() == 6);
	m_code->setEnabled(idle);
	m_rom->setEnabled(idle);
	m_server->setEnabled(idle);
	m_name->setEnabled(idle);
	m_roomBox->setVisible(m_inRoom);
	m_start->setVisible(m_isHost);
	m_start->setEnabled(m_isHost && m_playerCount >= 2);
	m_inputDelay->setEnabled(idle || m_isHost);
}

void LobbyDialog::setStatus(const QString& status, bool error) {
	m_status->setText(status);
	m_status->setStyleSheet(error ? QStringLiteral("color: #c0392b") : QString());
}

void LobbyDialog::saveSettings() {
	m_config->setQtOption("server", m_server->text().trimmed(), kSettingsGroup);
	m_config->setQtOption("name", m_name->text().trimmed(), kSettingsGroup);
	m_config->setQtOption("rom", m_rom->text(), kSettingsGroup);
	m_config->setQtOption("maxPlayers", m_maxPlayers->value(), kSettingsGroup);
	m_config->setQtOption("inputDelay", m_inputDelay->value(), kSettingsGroup);
	m_config->setQtOption("writeBackSave", m_writeBack->isChecked(), kSettingsGroup);
}

QString LobbyDialog::savePathFor(const QString& romPath) const {
	QFileInfo rom(romPath);
	QString dir = m_config->getOption("savegamePath");
	if (dir.isEmpty() || !QDir(dir).exists()) {
		dir = rom.absolutePath();
	}
	return QDir(dir).filePath(rom.completeBaseName() + QStringLiteral(".sav"));
}
