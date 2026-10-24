// SPDX-License-Identifier: GPL-2.0-or-later
// MatchFrame Multistream - Copyright (C) 2026 MatchFrame contributors

#include "ui/destination-card.hpp"
#include "ui/ui-helpers.hpp"

#include <QCheckBox>
#include <QDesktopServices>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>

namespace mf {

using ui::qstr;

DestinationCard::DestinationCard(const std::string &id, QWidget *parent) : QFrame(parent), id_(id)
{
	setObjectName(qstr("mfCard:" + id));
	setFrameShape(QFrame::StyledPanel);
	auto *layout = new QVBoxLayout(this);
	layout->setContentsMargins(8, 6, 8, 6);
	layout->setSpacing(3);

	auto *top = new QHBoxLayout();
	enabled_ = new QCheckBox(this);
	enabled_->setObjectName("mfCardEnabled");
	enabled_->setToolTip("Enabled destinations are started by Start Enabled Outputs");
	name_ = new QLabel(this);
	name_->setObjectName("mfCardName");
	QFont bold = name_->font();
	bold.setBold(true);
	name_->setFont(bold);
	name_->setTextFormat(Qt::PlainText);
	platform_ = new QLabel(this);
	ui::set_theme_class(platform_, "text-muted");
	state_ = new QLabel(this);
	state_->setObjectName("mfCardState");
	state_->setFont(bold);
	top->addWidget(enabled_);
	top->addWidget(name_, 1);
	top->addWidget(platform_);
	top->addSpacing(8);
	top->addWidget(state_);
	layout->addLayout(top);

	stats_ = new QLabel(this);
	stats_->setObjectName("mfCardStats");
	layout->addWidget(stats_);

	detail_ = new QLabel(this);
	detail_->setObjectName("mfCardDetail");
	detail_->setWordWrap(true);
	detail_->setTextFormat(Qt::PlainText);
	layout->addWidget(detail_);

	auto *buttons = new QHBoxLayout();
	startStop_ = new QPushButton(this);
	startStop_->setObjectName("mfCardStartStop");
	dismiss_ = new QToolButton(this);
	dismiss_->setObjectName("mfCardDismiss");
	dismiss_->setText("Dismiss");
	dismiss_->setToolTip("Clear the error without retrying");
	edit_ = new QToolButton(this);
	edit_->setObjectName("mfCardEdit");
	edit_->setText("Edit");
	duplicate_ = new QToolButton(this);
	duplicate_->setObjectName("mfCardDuplicate");
	duplicate_->setText("Duplicate");
	delete_ = new QToolButton(this);
	delete_->setObjectName("mfCardDelete");
	delete_->setText("Delete");
	dashboard_ = new QToolButton(this);
	dashboard_->setObjectName("mfCardDashboard");
	dashboard_->setText("Dashboard");
	dashboard_->setToolTip("Open the platform dashboard to confirm publication");
	buttons->addWidget(startStop_);
	buttons->addWidget(dismiss_);
	buttons->addStretch(1);
	buttons->addWidget(dashboard_);
	buttons->addWidget(edit_);
	buttons->addWidget(duplicate_);
	buttons->addWidget(delete_);
	layout->addLayout(buttons);

	QString qid = qstr(id_);
	connect(startStop_, &QPushButton::clicked, this, [this, qid]() {
		if (active_)
			emit stopRequested(qid);
		else
			emit startRequested(qid);
	});
	connect(dismiss_, &QToolButton::clicked, this, [this, qid]() { emit stopRequested(qid); });
	connect(edit_, &QToolButton::clicked, this, [this, qid]() { emit editRequested(qid); });
	connect(duplicate_, &QToolButton::clicked, this, [this, qid]() { emit duplicateRequested(qid); });
	connect(delete_, &QToolButton::clicked, this, [this, qid]() { emit deleteRequested(qid); });
	connect(enabled_, &QCheckBox::clicked, this, [this, qid](bool on) { emit enabledToggled(qid, on); });
	connect(dashboard_, &QToolButton::clicked, this, [this]() {
		if (!dashboardUrl_.isEmpty())
			QDesktopServices::openUrl(QUrl(dashboardUrl_));
	});
}

void DestinationCard::update(const CardModel &model)
{
	const SessionSnapshot &snap = model.snapshot;
	const PlatformInfo &info = platform_info(model.config.platform);

	name_->setText(qstr(model.config.name));
	platform_->setText(info.label);
	enabled_->setChecked(model.config.enabled);
	enabled_->setEnabled(!model.locked);
	dashboardUrl_ = info.dashboardUrl;
	dashboard_->setVisible(!dashboardUrl_.isEmpty());

	QString state = ui::state_label(snap.state);
	if (model.queued && !state_is_active(snap.state))
		state = "Queued";
	if (snap.state == SessionState::Reconnecting) {
		state += QString(" %1/%2").arg(snap.retryAttempt).arg(snap.retryMax);
		if (snap.retryInSeconds >= 0)
			state += QString(" in %1 s").arg(snap.retryInSeconds);
	}
	state_->setText(state);
	ui::set_theme_class(state_, ui::state_class(snap.state));
	state_->setToolTip(snap.state == SessionState::Sending
				   ? "The ingest server accepted the stream. This is not proof that the platform has "
				     "published the broadcast."
				   : QString());

	QStringList stats;
	if (snap.state == SessionState::Sending) {
		stats << ui::format_bitrate(snap.bitrateKbps) << ui::format_duration(snap.sendingSeconds);
		if (snap.dropStatsAvailable)
			stats << QString("dropped %1 (%2%)").arg(snap.framesDropped).arg(snap.dropPercent, 0, 'f', 1);
	}
	if (snap.reconnectCount > 0 || snap.state == SessionState::Sending)
		stats << QString("reconnects %1").arg(snap.reconnectCount);
	stats_->setText(stats.join("  ·  "));
	stats_->setVisible(!stats.isEmpty());

	QStringList details;
	QString detailClass = "text-muted";
	if (!snap.lastError.empty()) {
		details << (snap.lastErrorRecovered ? "Recovered from: " : "") + qstr(snap.lastError.message);
		if (!snap.lastErrorRecovered)
			detailClass = "text-danger";
	}
	if (model.keyMissing && !state_is_active(snap.state)) {
		details << "No stream key stored. Edit the destination to enter it.";
		if (detailClass == "text-muted")
			detailClass = "text-warning";
	} else if (model.sessionOnlyKey) {
		details << "Stream key held for this OBS session only.";
	}
	for (const Issue &issue : model.issues) {
		if (issue.severity == Severity::Info)
			continue;
		details << qstr(issue.message);
		if (detailClass == "text-muted")
			detailClass = issue.severity == Severity::Error ? "text-danger" : "text-warning";
	}
	detail_->setText(details.join("\n"));
	detail_->setVisible(!details.isEmpty());
	ui::set_theme_class(detail_, detailClass);

	active_ = state_is_active(snap.state) || snap.attemptId != 0;
	bool stopping = snap.state == SessionState::Stopping;
	if (active_) {
		startStop_->setText(stopping ? "Stopping..." : "Stop");
		startStop_->setEnabled(!stopping);
	} else if (snap.state == SessionState::Error) {
		/* Stop acknowledges the error and returns to Idle; Start retries. */
		startStop_->setText("Start");
		startStop_->setEnabled(model.config.enabled);
	} else {
		startStop_->setText(model.queued ? "Queued" : "Start");
		startStop_->setEnabled(model.config.enabled && !model.queued);
	}
	startStop_->setToolTip(model.config.enabled ? QString() : "Enable this destination to start it");
	dismiss_->setVisible(snap.state == SessionState::Error && snap.attemptId == 0);
	delete_->setEnabled(!model.locked);
	edit_->setToolTip(model.locked ? "Live: only the display name can be changed" : QString());
}

} // namespace mf
