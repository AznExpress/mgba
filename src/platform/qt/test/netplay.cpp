/* Copyright (c) 2026 mgba-netplay contributors
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
// End-to-end test of the netplay UI against a running lobby server, using Qt's offscreen platform:
// several LobbyDialogs create and join a room, the host starts, and the NetplayViews play for a while
// with scripted key presses. Fails on desyncs, stalls or missing frames.
//
//     mgba-qt-netplay-test <ws://server:port/ws> <rom> [players] [seconds] [screenshot dir]
#include "ConfigController.h"
#include "InputController.h"
#include "LogController.h"
#include "netplay/LobbyDialog.h"
#include "netplay/NetplayView.h"

#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QSpinBox>
#include <QThread>

#include <cstdio>
#include <functional>
#include <vector>

using namespace QGBA;

static bool waitFor(const std::function<bool()>& condition, int timeoutMs) {
	QElapsedTimer timer;
	timer.start();
	while (!condition()) {
		if (timer.elapsed() > timeoutMs) {
			return false;
		}
		QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
		QThread::msleep(2);
	}
	return true;
}

template<typename T>
static T* child(QWidget* parent, const char* name) {
	T* widget = parent->findChild<T*>(name);
	if (!widget) {
		fprintf(stderr, "missing widget %s\n", name);
		exit(2);
	}
	return widget;
}

static void sendKey(QWidget* target, int key, bool down) {
	QKeyEvent event(down ? QEvent::KeyPress : QEvent::KeyRelease, key, Qt::NoModifier);
	QCoreApplication::sendEvent(target, &event);
}

int main(int argc, char* argv[]) {
	qputenv("QT_QPA_PLATFORM", "offscreen");
	QApplication app(argc, argv);
	if (argc < 3) {
		fprintf(stderr, "usage: %s <ws://server:port/ws> <rom> [players] [seconds] [screenshot dir]\n", argv[0]);
		return 2;
	}
	QString server = QString::fromLocal8Bit(argv[1]);
	QString rom = QString::fromLocal8Bit(argv[2]);
	int players = argc > 3 ? atoi(argv[3]) : 2;
	int seconds = argc > 4 ? atoi(argv[4]) : 15;
	QString shots = argc > 5 ? QString::fromLocal8Bit(argv[5]) : QString();

	LogController::global()->setLevels(mLOG_WARN | mLOG_ERROR | mLOG_FATAL);
	ConfigController config;
	InputController input;
	input.setConfiguration(&config);

	std::vector<LobbyDialog*> lobbies;
	std::vector<NetplayView*> views(players, nullptr);
	for (int p = 0; p < players; ++p) {
		LobbyDialog* lobby = new LobbyDialog(&config, rom);
		lobby->setAttribute(Qt::WA_DeleteOnClose, false);
		child<QLineEdit>(lobby, "server")->setText(server);
		child<QLineEdit>(lobby, "name")->setText(QString("Tester %1").arg(p + 1));
		QObject::connect(lobby, &LobbyDialog::sessionReady, [&views, &input](const NetplaySessionParams& params) {
			NetplaySessionParams local = params;
			local.writeBackSave = false;
			local.savePath.clear();
			NetplayView* view = new NetplayView(local, &input);
			view->show();
			QString error = view->start();
			if (!error.isEmpty()) {
				fprintf(stderr, "player %i could not start: %s\n", params.slot, qPrintable(error));
				exit(1);
			}
			views[params.slot] = view;
		});
		lobby->show();
		lobbies.push_back(lobby);
	}

	LobbyDialog* host = lobbies[0];
	child<QSpinBox>(host, "maxPlayers")->setValue(players);
	child<QPushButton>(host, "create")->click();
	QLabel* codeLabel = child<QLabel>(host, "roomCode");
	if (!waitFor([&]() { return !codeLabel->text().isEmpty(); }, 5000)) {
		fprintf(stderr, "host never got a room: %s\n", qPrintable(child<QLabel>(host, "status")->text()));
		return 1;
	}
	QString code = codeLabel->text();
	printf("room %s created\n", qPrintable(code));
	for (int p = 1; p < players; ++p) {
		QLineEdit* codeEdit = child<QLineEdit>(lobbies[p], "code");
		codeEdit->setText(code.toLower());
		emit codeEdit->textEdited(codeEdit->text());
		child<QPushButton>(lobbies[p], "join")->click();
	}
	QListWidget* list = child<QListWidget>(host, "players");
	if (!waitFor([&]() { return list->count() == players; }, 5000)) {
		fprintf(stderr, "only %i players joined\n", list->count());
		return 1;
	}
	QPushButton* start = child<QPushButton>(host, "start");
	if (!start->isEnabled()) {
		fprintf(stderr, "Start is disabled for the host\n");
		return 1;
	}
	start->click();
	if (!waitFor([&]() {
		for (NetplayView* view : views) {
			if (!view || !view->stats().running) {
				return false;
			}
		}
		return true;
	}, 30000)) {
		fprintf(stderr, "the session never started on every client\n");
		return 1;
	}
	printf("session running with %i players\n", players);

	// Scripted input: each player taps a different pattern, and everyone toggles SELECT (link mode) at times
	static const int keys[] = { Qt::Key_Up, Qt::Key_Right, Qt::Key_Down, Qt::Key_Left, Qt::Key_X, Qt::Key_Z };
	QElapsedTimer timer;
	timer.start();
	int step = 0;
	while (timer.elapsed() < seconds * 1000) {
		for (int p = 0; p < players; ++p) {
			int key = keys[(step + p * 2) % 6];
			sendKey(views[p], key, (step + p) % 2 == 0);
			sendKey(views[p], Qt::Key_Backspace, (step / 40) % 3 == 1);
		}
		++step;
		waitFor([]() { return false; }, 70 + (step % 5) * 13);
		for (NetplayView* view : views) {
			if (view->hasFinished()) {
				fprintf(stderr, "a session ended early\n");
				return 1;
			}
		}
	}

	int failures = 0;
	double expectedFrames = seconds * 59.7275;
	for (int p = 0; p < players; ++p) {
		NetplayStats stats = views[p]->stats();
		printf("player %i: frame %u, ping %u ms, rollbacks %llu (max depth %u), waits %llu, desyncs %u, emulation %.2f ms/frame\n",
		       p, stats.frame, stats.rttMs, (unsigned long long) stats.rollbacks, stats.maxRollbackDepth,
		       (unsigned long long) stats.stalls, stats.desyncs, stats.emulationMs);
		if (stats.desyncs) {
			fprintf(stderr, "player %i desynced\n", p);
			++failures;
		}
		if (stats.frame < expectedFrames * 0.85) {
			fprintf(stderr, "player %i only reached frame %u of about %.0f\n", p, stats.frame, expectedFrames);
			++failures;
		}
		if (!shots.isEmpty()) {
			QDir().mkpath(shots);
			views[p]->grab().save(QDir(shots).filePath(QString("player%1.png").arg(p + 1)));
			lobbies[p]->grab().save(QDir(shots).filePath(QString("lobby%1.png").arg(p + 1)));
		}
	}
	for (NetplayView* view : views) {
		view->stopSession();
		view->close();
		delete view;
	}
	for (LobbyDialog* lobby : lobbies) {
		delete lobby;
	}
	printf("%s\n", failures ? "FAIL" : "PASS");
	return failures ? 1 : 0;
}
