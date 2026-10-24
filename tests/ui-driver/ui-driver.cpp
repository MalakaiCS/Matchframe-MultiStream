// SPDX-License-Identifier: GPL-2.0-or-later
// MatchFrame Multistream - Copyright (C) 2026 MatchFrame contributors
//
// TEST-ONLY OBS module. Drives the MatchFrame Multistream dock black-box, through the same
// widgets an operator clicks, inside the real OBS frontend. Configured by the JSON file in
// $MF_UITEST_CONFIG; writes one JSON line per step to the configured results file and closes
// OBS when the scenario ends. Never shipped.

#include <obs-frontend-api.h>
#include <obs-module.h>
#include <obs.hpp>

#include <QAbstractButton>
#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QDockWidget>
#include <QElapsedTimer>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QLabel>
#include <QLayout>
#include <QLineEdit>
#include <QMainWindow>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QSpinBox>
#include <QTimer>
#include <QToolButton>

#include <functional>
#include <vector>

OBS_DECLARE_MODULE()

namespace {

QString str(obs_data_t *data, const char *key)
{
	const char *value = obs_data_get_string(data, key);
	return value ? QString::fromUtf8(value) : QString();
}

class Driver : public QObject {
public:
	explicit Driver(obs_data_t *config) : config_(config)
	{
		obs_data_addref(config_);
		results_.setFileName(str(config_, "results"));
		if (!results_.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text))
			blog(LOG_WARNING, "[mf-ui-driver] cannot write results to %s",
			     results_.fileName().toUtf8().constData());
		connect(&tick_, &QTimer::timeout, this, [this]() { tick(); });
	}
	~Driver() override { obs_data_release(config_); }

	void begin()
	{
		/* Qt's own file dialogs can be driven through their widgets; native Windows ones cannot.
		 * Test-only: the plugin itself keeps the platform's native dialogs. */
		QCoreApplication::setAttribute(Qt::AA_DontUseNativeDialogs, true);
		QString scenario = str(config_, "scenario");
		emit_line("scenario", true, scenario);
		showDock();
		expect(
			"dock ready",
			[this]() {
				auto *b = find<QPushButton>("mfStartEnabled");
				return b && b->isEnabled();
			},
			20000);
		if (scenario == "crud")
			buildCrud();
		else if (scenario == "restore")
			buildRestore();
		else if (scenario == "eight")
			buildEight();
		else if (scenario == "program")
			buildProgram();
		else if (scenario == "profile_exit")
			buildProfileExit();
		else if (scenario == "no_autostart")
			buildNoAutostart();
		else
			emit_line("unknown scenario", false, scenario);
		if (!obs_data_get_bool(config_, "keepOpen"))
			act("close OBS", [this]() { closeObs(); });
		clock_.start();
		tick_.start(50);
	}

private:
	struct Step {
		QString name;
		std::function<void()> action;
		std::function<bool()> done;
		int timeoutMs;
		bool record;
	};

	/* ---- step engine ---- */

	void act(const QString &name, std::function<void()> action)
	{
		steps_.push_back({name, std::move(action), nullptr, 0, false});
	}
	void expect(const QString &name, std::function<bool()> done, int timeoutMs = 10000)
	{
		steps_.push_back({name, nullptr, std::move(done), timeoutMs, true});
	}
	/* Like expect(), but only reported if it times out. */
	void wait(const QString &name, std::function<bool()> done, int timeoutMs = 10000)
	{
		steps_.push_back({name, nullptr, std::move(done), timeoutMs, false});
	}
	void sleep(int ms)
	{
		auto until = std::make_shared<qint64>(0);
		steps_.push_back({QString("sleep %1 ms").arg(ms),
				  [this, until, ms]() { *until = clock_.elapsed() + ms; },
				  [this, until]() { return clock_.elapsed() >= *until; }, ms + 5000, false});
	}
	void mark(const QString &name)
	{
		act("mark " + name,
		    [this, name]() { emit_line("mark:" + name, true, QString::number(clock_.elapsed())); });
	}

	void tick()
	{
		if (inTick_)
			return;
		inTick_ = true;
		if (index_ >= steps_.size()) {
			tick_.stop();
			inTick_ = false;
			return;
		}
		Step &step = steps_[index_];
		if (!started_) {
			started_ = true;
			stepClock_.restart();
			if (step.action)
				step.action();
			inTick_ = false;
			return;
		}
		bool ok = !step.done || step.done();
		if (ok || stepClock_.elapsed() > step.timeoutMs) {
			if (step.record || !ok)
				emit_line(step.name, ok, ok ? QString() : detailForFailure());
			index_++;
			started_ = false;
		}
		inTick_ = false;
	}

	QString detailForFailure()
	{
		QWidget *modal = QApplication::activeModalWidget();
		QString detail = QString("timeout; modal=%1")
					 .arg(modal ? modal->objectName() + "/" + modal->metaObject()->className()
						    : QString("none"));
		/* Card states and (already sanitized) details, so a failure explains itself. */
		for (QWidget *c : cards()) {
			auto text = [c](const char *child) {
				auto *label = c->findChild<QLabel *>(child);
				return label ? label->text().simplified().left(160) : QString();
			};
			detail += QString("; %1: %2 [%3]")
					  .arg(text("mfCardName"), text("mfCardState"), text("mfCardDetail"));
		}
		return detail;
	}

	void emit_line(const QString &step, bool ok, const QString &detail)
	{
		QString line = QString("{\"t\":%1,\"step\":\"%2\",\"ok\":%3,\"detail\":\"%4\"}\n")
				       .arg(clock_.isValid() ? clock_.elapsed() : 0)
				       .arg(escape(step), ok ? "true" : "false", escape(detail));
		results_.write(line.toUtf8());
		results_.flush();
		blog(ok ? LOG_INFO : LOG_WARNING, "[mf-ui-driver] %s %s %s", ok ? "PASS" : "FAIL",
		     step.toUtf8().constData(), detail.toUtf8().constData());
	}

	static QString escape(const QString &text)
	{
		QString out;
		for (QChar c : text) {
			if (c == QChar('\\') || c == QChar('"'))
				out += QChar('\\') + QString(c);
			else if (c.unicode() < 0x20)
				out += QString("\\u%1").arg((int)c.unicode(), 4, 16, QChar('0'));
			else
				out += c;
		}
		return out;
	}

	/* ---- widget helpers ---- */

	template<typename T> T *find(const QString &name)
	{
		T *fallback = nullptr;
		for (QWidget *w : QApplication::allWidgets()) {
			if (w->objectName() != name)
				continue;
			if (T *t = qobject_cast<T *>(w)) {
				if (t->isVisible())
					return t;
				fallback = t;
			}
		}
		return fallback;
	}

	template<typename T> T *inModal(const QString &modalName, const QString &child)
	{
		QWidget *modal = QApplication::activeModalWidget();
		if (!modal || modal->objectName() != modalName)
			return nullptr;
		return modal->findChild<T *>(child);
	}

	/* A widget an action is about to use. The step engine carries on after a failed wait, so a
	 * missing widget is reported here and never dereferenced: a null pointer would crash OBS and
	 * look like a plugin crash. */
	template<typename T> T *need(const QString &modalName, const QString &child)
	{
		T *widget = inModal<T>(modalName, child);
		if (!widget)
			emit_line(modalName + "/" + child + " available", false, detailForFailure());
		return widget;
	}

	std::vector<QWidget *> cards()
	{
		std::vector<QWidget *> out;
		QWidget *any = nullptr;
		for (QWidget *w : QApplication::allWidgets()) {
			if (w->objectName().startsWith("mfCard:")) {
				any = w;
				break;
			}
		}
		if (!any || !any->parentWidget() || !any->parentWidget()->layout())
			return out;
		QLayout *layout = any->parentWidget()->layout();
		for (int i = 0; i < layout->count(); i++) {
			QWidget *w = layout->itemAt(i)->widget();
			if (w && w->objectName().startsWith("mfCard:"))
				out.push_back(w);
		}
		return out;
	}

	QWidget *card(const QString &name, int occurrence = 0)
	{
		int seen = 0;
		for (QWidget *c : cards()) {
			auto *label = c->findChild<QLabel *>("mfCardName");
			if (label && label->text() == name && seen++ == occurrence)
				return c;
		}
		return nullptr;
	}

	QString cardText(const QString &name, const char *child)
	{
		QWidget *c = card(name);
		auto *label = c ? c->findChild<QLabel *>(child) : nullptr;
		return label ? label->text() : QString();
	}

	bool cardStateIs(const QString &name, const QString &state)
	{
		return cardText(name, "mfCardState").startsWith(state);
	}

	void clickLater(QAbstractButton *button)
	{
		if (button)
			QTimer::singleShot(0, button, [button]() { button->click(); });
	}

	void clickCardButton(const QString &name, const char *button, int occurrence = 0)
	{
		QWidget *c = card(name, occurrence);
		clickLater(c ? c->findChild<QAbstractButton *>(button) : nullptr);
	}

	void showDock()
	{
		act("show dock", [this]() {
			auto *window = static_cast<QMainWindow *>(obs_frontend_get_main_window());
			auto *dock = window->findChild<QDockWidget *>("matchframe-multistream");
			if (dock) {
				dock->setFloating(false);
				dock->show();
				dock->raise();
			}
			emit_line("dock registered in Docks menu", dock != nullptr,
				  dock ? dock->windowTitle() : QString());
		});
	}

	void hideDock()
	{
		act("hide dock", []() {
			auto *window = static_cast<QMainWindow *>(obs_frontend_get_main_window());
			if (auto *dock = window->findChild<QDockWidget *>("matchframe-multistream"))
				dock->hide();
		});
	}

	/* Accepts any "keep for this session only" question until the editor has closed. */
	void acceptSessionOnly(const QString &label)
	{
		expect(label + ": editor closed (session-only consent if asked)", [this]() {
			QWidget *modal = QApplication::activeModalWidget();
			if (auto *box = qobject_cast<QMessageBox *>(modal)) {
				if (QAbstractButton *yes = box->button(QMessageBox::Yes)) {
					sessionConsent_++;
					clickLater(yes);
				}
				return false;
			}
			return !modal || modal->objectName() != "mfEditor";
		});
	}

	void addDestination(const QString &name, const QString &platform, const QString &url, const QString &key,
			    bool enabled = true)
	{
		act("open editor for " + name, [this]() { clickLater(find<QPushButton>("mfAdd")); });
		expect("editor open for " + name,
		       [this]() { return inModal<QLineEdit>("mfEditor", "mfName") != nullptr; });
		act("fill " + name, [this, name, platform, url, key, enabled]() {
			auto *nameEdit = need<QLineEdit>("mfEditor", "mfName");
			auto *combo = need<QComboBox>("mfEditor", "mfPlatform");
			auto *server = need<QLineEdit>("mfEditor", "mfServer");
			auto *keyEdit = need<QLineEdit>("mfEditor", "mfKey");
			auto *enabledBox = need<QCheckBox>("mfEditor", "mfEnabled");
			if (!nameEdit || !combo || !server || !keyEdit || !enabledBox)
				return;
			nameEdit->setText(name);
			combo->setCurrentIndex(combo->findData(platform));
			server->setText(url);
			keyEdit->setText(key);
			enabledBox->setChecked(enabled);
			clickLater(inModal<QPushButton>("mfEditor", "mfSave"));
		});
		acceptSessionOnly(name);
		expect("card shown for " + name, [this, name]() { return card(name) != nullptr; });
	}

	void stopAllConfirmed(const QStringList &mustList)
	{
		act("click Stop All", [this]() { clickLater(find<QPushButton>("mfStopAll")); });
		expect("Stop All confirmation lists the affected destinations", [this, mustList]() {
			auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
			if (!box || box->objectName() != "mfStopAllConfirm")
				return false;
			for (const QString &name : mustList) {
				if (!box->text().contains(name))
					return false;
			}
			return true;
		});
		act("confirm Stop All", [this]() {
			auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
			if (box)
				clickLater(box->findChild<QPushButton *>("mfStopAllConfirmButton"));
		});
	}

	void triggerMenu(const QString &text)
	{
		auto *menuButton = find<QToolButton>("mfMenu");
		if (!menuButton || !menuButton->menu())
			return;
		for (QAction *action : menuButton->menu()->actions()) {
			if (action->text() == text)
				QTimer::singleShot(0, action, [action]() { action->trigger(); });
		}
	}

	void answerFileDialog(const QString &path)
	{
		expect("file dialog answered", [path]() {
			auto *dialog = qobject_cast<QFileDialog *>(QApplication::activeModalWidget());
			if (!dialog || !dialog->isVisible())
				return false;
			QFileInfo info(path);
			dialog->setDirectory(info.absolutePath());
			dialog->selectFile(info.fileName());
			if (auto *edit = dialog->findChild<QLineEdit *>("fileNameEdit"))
				edit->setText(info.absoluteFilePath());
			QTimer::singleShot(100, dialog, [dialog]() { QMetaObject::invokeMethod(dialog, "accept"); });
			return true;
		});
	}

	void closeMessageBox(const QString &label, const QString &buttonText = QString())
	{
		expect(label, [this, buttonText]() {
			auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
			if (!box)
				return false;
			emit_line("message box", true, box->windowTitle() + ": " + box->text());
			for (QAbstractButton *b : box->buttons()) {
				if (buttonText.isEmpty() || b->text() == buttonText) {
					clickLater(b);
					return true;
				}
			}
			return false;
		});
	}

	void closeObs()
	{
		emit_line("finished", true, QString("session-only consents: %1").arg(sessionConsent_));
		QTimer::singleShot(500, []() {
			auto *window = static_cast<QMainWindow *>(obs_frontend_get_main_window());
			window->close();
		});
	}

	QString dest(int i, const char *field)
	{
		OBSDataArrayAutoRelease array = obs_data_get_array(config_, "destinations");
		OBSDataAutoRelease item = obs_data_array_item(array, (size_t)i);
		return item ? str(item, field) : QString();
	}

	/* ---- scenarios ---- */

	void buildCrud()
	{
		QString a = "KGB YouTube";
		addDestination(a, "youtube", dest(0, "url"), dest(0, "key"));
		expect("new destination is Idle", [this, a]() { return cardStateIs(a, "Idle"); });

		act("edit: rename", [this, a]() { clickCardButton(a, "mfCardEdit"); });
		expect("editor shows a masked key", [this]() {
			auto *key = inModal<QLineEdit>("mfEditor", "mfKey");
			return key && key->echoMode() == QLineEdit::Password && !key->text().isEmpty();
		});
		act("reveal key temporarily", [this]() {
			if (auto *reveal = inModal<QToolButton>("mfEditor", "mfKeyReveal"))
				reveal->setChecked(true);
		});
		expect("reveal shows the key", [this]() {
			auto *key = inModal<QLineEdit>("mfEditor", "mfKey");
			return key && key->echoMode() == QLineEdit::Normal;
		});
		act("hide and rename", [this]() {
			auto *reveal = need<QToolButton>("mfEditor", "mfKeyReveal");
			auto *nameEdit = need<QLineEdit>("mfEditor", "mfName");
			if (!reveal || !nameEdit)
				return;
			reveal->setChecked(false);
			nameEdit->setText("KGB Sports YouTube");
			clickLater(inModal<QPushButton>("mfEditor", "mfSave"));
		});
		a = "KGB Sports YouTube";
		acceptSessionOnly("rename");
		expect("card renamed", [this, a]() { return card(a) != nullptr; });

		act("duplicate", [this, a]() { clickCardButton(a, "mfCardDuplicate"); });
		QString copy = a + " (copy)";
		expect("duplicate is disabled and has no key", [this, copy]() {
			return cardStateIs(copy, "Disabled") &&
			       cardText(copy, "mfCardDetail").contains("No stream key");
		});
		act("delete duplicate", [this, copy]() { clickCardButton(copy, "mfCardDelete"); });
		expect("delete confirmation", [this]() {
			auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
			if (!box || box->objectName() != "mfDeleteConfirm")
				return false;
			clickLater(box->findChild<QPushButton *>("mfDeleteConfirmButton"));
			return true;
		});
		expect("duplicate deleted", [this, copy]() { return card(copy) == nullptr; });

		addDestination("Home Club Custom", "custom", dest(1, "url"), dest(1, "key"));

		act("open encoding settings", [this]() { clickLater(find<QPushButton>("mfEncoding")); });
		expect("encoding dialog open",
		       [this]() { return inModal<QSpinBox>("mfEncodingDialog", "mfVideoBitrate"); });
		act("set 4500 kbps", [this]() {
			auto *bitrate = need<QSpinBox>("mfEncodingDialog", "mfVideoBitrate");
			if (!bitrate)
				return;
			bitrate->setValue(4500);
			clickLater(inModal<QPushButton>("mfEncodingDialog", "mfEncodingSave"));
		});
		expect("program summary shows 4500 kbps", [this]() {
			auto *l = find<QLabel>("mfProgram");
			return l && l->text().contains("4500 kbps");
		});
		expect("estimate reflects 2 enabled destinations at 4500+128 kbps", [this]() {
			auto *l = find<QLabel>("mfEstimate");
			return l && l->text().contains("(2 enabled)") && l->text().contains("9.26 Mbps payload") &&
			       l->text().contains("12.0 Mbps");
		});

		QString exportPath = str(config_, "exportPath");
		act("export", [this]() { triggerMenu("Export destinations (no keys)..."); });
		answerFileDialog(exportPath);
		closeMessageBox("export confirmation");
		act("import (append)", [this]() { triggerMenu("Import destinations..."); });
		answerFileDialog(exportPath);
		closeMessageBox("import: add to current list", "Add to current list");
		expect("imported destinations appear without keys", [this, a]() {
			QWidget *imported = card(a, 1);
			auto *detail = imported ? imported->findChild<QLabel *>("mfCardDetail") : nullptr;
			return cards().size() == 4 && detail && detail->text().contains("No stream key");
		});
		for (int i = 0; i < 2; i++) {
			size_t expectedBefore = 4 - (size_t)i;
			expect(QString("%1 cards before deleting an imported copy").arg(expectedBefore),
			       [this, expectedBefore]() { return cards().size() == expectedBefore; });
			act("delete imported copy", [this, expectedBefore]() {
				/* Only ever delete when the import really added the two cards at the end. */
				if (cards().size() == expectedBefore)
					clickLater(cards().back()->findChild<QAbstractButton *>("mfCardDelete"));
			});
			expect("confirm delete of imported copy", [this]() {
				auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
				if (!box || box->objectName() != "mfDeleteConfirm")
					return false;
				clickLater(box->findChild<QPushButton *>("mfDeleteConfirmButton"));
				return true;
			});
		}
		expect("two destinations remain", [this]() { return cards().size() == 2; });

		act("start KGB Sports YouTube", [this, a]() { clickCardButton(a, "mfCardStartStop"); });
		expect("Sending to server", [this, a]() { return cardStateIs(a, "Sending to server"); }, 25000);
		expect("transport-vs-publication note shown while sending", [this]() {
			auto *note = find<QLabel>("mfTransportNote");
			return note && note->isVisible() && note->text().contains("does not prove");
		});
		expect("encoding settings locked while active", [this]() {
			auto *b = find<QPushButton>("mfEncoding");
			return b && b->toolTip().contains("Locked");
		});
		hideDock();
		sleep(5000);
		expect("still sending while the dock is hidden",
		       [this, a]() { return cardStateIs(a, "Sending to server"); });
		showDock();
		stopAllConfirmed({a});
		expect("Idle after Stop All", [this, a]() { return cardStateIs(a, "Idle"); }, 20000);
	}

	void buildRestore()
	{
		expect("both destinations restored", [this]() {
			return card("KGB Sports YouTube") && card("Home Club Custom") && cards().size() == 2;
		});
		expect("restored destinations are idle (nothing auto-started)", [this]() {
			return cardStateIs("KGB Sports YouTube", "Idle") && cardStateIs("Home Club Custom", "Idle");
		});
		expect("encoding settings restored", [this]() {
			auto *l = find<QLabel>("mfProgram");
			return l && l->text().contains("4500 kbps");
		});
		act("record key status", [this]() {
			emit_line("key status after restart", true, cardText("KGB Sports YouTube", "mfCardDetail"));
		});
		sleep(5000);
		expect("still idle after 5 s", [this]() { return cardStateIs("KGB Sports YouTube", "Idle"); });
		if (!obs_data_get_bool(config_, "expectStoredKeys"))
			return;

		/* Platforms with a secure store: the keys must come back from it and work. */
		expect("both keys restored from the secure store", [this]() {
			return !cardText("KGB Sports YouTube", "mfCardDetail").contains("No stream key") &&
			       !cardText("Home Club Custom", "mfCardDetail").contains("No stream key");
		});
		act("start Home Club Custom with its restored key",
		    [this]() { clickCardButton("Home Club Custom", "mfCardStartStop"); });
		expect(
			"restored key accepted: Sending to server",
			[this]() { return cardStateIs("Home Club Custom", "Sending to server"); }, 25000);
		sleep(5000);
		act("stop Home Club Custom", [this]() { clickCardButton("Home Club Custom", "mfCardStartStop"); });
		expect("Home Club Custom Idle", [this]() { return cardStateIs("Home Club Custom", "Idle"); }, 20000);
	}

	/* Earlier scenarios leave destinations behind; delete them through the dock (which also
	 * deletes their stored keys) so each scenario starts from an empty list. */
	void clearDestinations()
	{
		auto before = std::make_shared<size_t>(0);
		for (int i = 0; i < 16; i++) {
			act("delete a leftover destination", [this, before]() {
				*before = cards().size();
				if (*before)
					clickLater(cards().front()->findChild<QAbstractButton *>("mfCardDelete"));
			});
			wait("confirm deleting a leftover destination", [this, before]() {
				if (*before == 0)
					return true;
				auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
				if (!box || box->objectName() != "mfDeleteConfirm")
					return false;
				clickLater(box->findChild<QPushButton *>("mfDeleteConfirmButton"));
				return true;
			});
			wait("leftover destination deleted",
			     [this, before]() { return *before == 0 || cards().size() < *before; });
		}
		expect("no destinations left from earlier scenarios", [this]() { return cards().empty(); });
	}

	void buildEight()
	{
		clearDestinations();
		for (int i = 0; i < 8; i++)
			addDestination(QString("Dest %1").arg(i + 1), "custom", dest(i, "url"), dest(i, "key"));
		act("Start Enabled Outputs", [this]() { clickLater(find<QPushButton>("mfStartEnabled")); });
		expect(
			"all eight Sending to server",
			[this]() {
				for (int i = 0; i < 8; i++) {
					if (!cardStateIs(QString("Dest %1").arg(i + 1), "Sending to server"))
						return false;
				}
				return true;
			},
			40000);
		mark("all_sending");
		expect("summary shows 8 sending", [this]() {
			auto *l = find<QLabel>("mfSummary");
			return l && l->text().contains("8 sending");
		});
		sleep(8000);
		act("stop Dest 1", [this]() { clickCardButton("Dest 1", "mfCardStartStop"); });
		expect("Dest 1 Idle", [this]() { return cardStateIs("Dest 1", "Idle"); }, 20000);
		mark("dest1_stopped");
		sleep(8000);
		expect("the other seven still Sending", [this]() {
			for (int i = 2; i <= 8; i++) {
				if (!cardStateIs(QString("Dest %1").arg(i), "Sending to server"))
					return false;
			}
			return true;
		});
		stopAllConfirmed({"Dest 2", "Dest 8"});
		expect(
			"all idle",
			[this]() {
				for (int i = 1; i <= 8; i++) {
					if (!cardStateIs(QString("Dest %1").arg(i), "Idle"))
						return false;
				}
				return true;
			},
			30000);
	}

	void makeScenes()
	{
		act("create scenes Cam A / Cam B", [this]() {
			auto media = [&](const char *name, const QString &file) {
				OBSDataAutoRelease settings = obs_data_create();
				obs_data_set_bool(settings, "is_local_file", true);
				obs_data_set_string(settings, "local_file", file.toUtf8().constData());
				obs_data_set_bool(settings, "looping", true);
				obs_data_set_bool(settings, "close_when_inactive", false);
				/* Keep playing through scene changes: a clip restarting on activation leaves a
				 * gap in Program audio that has nothing to do with the plugin. */
				obs_data_set_bool(settings, "restart_on_activate", false);
				obs_source_t *source = obs_source_create("ffmpeg_source", name, settings, nullptr);
				obs_source_set_audio_mixers(source, 1u << 0); // both on Track 1
				return source;
			};
			OBSSceneAutoRelease camA = obs_scene_create("Cam A");
			OBSSceneAutoRelease camB = obs_scene_create("Cam B");
			OBSSourceAutoRelease clipA = media("Clip A", str(config_, "clipA"));
			OBSSourceAutoRelease clipB = media("Clip B", str(config_, "clipB"));
			obs_sceneitem_t *ia = obs_scene_add(camA, clipA);
			obs_sceneitem_t *ib = obs_scene_add(camB, clipB);
			obs_video_info ovi;
			obs_get_video_info(&ovi);
			vec2 bounds = {(float)ovi.base_width, (float)ovi.base_height};
			for (obs_sceneitem_t *item : {ia, ib}) {
				obs_sceneitem_set_bounds_type(item, OBS_BOUNDS_STRETCH);
				obs_sceneitem_set_bounds(item, &bounds);
			}
			obs_frontend_set_current_scene(obs_scene_get_source(camA));
		});
		sleep(1500);
	}

	static obs_source_t *scene_named(const char *name) { return obs_get_source_by_name(name); }

	void buildProgram()
	{
		clearDestinations();
		makeScenes();
		addDestination("Program check", "custom", dest(0, "url"), dest(0, "key"));
		/* Studio Mode is entered before the output starts: entering it swaps Program to a duplicate
		 * of the scene (OBS's default scene duplication), which can briefly drop Program audio and
		 * is not what this scenario checks. Changing only the Preview, below, is. */
		act("enable Studio Mode (Program stays Cam A)", []() { obs_frontend_set_preview_program_mode(true); });
		sleep(1500);
		act("start", [this]() { clickCardButton("Program check", "mfCardStartStop"); });
		expect(
			"Sending to server", [this]() { return cardStateIs("Program check", "Sending to server"); },
			25000);
		mark("sending");
		sleep(6000);

		act("preview Cam B (program stays Cam A)", []() {
			OBSSourceAutoRelease b = scene_named("Cam B");
			obs_frontend_set_current_preview_scene(b);
		});
		mark("preview_b");
		sleep(2000);
		/* Edited while Cam B is only in Preview: it must reach the output with the transition, not
		 * before (OBS's default Studio Mode scene duplication keeps Program's copy separate). */
		act("add a score graphic (green box, top left) to Cam B in Preview", []() {
			OBSDataAutoRelease settings = obs_data_create();
			obs_data_set_int(settings, "color", 0xFF00FF00); // ABGR: opaque green
			obs_data_set_int(settings, "width", 240);
			obs_data_set_int(settings, "height", 136);
			OBSSourceAutoRelease graphic =
				obs_source_create("color_source", "Score graphic", settings, nullptr);
			OBSSourceAutoRelease camB = scene_named("Cam B");
			obs_sceneitem_t *item = obs_scene_add(obs_scene_from_source(camB), graphic);
			vec2 origin = {0.0f, 0.0f};
			obs_sceneitem_set_pos(item, &origin);
		});
		mark("graphic_in_preview");
		sleep(4500);
		act("transition preview to program", []() { obs_frontend_preview_program_trigger_transition(); });
		mark("program_b");
		sleep(6000);

		act("start native OBS streaming", []() { obs_frontend_streaming_start(); });
		expect("native streaming active", []() { return obs_frontend_streaming_active(); }, 20000);
		sleep(3000);
		expect("plugin output unaffected by native start",
		       [this]() { return cardStateIs("Program check", "Sending"); });
		act("stop native streaming", []() { obs_frontend_streaming_stop(); });
		expect("native streaming stopped", []() { return !obs_frontend_streaming_active(); }, 20000);
		expect("plugin output unaffected by native stop",
		       [this]() { return cardStateIs("Program check", "Sending"); });

		act("start recording", []() { obs_frontend_recording_start(); });
		expect("recording active", []() { return obs_frontend_recording_active(); }, 20000);
		sleep(3000);
		act("stop recording", []() { obs_frontend_recording_stop(); });
		expect("recording stopped", []() { return !obs_frontend_recording_active(); }, 20000);
		expect("plugin output unaffected by recording",
		       [this]() { return cardStateIs("Program check", "Sending"); });

		act("start replay buffer", []() { obs_frontend_replay_buffer_start(); });
		expect("replay buffer active", []() { return obs_frontend_replay_buffer_active(); }, 20000);
		sleep(3000);
		act("save replay", []() { obs_frontend_replay_buffer_save(); });
		sleep(1500);
		act("stop replay buffer", []() { obs_frontend_replay_buffer_stop(); });
		expect("replay buffer stopped", []() { return !obs_frontend_replay_buffer_active(); }, 20000);
		expect("plugin output unaffected by replay buffer",
		       [this]() { return cardStateIs("Program check", "Sending"); });

		act("mute Clip B (on program)", []() {
			OBSSourceAutoRelease clip = obs_get_source_by_name("Clip B");
			obs_source_set_muted(clip, true);
		});
		mark("muted");
		sleep(6000);
		act("unmute Clip B and leave Studio Mode", []() {
			OBSSourceAutoRelease clip = obs_get_source_by_name("Clip B");
			obs_source_set_muted(clip, false);
			obs_frontend_set_preview_program_mode(false);
		});
		mark("unmuted");
		sleep(3000);
		act("cut to Cam A", []() {
			OBSSourceAutoRelease a = scene_named("Cam A");
			obs_frontend_set_current_scene(a);
		});
		mark("program_a_again");
		sleep(6000);
		act("stop plugin output", [this]() { clickCardButton("Program check", "mfCardStartStop"); });
		expect("Idle", [this]() { return cardStateIs("Program check", "Idle"); }, 20000);
		mark("stopped");
	}

	void buildProfileExit()
	{
		clearDestinations();
		act("create second profile while idle", []() { obs_frontend_create_profile("Second"); });
		sleep(2000);
		act("back to first profile", []() { obs_frontend_set_current_profile("Untitled"); });
		sleep(2000);
		addDestination("Profile test", "custom", dest(0, "url"), dest(0, "key"));
		act("start", [this]() { clickCardButton("Profile test", "mfCardStartStop"); });
		expect(
			"Sending to server", [this]() { return cardStateIs("Profile test", "Sending to server"); },
			25000);
		sleep(3000);
		act("switch to profile Second while sending", []() { obs_frontend_set_current_profile("Second"); });
		mark("switched_to_second");
		expect(
			"second profile shows its own (empty) destination list",
			[this]() {
				auto *empty = find<QLabel>("mfNotices");
				return cards().empty() && empty && empty->text().contains("profile");
			},
			20000);
		act("switch back", []() { obs_frontend_set_current_profile("Untitled"); });
		expect("destination back, idle", [this]() { return cardStateIs("Profile test", "Idle"); }, 20000);
		act("start again", [this]() { clickCardButton("Profile test", "mfCardStartStop"); });
		expect("Sending again", [this]() { return cardStateIs("Profile test", "Sending to server"); }, 25000);
		sleep(3000);
		mark("exit_requested");
		/* The scenario ends by closing OBS while the output is live. */
	}

	void buildNoAutostart()
	{
		expect("destination restored", [this]() { return card("Profile test") != nullptr; });
		sleep(8000);
		expect("not started automatically after relaunch",
		       [this]() { return cardStateIs("Profile test", "Idle"); });
	}

	obs_data_t *config_;
	QFile results_;
	QTimer tick_;
	QElapsedTimer clock_;
	QElapsedTimer stepClock_;
	std::vector<Step> steps_;
	size_t index_ = 0;
	bool started_ = false;
	bool inTick_ = false;
	int sessionConsent_ = 0;
};

Driver *g_driver = nullptr;

void on_event(enum obs_frontend_event event, void *)
{
	if (event == OBS_FRONTEND_EVENT_FINISHED_LOADING && !g_driver) {
		const char *path = getenv("MF_UITEST_CONFIG");
		if (!path)
			return;
		obs_data_t *config = obs_data_create_from_json_file(path);
		if (!config) {
			blog(LOG_ERROR, "[mf-ui-driver] cannot read %s", path);
			return;
		}
		g_driver = new Driver(config);
		obs_data_release(config);
		QTimer::singleShot(1500, []() { g_driver->begin(); });
	} else if (event == OBS_FRONTEND_EVENT_EXIT && g_driver) {
		blog(LOG_INFO, "[mf-ui-driver] OBS exit event");
	}
}

} // namespace

bool obs_module_load(void)
{
	obs_frontend_add_event_callback(on_event, nullptr);
	return true;
}

void obs_module_unload(void)
{
	delete g_driver;
	g_driver = nullptr;
}
