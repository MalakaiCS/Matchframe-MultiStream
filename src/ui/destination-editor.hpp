// SPDX-License-Identifier: GPL-2.0-or-later
// MatchFrame Multistream - Copyright (C) 2026 MatchFrame contributors

#pragma once

#include "core/destination-config.hpp"

#include <QDialog>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QSpinBox;
class QTimer;
class QToolButton;
class QWidget;

namespace mf {

class PluginContext;

/*
 * Add/edit one destination. Secrets are masked; "Show" reveals for 10 seconds. While the
 * destination is live only its display name can change. Nothing here connects to a server:
 * validation is local only (no hidden "test connection" that would publish the program).
 */
class DestinationEditorDialog final : public QDialog {
	Q_OBJECT

public:
	DestinationEditorDialog(PluginContext &context, const DestinationConfig *existing, QWidget *parent);
	~DestinationEditorDialog() override;

	const std::string &savedId() const { return savedId_; }

private slots:
	void onPlatformChanged();
	void validate();
	void onSave();

private:
	DestinationConfig collect() const;
	void loadStoredSecret(QLineEdit *field, SecretField field_id);
	QToolButton *makeRevealButton(QLineEdit *field);
	void applyLock(bool locked);

	PluginContext &context_;
	DestinationConfig original_;
	bool isNew_;
	bool locked_ = false;
	std::string savedId_;
	std::string loadedKey_, loadedUser_, loadedPass_;

	QLabel *lockBanner_;
	QLineEdit *name_;
	QComboBox *platform_;
	QLineEdit *server_;
	QLabel *serverHint_;
	QLineEdit *key_;
	QLabel *keyStatus_;
	QCheckBox *useAuth_;
	QWidget *authBox_;
	QLineEdit *username_;
	QLineEdit *password_;
	QCheckBox *enabled_;
	QCheckBox *autoReconnect_;
	QSpinBox *maxAttempts_;
	QLabel *help_;
	QLabel *issues_;
	QPushButton *save_;
};

} // namespace mf
