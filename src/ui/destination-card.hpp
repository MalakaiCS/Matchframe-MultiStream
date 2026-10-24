// SPDX-License-Identifier: GPL-2.0-or-later
// MatchFrame Multistream - Copyright (C) 2026 MatchFrame contributors

#pragma once

#include "core/destination-config.hpp"
#include "core/session-types.hpp"
#include "core/validation.hpp"

#include <QFrame>

class QCheckBox;
class QLabel;
class QPushButton;
class QToolButton;

namespace mf {

struct CardModel {
	DestinationConfig config;
	SessionSnapshot snapshot;
	bool locked = false;     // URL/credentials/destructive actions locked
	bool queued = false;     // waiting in the Start Enabled queue
	bool keyMissing = false; // no stream key stored or held
	bool sessionOnlyKey = false;
	Issues issues; // configuration and platform-constraint warnings
};

class DestinationCard final : public QFrame {
	Q_OBJECT

public:
	DestinationCard(const std::string &id, QWidget *parent);
	void update(const CardModel &model);
	const std::string &id() const { return id_; }

signals:
	void startRequested(const QString &id);
	void stopRequested(const QString &id);
	void editRequested(const QString &id);
	void duplicateRequested(const QString &id);
	void deleteRequested(const QString &id);
	void enabledToggled(const QString &id, bool enabled);

private:
	std::string id_;
	bool active_ = false;
	QCheckBox *enabled_;
	QLabel *name_;
	QLabel *platform_;
	QLabel *state_;
	QLabel *stats_;
	QLabel *detail_;
	QPushButton *startStop_;
	QToolButton *dismiss_;
	QToolButton *edit_;
	QToolButton *duplicate_;
	QToolButton *delete_;
	QToolButton *dashboard_;
	QString dashboardUrl_;
};

} // namespace mf
