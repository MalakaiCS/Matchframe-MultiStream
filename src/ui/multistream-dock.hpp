// SPDX-License-Identifier: GPL-2.0-or-later
// MatchFrame Multistream - Copyright (C) 2026 MatchFrame contributors

#pragma once

#include <QWidget>

#include <map>
#include <string>

class QLabel;
class QPushButton;
class QToolButton;
class QVBoxLayout;
class QTimer;

namespace mf {

class DestinationCard;
class PluginContext;

/*
 * The operator panel. A view only: closing or hiding the dock never stops an output, and
 * every action goes through PluginContext/OutputManager, which enforce the locking rules.
 */
class MultistreamDock final : public QWidget {
	Q_OBJECT

public:
	MultistreamDock(PluginContext &context, QWidget *parent = nullptr);
	~MultistreamDock() override;

public slots:
	void startEnabled();
	void stopAll();
	void openEncoding();
	void addDestination();
	void exportConfig();
	void importConfig();
	void copyDiagnostics();

private slots:
	void refresh();

private:
	void scheduleRefresh();
	void rebuildCards();
	void editDestination(const QString &id);
	void duplicateDestination(const QString &id);
	void deleteDestination(const QString &id);
	void startDestination(const QString &id);
	void stopDestination(const QString &id);
	void setEnabled(const QString &id, bool enabled);
	QString destinationName(const std::string &id) const;

	PluginContext &context_;
	QPushButton *startEnabled_;
	QPushButton *stopAll_;
	QPushButton *encoding_;
	QPushButton *add_;
	QToolButton *menu_;
	QLabel *program_;
	QLabel *summary_;
	QLabel *estimate_;
	QLabel *transportNote_;
	QLabel *degraded_;
	QLabel *notices_;
	QPushButton *dismissNotices_;
	QLabel *runtimeIssues_;
	QLabel *empty_;
	QVBoxLayout *cardsLayout_;
	std::map<std::string, DestinationCard *> cards_;
	std::string cardOrder_;
	QTimer *refreshTimer_;
	QTimer *clockTimer_;
	bool refreshPending_ = false;
};

} // namespace mf
