// SPDX-License-Identifier: GPL-2.0-or-later
// MatchFrame Multistream - Copyright (C) 2026 MatchFrame contributors

#include "ui/destination-editor.hpp"
#include "core/redaction.hpp"
#include "plugin/plugin-context.hpp"
#include "ui/ui-helpers.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QSpinBox>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

namespace mf {

using ui::qstr;

namespace {

constexpr int kRevealMs = 10000;

std::string text_of(const QLineEdit *edit)
{
	return edit->text().toStdString();
}

void clear_line_edit(QLineEdit *edit)
{
	QString text = edit->text();
	text.fill(QChar('\0'));
	edit->clear();
}

} // namespace

DestinationEditorDialog::DestinationEditorDialog(PluginContext &context, const DestinationConfig *existing,
						 QWidget *parent)
	: QDialog(parent),
	  context_(context),
	  isNew_(existing == nullptr)
{
	setObjectName("mfEditor");
	setWindowTitle(isNew_ ? "Add Destination" : "Edit Destination");
	setMinimumWidth(520);
	if (existing) {
		original_ = *existing;
	} else {
		original_.platform = Platform::YouTube;
		original_.enabled = true;
	}

	auto *layout = new QVBoxLayout(this);

	lockBanner_ = new QLabel(
		"This destination is active. Stop it to change its server, stream key, platform or reconnect "
		"settings. You can still rename it.",
		this);
	lockBanner_->setWordWrap(true);
	ui::set_theme_class(lockBanner_, "frame-notice text-warning");
	layout->addWidget(lockBanner_);

	auto *form = new QFormLayout();
	name_ = new QLineEdit(qstr(original_.name), this);
	name_->setObjectName("mfName");
	name_->setMaxLength((int)kMaxNameLength);
	name_->setPlaceholderText("e.g. KGB Sports YouTube");
	form->addRow("Display name", name_);

	platform_ = new QComboBox(this);
	platform_->setObjectName("mfPlatform");
	for (const PlatformInfo &info : all_platforms())
		platform_->addItem(info.label, info.id);
	platform_->setCurrentIndex(platform_->findData(platform_id(original_.platform)));
	form->addRow("Platform", platform_);

	server_ = new QLineEdit(qstr(original_.serverUrl), this);
	server_->setObjectName("mfServer");
	server_->setMaxLength((int)kMaxUrlLength);
	form->addRow("Server URL", server_);
	serverHint_ = new QLabel(this);
	serverHint_->setWordWrap(true);
	ui::set_theme_class(serverHint_, "text-muted");
	form->addRow("", serverHint_);

	key_ = new QLineEdit(this);
	key_->setObjectName("mfKey");
	key_->setEchoMode(QLineEdit::Password);
	key_->setMaxLength((int)kMaxSecretLength);
	auto *keyRow = new QHBoxLayout();
	keyRow->addWidget(key_, 1);
	keyRow->addWidget(makeRevealButton(key_));
	form->addRow("Stream key", keyRow);
	keyStatus_ = new QLabel(this);
	keyStatus_->setWordWrap(true);
	ui::set_theme_class(keyStatus_, "text-muted");
	form->addRow("", keyStatus_);

	useAuth_ = new QCheckBox("Server requires a username and password", this);
	useAuth_->setObjectName("mfUseAuth");
	useAuth_->setChecked(original_.useAuth);
	form->addRow("", useAuth_);
	authBox_ = new QWidget(this);
	auto *authForm = new QFormLayout(authBox_);
	authForm->setContentsMargins(0, 0, 0, 0);
	username_ = new QLineEdit(authBox_);
	username_->setObjectName("mfUsername");
	username_->setEchoMode(QLineEdit::Password);
	auto *userRow = new QHBoxLayout();
	userRow->addWidget(username_, 1);
	userRow->addWidget(makeRevealButton(username_));
	authForm->addRow("Username", userRow);
	password_ = new QLineEdit(authBox_);
	password_->setObjectName("mfPassword");
	password_->setEchoMode(QLineEdit::Password);
	auto *passRow = new QHBoxLayout();
	passRow->addWidget(password_, 1);
	passRow->addWidget(makeRevealButton(password_));
	authForm->addRow("Password", passRow);
	form->addRow("", authBox_);

	enabled_ = new QCheckBox("Enabled (included in Start Enabled Outputs)", this);
	enabled_->setObjectName("mfEnabled");
	enabled_->setChecked(original_.enabled);
	form->addRow("", enabled_);

	autoReconnect_ = new QCheckBox("Reconnect automatically after a connection loss", this);
	autoReconnect_->setObjectName("mfAutoReconnect");
	autoReconnect_->setChecked(original_.autoReconnect);
	maxAttempts_ = new QSpinBox(this);
	maxAttempts_->setObjectName("mfMaxAttempts");
	maxAttempts_->setRange(1, kMaxReconnectAttemptsLimit);
	maxAttempts_->setValue(original_.maxReconnectAttempts);
	maxAttempts_->setSuffix(" attempts");
	auto *reconnectRow = new QHBoxLayout();
	reconnectRow->addWidget(autoReconnect_, 1);
	reconnectRow->addWidget(maxAttempts_);
	form->addRow("Reconnect", reconnectRow);
	auto *reconnectHint = new QLabel("Waits 2, 4, 8, 16, then 30 seconds between attempts.", this);
	ui::set_theme_class(reconnectHint, "text-muted");
	form->addRow("", reconnectHint);

	layout->addLayout(form);

	help_ = new QLabel(this);
	help_->setWordWrap(true);
	help_->setOpenExternalLinks(true);
	help_->setTextFormat(Qt::RichText);
	ui::set_theme_class(help_, "frame-notice");
	layout->addWidget(help_);

	issues_ = new QLabel(this);
	issues_->setObjectName("mfIssues");
	issues_->setWordWrap(true);
	layout->addWidget(issues_);

	auto *buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, this);
	save_ = buttons->button(QDialogButtonBox::Save);
	save_->setObjectName("mfSave");
	layout->addWidget(buttons);
	connect(buttons, &QDialogButtonBox::accepted, this, &DestinationEditorDialog::onSave);
	connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

	if (!isNew_) {
		loadStoredSecret(key_, SecretField::StreamKey);
		if (original_.useAuth) {
			loadStoredSecret(username_, SecretField::Username);
			loadStoredSecret(password_, SecretField::Password);
		}
		loadedKey_ = text_of(key_);
		loadedUser_ = text_of(username_);
		loadedPass_ = text_of(password_);
	}

	for (QLineEdit *edit : {name_, server_, key_, username_, password_})
		connect(edit, &QLineEdit::textChanged, this, &DestinationEditorDialog::validate);
	connect(platform_, &QComboBox::currentIndexChanged, this, &DestinationEditorDialog::onPlatformChanged);
	connect(useAuth_, &QCheckBox::toggled, this, &DestinationEditorDialog::onPlatformChanged);
	connect(autoReconnect_, &QCheckBox::toggled, maxAttempts_, &QWidget::setEnabled);
	maxAttempts_->setEnabled(original_.autoReconnect);

	applyLock(!isNew_ && context_.manager().destinationLocked(original_.id));
	onPlatformChanged();
}

DestinationEditorDialog::~DestinationEditorDialog()
{
	secure_clear(loadedKey_);
	secure_clear(loadedUser_);
	secure_clear(loadedPass_);
	for (QLineEdit *edit : {key_, username_, password_})
		clear_line_edit(edit);
}

QToolButton *DestinationEditorDialog::makeRevealButton(QLineEdit *field)
{
	auto *button = new QToolButton(this);
	button->setObjectName(field->objectName() + "Reveal");
	button->setText("Show");
	button->setCheckable(true);
	button->setToolTip("Reveal for 10 seconds");
	auto *timer = new QTimer(button);
	timer->setSingleShot(true);
	connect(timer, &QTimer::timeout, button, [button]() { button->setChecked(false); });
	connect(button, &QToolButton::toggled, this, [field, button, timer](bool on) {
		field->setEchoMode(on ? QLineEdit::Normal : QLineEdit::Password);
		button->setText(on ? "Hide" : "Show");
		if (on)
			timer->start(kRevealMs);
		else
			timer->stop();
	});
	return button;
}

void DestinationEditorDialog::loadStoredSecret(QLineEdit *field, SecretField fieldId)
{
	std::string value;
	if (context_.loadSecret(original_, fieldId, value))
		field->setText(qstr(value));
	secure_clear(value);
}

void DestinationEditorDialog::applyLock(bool locked)
{
	locked_ = locked;
	lockBanner_->setVisible(locked);
	for (QWidget *w : std::initializer_list<QWidget *>{platform_, server_, key_, useAuth_, authBox_, enabled_,
							   autoReconnect_, maxAttempts_})
		w->setEnabled(!locked);
}

void DestinationEditorDialog::onPlatformChanged()
{
	Platform platform = Platform::Custom;
	platform_from_id(platform_->currentData().toString().toStdString(), platform);
	const PlatformInfo &info = platform_info(platform);
	bool custom = platform == Platform::Custom;
	useAuth_->setVisible(custom);
	authBox_->setVisible(custom && useAuth_->isChecked());
	server_->setPlaceholderText(info.serverExample);
	serverHint_->setText(
		custom ? "Enter the rtmp:// or rtmps:// URL of your server."
		       : QString("Copy the current ingest URL from your %1 dashboard (it can be region- or "
				 "account-specific). Prefer an rtmps:// URL.")
				 .arg(info.label));

	QString html = QString("<b>%1</b>").arg(QString(info.label).toHtmlEscaped());
	if (*info.helpUrl)
		html += QString(" &middot; <a href=\"%1\">Setup help</a>").arg(info.helpUrl);
	if (*info.dashboardUrl)
		html += QString(" &middot; <a href=\"%1\">Dashboard</a>").arg(info.dashboardUrl);
	html += "<br>" + QString(info.goLiveNote).toHtmlEscaped();
	html += "<br>\"Sending to server\" in the dock means the ingest accepted the connection; it does not "
		"prove the platform has made the broadcast public.";
	help_->setText(html);
	validate();
}

DestinationConfig DestinationEditorDialog::collect() const
{
	DestinationConfig config = original_;
	if (config.id.empty())
		config.id = new_uuid();
	config.name = trim(name_->text().toStdString());
	platform_from_id(platform_->currentData().toString().toStdString(), config.platform);
	config.serverUrl = trim(server_->text().toStdString());
	config.useAuth = config.platform == Platform::Custom && useAuth_->isChecked();
	config.enabled = enabled_->isChecked();
	config.autoReconnect = autoReconnect_->isChecked();
	config.maxReconnectAttempts = maxAttempts_->value();
	return config;
}

void DestinationEditorDialog::validate()
{
	DestinationConfig config = collect();
	DestinationSecrets secrets;
	secrets.streamKey = trim(text_of(key_));
	secrets.username = trim(text_of(username_));
	secrets.password = text_of(password_);
	Issues issues = validate_destination(config);
	append(issues, validate_secrets(config, secrets));

	SecretLocation where = isNew_ ? SecretLocation::None
				      : context_.secretLocation(original_, SecretField::StreamKey);
	if (!context_.vault().persistentAvailable())
		keyStatus_->setText(
			"Secure storage is unavailable here: the key can be kept for this OBS session only.");
	else if (where == SecretLocation::Persistent)
		keyStatus_->setText(QString("Stored in %1. Never written to plugin files or logs.")
					    .arg(qstr(context_.vault().persistentName())));
	else if (where == SecretLocation::SessionOnly)
		keyStatus_->setText("Held for this OBS session only; it will be gone after OBS restarts.");
	else
		keyStatus_->setText(QString("Will be stored in %1.").arg(qstr(context_.vault().persistentName())));

	ui::show_issues(issues_, issues);
	save_->setEnabled(!has_errors(issues));
}

void DestinationEditorDialog::onSave()
{
	DestinationConfig config = collect();
	SecretEdits edits;
	std::string key = trim(text_of(key_));
	std::string user = trim(text_of(username_));
	std::string pass = text_of(password_);
	if (!locked_) {
		edits.streamKey = {isNew_ || key != loadedKey_, key, false};
		edits.username = {isNew_ || user != loadedUser_, user, false};
		edits.password = {isNew_ || pass != loadedPass_, pass, false};
	}
	secure_clear(key);
	secure_clear(user);
	secure_clear(pass);

	for (int round = 0; round < 3; round++) {
		SaveOutcome outcome = context_.saveDestination(config, edits);
		if (outcome.ok) {
			savedId_ = outcome.savedId;
			accept();
			return;
		}
		if (!outcome.needsSessionConsent) {
			QMessageBox::warning(this, "Cannot save destination", qstr(outcome.error));
			return;
		}
		QMessageBox::StandardButton answer = QMessageBox::question(
			this, "Secure storage unavailable",
			QString("The %1 could not be stored securely:\n%2\n\nKeep it in memory for this OBS session only? "
				"It will not be written to disk and must be re-entered after OBS restarts.")
				.arg(outcome.consentField == SecretField::StreamKey  ? "stream key"
				     : outcome.consentField == SecretField::Username ? "server username"
										     : "server password",
				     qstr(outcome.storageError)),
			QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
		if (answer != QMessageBox::Yes)
			return;
		edits.of(outcome.consentField).allowSessionOnly = true;
	}
}

} // namespace mf
