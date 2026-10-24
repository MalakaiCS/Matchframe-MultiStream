// SPDX-License-Identifier: GPL-2.0-or-later
// MatchFrame Multistream - Copyright (C) 2026 MatchFrame contributors

#pragma once

#include "core/session-types.hpp"
#include "core/validation.hpp"

#include <QLabel>
#include <QString>
#include <QWidget>

#include <string>

namespace mf::ui {

inline QString qstr(const std::string &text)
{
	return QString::fromStdString(text);
}

/* Applies one of OBS's theme utility classes (text-danger, text-warning, text-success,
 * text-muted, frame-notice ...) so colours follow the active OBS theme. */
void set_theme_class(QWidget *widget, const QString &classes);

QString format_duration(int64_t seconds);
QString format_bitrate(double kbps);
QString state_label(SessionState state);
QString state_class(SessionState state);

/* Fills a label with issues, most severe first; hides it when there are none. */
void show_issues(QLabel *label, const Issues &issues, bool includeInfo = true);

} // namespace mf::ui
