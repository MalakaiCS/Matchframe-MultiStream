// SPDX-License-Identifier: GPL-2.0-or-later
// MatchFrame Multistream - Copyright (C) 2026 MatchFrame contributors

#pragma once

#include "core/encoder-profile.hpp"

#include <QDialog>

class QComboBox;
class QLabel;
class QPushButton;
class QSpinBox;

namespace mf {

class PluginContext;

/* The one shared encoding profile. Read-only while any plugin output is active. */
class EncoderSettingsDialog final : public QDialog {
	Q_OBJECT

public:
	EncoderSettingsDialog(PluginContext &context, QWidget *parent);

private slots:
	void validate();
	void onSave();

private:
	EncoderProfile collect() const;

	PluginContext &context_;
	bool locked_;
	QComboBox *videoEncoder_;
	QSpinBox *videoBitrate_;
	QSpinBox *keyframe_;
	QComboBox *audioEncoder_;
	QComboBox *audioBitrate_;
	QComboBox *audioTrack_;
	QLabel *issues_;
	QPushButton *save_;
};

} // namespace mf
