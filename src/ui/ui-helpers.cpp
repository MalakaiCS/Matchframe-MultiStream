// SPDX-License-Identifier: GPL-2.0-or-later
// MatchFrame Multistream - Copyright (C) 2026 MatchFrame contributors

#include "ui/ui-helpers.hpp"

#include <QStyle>

namespace mf::ui {

void set_theme_class(QWidget *widget, const QString &classes)
{
	if (widget->property("class").toString() == classes)
		return;
	widget->setProperty("class", classes);
	widget->style()->unpolish(widget);
	widget->style()->polish(widget);
}

QString format_duration(int64_t seconds)
{
	if (seconds < 0)
		seconds = 0;
	int64_t h = seconds / 3600, m = (seconds / 60) % 60, s = seconds % 60;
	return QString("%1:%2:%3").arg(h).arg(m, 2, 10, QChar('0')).arg(s, 2, 10, QChar('0'));
}

QString format_bitrate(double kbps)
{
	if (kbps >= 1000.0)
		return QString("%1 Mb/s").arg(kbps / 1000.0, 0, 'f', 1);
	return QString("%1 kb/s").arg(kbps, 0, 'f', 0);
}

QString state_label(SessionState state)
{
	switch (state) {
	case SessionState::Disabled:
		return "Disabled";
	case SessionState::Idle:
		return "Idle";
	case SessionState::Connecting:
		return "Connecting";
	case SessionState::Sending:
		/* Transport only: never presented as platform publication. */
		return "Sending to server";
	case SessionState::Reconnecting:
		return "Reconnecting";
	case SessionState::Stopping:
		return "Stopping";
	case SessionState::Error:
		return "Error";
	}
	return "Unknown";
}

QString state_class(SessionState state)
{
	switch (state) {
	case SessionState::Sending:
		return "text-success";
	case SessionState::Connecting:
	case SessionState::Reconnecting:
	case SessionState::Stopping:
		return "text-warning";
	case SessionState::Error:
		return "text-danger";
	default:
		return "text-muted";
	}
}

void show_issues(QLabel *label, const Issues &issues, bool includeInfo)
{
	QStringList errors, warnings, infos;
	for (const Issue &issue : issues) {
		QString text = qstr(issue.message).toHtmlEscaped();
		if (issue.severity == Severity::Error)
			errors << text;
		else if (issue.severity == Severity::Warning)
			warnings << text;
		else if (includeInfo)
			infos << text;
	}
	QStringList all;
	for (const QString &e : errors)
		all << QString("&#9888; %1").arg(e);
	for (const QString &w : warnings)
		all << QString("&#9888; %1").arg(w);
	for (const QString &i : infos)
		all << QString("&#8505; %1").arg(i);
	label->setTextFormat(Qt::RichText);
	label->setText(all.join("<br>"));
	label->setVisible(!all.isEmpty());
	set_theme_class(label, !errors.isEmpty() ? "text-danger" : !warnings.isEmpty() ? "text-warning" : "text-muted");
}

} // namespace mf::ui
