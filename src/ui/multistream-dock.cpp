// SPDX-License-Identifier: GPL-2.0-or-later
// MatchFrame Multistream - Copyright (C) 2026 MatchFrame contributors

#include "ui/multistream-dock.hpp"
#include "core/media-info.hpp"
#include "plugin/plugin-context.hpp"
#include "ui/destination-card.hpp"
#include "ui/destination-editor.hpp"
#include "ui/encoder-settings-dialog.hpp"
#include "ui/ui-helpers.hpp"

#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollArea>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

namespace mf {

using ui::qstr;

MultistreamDock::MultistreamDock(PluginContext &context, QWidget *parent) : QWidget(parent), context_(context)
{
	setObjectName("mfDock");
	auto *layout = new QVBoxLayout(this);
	layout->setContentsMargins(6, 6, 6, 6);
	layout->setSpacing(6);

	auto *toolbar = new QHBoxLayout();
	startEnabled_ = new QPushButton("Start Enabled Outputs", this);
	startEnabled_->setObjectName("mfStartEnabled");
	startEnabled_->setToolTip("Starts every enabled destination that is not already active");
	stopAll_ = new QPushButton("Stop All Plugin Outputs", this);
	stopAll_->setObjectName("mfStopAll");
	stopAll_->setToolTip("Stops plugin outputs only; native OBS streaming and recording are not affected");
	toolbar->addWidget(startEnabled_);
	toolbar->addWidget(stopAll_);
	layout->addLayout(toolbar);

	auto *toolbar2 = new QHBoxLayout();
	encoding_ = new QPushButton("Encoding...", this);
	encoding_->setObjectName("mfEncoding");
	add_ = new QPushButton("Add Destination", this);
	add_->setObjectName("mfAdd");
	menu_ = new QToolButton(this);
	menu_->setObjectName("mfMenu");
	menu_->setText("More");
	menu_->setPopupMode(QToolButton::InstantPopup);
	auto *menu = new QMenu(menu_);
	connect(menu->addAction("Export destinations (no keys)..."), &QAction::triggered, this,
		&MultistreamDock::exportConfig);
	connect(menu->addAction("Import destinations..."), &QAction::triggered, this, &MultistreamDock::importConfig);
	menu->addSeparator();
	connect(menu->addAction("Copy diagnostics"), &QAction::triggered, this, &MultistreamDock::copyDiagnostics);
	menu_->setMenu(menu);
	toolbar2->addWidget(encoding_);
	toolbar2->addWidget(add_);
	toolbar2->addStretch(1);
	toolbar2->addWidget(menu_);
	layout->addLayout(toolbar2);

	program_ = new QLabel(this);
	program_->setObjectName("mfProgram");
	program_->setWordWrap(true);
	ui::set_theme_class(program_, "text-muted");
	layout->addWidget(program_);

	auto *summaryRow = new QHBoxLayout();
	summary_ = new QLabel(this);
	summary_->setObjectName("mfSummary");
	summary_->setWordWrap(true);
	degraded_ = new QLabel("DEGRADED", this);
	degraded_->setObjectName("mfDegraded");
	QFont bold = degraded_->font();
	bold.setBold(true);
	degraded_->setFont(bold);
	degraded_->setToolTip("An enabled destination is failing or reconnecting");
	ui::set_theme_class(degraded_, "text-danger");
	summaryRow->addWidget(summary_, 1);
	summaryRow->addWidget(degraded_);
	layout->addLayout(summaryRow);

	estimate_ = new QLabel(this);
	estimate_->setObjectName("mfEstimate");
	estimate_->setWordWrap(true);
	estimate_->setToolTip("Enabled destinations x (video + audio kbps) / 1000, plus a 30% allowance. A planning "
			      "estimate, not a measured speed test. Native OBS streaming, recording uploads and other "
			      "traffic on the same connection need additional capacity.");
	ui::set_theme_class(estimate_, "text-muted");
	layout->addWidget(estimate_);

	transportNote_ = new QLabel("\"Sending to server\" means the ingest accepted the stream. It does not prove a "
				    "platform has published the broadcast: confirm in each platform's dashboard.",
				    this);
	transportNote_->setObjectName("mfTransportNote");
	transportNote_->setWordWrap(true);
	ui::set_theme_class(transportNote_, "text-muted");
	layout->addWidget(transportNote_);

	auto *noticeRow = new QHBoxLayout();
	notices_ = new QLabel(this);
	notices_->setObjectName("mfNotices");
	notices_->setWordWrap(true);
	ui::set_theme_class(notices_, "frame-notice text-warning");
	dismissNotices_ = new QPushButton("OK", this);
	dismissNotices_->setObjectName("mfDismissNotices");
	noticeRow->addWidget(notices_, 1);
	noticeRow->addWidget(dismissNotices_, 0, Qt::AlignTop);
	layout->addLayout(noticeRow);

	runtimeIssues_ = new QLabel(this);
	runtimeIssues_->setObjectName("mfRuntimeIssues");
	runtimeIssues_->setWordWrap(true);
	layout->addWidget(runtimeIssues_);

	auto *scroll = new QScrollArea(this);
	scroll->setWidgetResizable(true);
	scroll->setFrameShape(QFrame::NoFrame);
	auto *cardsHost = new QWidget(scroll);
	cardsLayout_ = new QVBoxLayout(cardsHost);
	cardsLayout_->setContentsMargins(0, 0, 0, 0);
	cardsLayout_->setSpacing(6);
	empty_ = new QLabel("No destinations yet. Add one to send the OBS Program feed to another server.", cardsHost);
	empty_->setWordWrap(true);
	ui::set_theme_class(empty_, "text-muted");
	cardsLayout_->addWidget(empty_);
	cardsLayout_->addStretch(1);
	scroll->setWidget(cardsHost);
	layout->addWidget(scroll, 1);

	connect(startEnabled_, &QPushButton::clicked, this, &MultistreamDock::startEnabled);
	connect(stopAll_, &QPushButton::clicked, this, &MultistreamDock::stopAll);
	connect(encoding_, &QPushButton::clicked, this, &MultistreamDock::openEncoding);
	connect(add_, &QPushButton::clicked, this, &MultistreamDock::addDestination);
	connect(dismissNotices_, &QPushButton::clicked, this, [this]() { context_.dismissNotices(); });

	/* Manager events are coalesced; a 1 s clock keeps elapsed times and countdowns moving. */
	refreshTimer_ = new QTimer(this);
	refreshTimer_->setSingleShot(true);
	refreshTimer_->setInterval(100);
	connect(refreshTimer_, &QTimer::timeout, this, &MultistreamDock::refresh);
	clockTimer_ = new QTimer(this);
	clockTimer_->setInterval(1000);
	connect(clockTimer_, &QTimer::timeout, this, &MultistreamDock::refresh);
	clockTimer_->start();

	context_.setChangeHandler([this](const std::string &) { scheduleRefresh(); });
	refresh();
}

MultistreamDock::~MultistreamDock()
{
	context_.setChangeHandler(nullptr);
}

void MultistreamDock::scheduleRefresh()
{
	if (!refreshTimer_->isActive())
		refreshTimer_->start();
}

QString MultistreamDock::destinationName(const std::string &id) const
{
	const DestinationConfig *config = context_.manager().config().find(id);
	return config ? qstr(config->name) : QString();
}

void MultistreamDock::rebuildCards()
{
	const auto &destinations = context_.manager().config().destinations;
	std::string order;
	for (const DestinationConfig &dest : destinations)
		order += dest.id + ";";
	if (order == cardOrder_)
		return;
	cardOrder_ = order;

	std::map<std::string, DestinationCard *> kept;
	for (const DestinationConfig &dest : destinations) {
		auto it = cards_.find(dest.id);
		if (it != cards_.end()) {
			kept[dest.id] = it->second;
			cards_.erase(it);
		}
	}
	for (auto &stale : cards_)
		stale.second->deleteLater();
	cards_ = std::move(kept);

	for (const DestinationConfig &dest : destinations) {
		DestinationCard *card = cards_[dest.id];
		if (!card) {
			card = new DestinationCard(dest.id, cardsLayout_->parentWidget());
			connect(card, &DestinationCard::startRequested, this, &MultistreamDock::startDestination);
			connect(card, &DestinationCard::stopRequested, this, &MultistreamDock::stopDestination);
			connect(card, &DestinationCard::editRequested, this, &MultistreamDock::editDestination);
			connect(card, &DestinationCard::duplicateRequested, this,
				&MultistreamDock::duplicateDestination);
			connect(card, &DestinationCard::deleteRequested, this, &MultistreamDock::deleteDestination);
			connect(card, &DestinationCard::enabledToggled, this, &MultistreamDock::setEnabled);
			cards_[dest.id] = card;
		}
		cardsLayout_->removeWidget(card);
	}
	int index = 0;
	for (const DestinationConfig &dest : destinations)
		cardsLayout_->insertWidget(index++, cards_[dest.id]);
	empty_->setVisible(destinations.empty());
}

void MultistreamDock::refresh()
{
	if (!context_.loaded()) {
		program_->setText("Waiting for OBS to finish loading...");
		for (QWidget *w : std::initializer_list<QWidget *>{startEnabled_, stopAll_, encoding_, add_, menu_})
			w->setEnabled(false);
		return;
	}
	for (QWidget *w : std::initializer_list<QWidget *>{startEnabled_, stopAll_, encoding_, add_, menu_})
		w->setEnabled(true);

	OutputManager &manager = context_.manager();
	const EncoderProfile &p = manager.config().encoder;
	MediaInfo media = manager.mediaInfo();
	std::string encoderName = manager.sharedEncoders().videoChoice().id.empty()
					  ? (p.videoEncoderId.empty() ? std::string("automatic") : p.videoEncoderId)
					  : manager.sharedEncoders().videoChoice().displayName;
	program_->setText(QString("Program feed: %1 · audio Track %2 · H.264 %3 kbps CBR, %4 s keyframes "
				  "(%5) · AAC %6 kbps")
				  .arg(qstr(media.describeVideo()))
				  .arg(p.audioTrack)
				  .arg(p.videoBitrateKbps)
				  .arg(p.keyframeIntervalSec)
				  .arg(qstr(encoderName))
				  .arg(p.audioBitrateKbps));

	ManagerSummary sum = manager.summary();
	int active = sum.sending + sum.connecting + sum.reconnecting + sum.stopping;
	QStringList parts;
	parts << QString("%1 active / %2 configured").arg(active).arg(sum.configured);
	if (sum.sending)
		parts << QString("%1 sending").arg(sum.sending);
	if (sum.reconnecting)
		parts << QString("%1 reconnecting").arg(sum.reconnecting);
	if (sum.errors)
		parts << QString("%1 error").arg(sum.errors);
	if (sum.queued)
		parts << QString("%1 queued").arg(sum.queued);
	parts << QString("measured out %1").arg(ui::format_bitrate(sum.measuredKbps));
	summary_->setText(parts.join("  ·  "));
	degraded_->setVisible(sum.degraded);
	estimate_->setText(
		QString("Upload estimate, plugin outputs only (%1 enabled): %2 Mbps payload, about %3 Mbps "
			"recommended. Not a speed test; native streaming and other traffic need extra capacity.")
			.arg(sum.estimate.destinations)
			.arg(sum.estimate.payloadMbps, 0, 'f', 2)
			.arg(sum.estimate.recommendedMbps, 0, 'f', 1));
	transportNote_->setVisible(sum.sending > 0);
	stopAll_->setEnabled(!manager.stopAllTargets().empty());
	encoding_->setToolTip(sum.encoderLocked ? "Locked while plugin outputs are active" : QString());

	QStringList notices;
	if (!sum.groupFailure.empty())
		notices << "Shared encoder: " + qstr(sum.groupFailure);
	if (context_.readOnly())
		notices << "This configuration was written by a newer plugin version and is read-only.";
	for (const std::string &notice : context_.notices())
		notices << qstr(notice);
	notices_->setText(notices.join("\n"));
	notices_->setVisible(!notices.isEmpty());
	dismissNotices_->setVisible(!context_.notices().empty());

	Issues runtime = validate_media(media);
	ui::show_issues(runtimeIssues_, runtime, false);

	rebuildCards();
	for (const DestinationConfig &dest : manager.config().destinations) {
		CardModel model;
		model.config = dest;
		model.snapshot = manager.snapshot(dest.id);
		model.locked = manager.destinationLocked(dest.id);
		model.queued = manager.isQueued(dest.id);
		SecretLocation key = context_.secretLocation(dest, SecretField::StreamKey);
		model.keyMissing = key == SecretLocation::None;
		model.sessionOnlyKey = key == SecretLocation::SessionOnly;
		model.issues = manager.destinationIssues(dest.id);
		cards_[dest.id]->update(model);
	}
}

void MultistreamDock::startEnabled()
{
	/* Destinations that cannot start show the reason on their own card. */
	context_.manager().startEnabled();
	scheduleRefresh();
}

void MultistreamDock::stopAll()
{
	std::vector<std::string> targets = context_.manager().stopAllTargets();
	if (targets.empty())
		return;
	QString list;
	for (const std::string &id : targets) {
		SessionSnapshot snap = context_.manager().snapshot(id);
		QString state = context_.manager().isQueued(id) && !state_is_active(snap.state)
					? "Queued"
					: ui::state_label(snap.state);
		list += QString("• %1 (%2)\n").arg(destinationName(id), state);
	}
	QMessageBox box(QMessageBox::Warning, "Stop all plugin outputs?",
			QString("These plugin destinations will stop, and pending reconnects and queued starts are "
				"cancelled:\n\n%1\nNative OBS streaming, recording and replay buffer are not affected.")
				.arg(list),
			QMessageBox::Cancel, this);
	box.setObjectName("mfStopAllConfirm");
	QPushButton *confirm = box.addButton("Stop All", QMessageBox::DestructiveRole);
	confirm->setObjectName("mfStopAllConfirmButton");
	box.setDefaultButton(QMessageBox::Cancel);
	box.exec();
	if (box.clickedButton() != confirm)
		return;
	context_.manager().stopAll(StopReason::StopAll);
	scheduleRefresh();
}

void MultistreamDock::openEncoding()
{
	EncoderSettingsDialog dialog(context_, this);
	dialog.exec();
	scheduleRefresh();
}

void MultistreamDock::addDestination()
{
	DestinationEditorDialog dialog(context_, nullptr, this);
	dialog.exec();
	scheduleRefresh();
}

void MultistreamDock::editDestination(const QString &id)
{
	const DestinationConfig *config = context_.manager().config().find(id.toStdString());
	if (!config)
		return;
	DestinationConfig copy = *config;
	DestinationEditorDialog dialog(context_, &copy, this);
	dialog.exec();
	scheduleRefresh();
}

void MultistreamDock::duplicateDestination(const QString &id)
{
	std::string newId, error;
	if (!context_.duplicateDestination(id.toStdString(), newId, error))
		QMessageBox::warning(this, "Cannot duplicate", qstr(error));
	scheduleRefresh();
}

void MultistreamDock::deleteDestination(const QString &id)
{
	QString name = destinationName(id.toStdString());
	QMessageBox box(
		QMessageBox::Question, "Delete destination?",
		QString("Delete \"%1\"? Its stored stream key and server credentials are removed too.").arg(name),
		QMessageBox::Cancel, this);
	box.setObjectName("mfDeleteConfirm");
	QPushButton *confirm = box.addButton("Delete", QMessageBox::DestructiveRole);
	confirm->setObjectName("mfDeleteConfirmButton");
	box.setDefaultButton(QMessageBox::Cancel);
	box.exec();
	if (box.clickedButton() != confirm)
		return;
	std::string error;
	if (!context_.deleteDestination(id.toStdString(), error))
		QMessageBox::warning(this, "Cannot delete", qstr(error));
	scheduleRefresh();
}

void MultistreamDock::startDestination(const QString &id)
{
	std::string error;
	context_.manager().startDestination(id.toStdString(), error); // failures are shown on the card
	scheduleRefresh();
}

void MultistreamDock::stopDestination(const QString &id)
{
	context_.manager().stopDestination(id.toStdString());
	scheduleRefresh();
}

void MultistreamDock::setEnabled(const QString &id, bool enabled)
{
	std::string error;
	if (!context_.setEnabled(id.toStdString(), enabled, error))
		QMessageBox::warning(this, "Cannot change", qstr(error));
	scheduleRefresh();
}

void MultistreamDock::exportConfig()
{
	QString path = QFileDialog::getSaveFileName(this, "Export destinations", "matchframe-destinations.json",
						    "JSON (*.json)");
	if (path.isEmpty())
		return;
	std::string error;
	if (context_.exportTo(path.toStdString(), error))
		QMessageBox::information(
			this, "Exported",
			QString("Exported %1 destination(s) and the shared encoding settings. Stream keys "
				"and server credentials were not exported.")
				.arg(context_.manager().config().destinations.size()));
	else
		QMessageBox::warning(this, "Export failed", qstr(error));
}

void MultistreamDock::importConfig()
{
	QString path = QFileDialog::getOpenFileName(this, "Import destinations", QString(), "JSON (*.json)");
	if (path.isEmpty())
		return;
	ImportResult import = context_.readImport(path.toStdString());
	if (!import.ok) {
		QStringList problems;
		for (const std::string &problem : import.problems)
			problems << qstr(problem);
		QMessageBox::warning(this, "Import rejected",
				     "The file was not imported and the current configuration is unchanged:\n\n" +
					     problems.join("\n"));
		return;
	}
	QMessageBox box(QMessageBox::Question, "Import destinations",
			QString("The file contains %1 destination(s). Imported destinations have no stream keys; enter "
				"them before starting. Importing never starts a broadcast.")
				.arg(import.destinations.size()),
			QMessageBox::Cancel, this);
	QPushButton *append = box.addButton("Add to current list", QMessageBox::AcceptRole);
	QPushButton *replace = box.addButton("Replace all", QMessageBox::DestructiveRole);
	replace->setEnabled(!context_.manager().anyBusy());
	replace->setToolTip("Only while no plugin output is active. Replaces the shared encoding settings too.");
	box.exec();
	if (box.clickedButton() != append && box.clickedButton() != replace)
		return;
	std::string error;
	if (!context_.applyImport(import, box.clickedButton() == replace, error))
		QMessageBox::warning(this, "Import failed", qstr(error));
	scheduleRefresh();
}

void MultistreamDock::copyDiagnostics()
{
	QApplication::clipboard()->setText(qstr(context_.diagnostics()));
	QMessageBox::information(this, "Diagnostics copied",
				 "Diagnostics were copied to the clipboard. They contain no stream keys, passwords or "
				 "full ingest URLs.");
}

} // namespace mf
