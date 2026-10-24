// SPDX-License-Identifier: GPL-2.0-or-later
// MatchFrame Multistream - Copyright (C) 2026 MatchFrame contributors

#include "ui/encoder-settings-dialog.hpp"
#include "core/encoder-catalog.hpp"
#include "core/media-info.hpp"
#include "core/service-constraints.hpp"
#include "plugin/plugin-context.hpp"
#include "ui/ui-helpers.hpp"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QSpinBox>
#include <QStandardItemModel>
#include <QVBoxLayout>

namespace mf {

using ui::qstr;

namespace {

void disable_item(QComboBox *combo, int index)
{
	if (auto *model = qobject_cast<QStandardItemModel *>(combo->model())) {
		if (QStandardItem *item = model->item(index))
			item->setEnabled(false);
	}
}

} // namespace

EncoderSettingsDialog::EncoderSettingsDialog(PluginContext &context, QWidget *parent)
	: QDialog(parent),
	  context_(context),
	  locked_(context.manager().encoderLocked())
{
	setObjectName("mfEncodingDialog");
	setWindowTitle("Shared Encoding Settings");
	setMinimumWidth(500);
	const EncoderProfile &profile = context_.manager().config().encoder;
	MediaInfo media = context_.manager().mediaInfo();

	auto *layout = new QVBoxLayout(this);
	if (locked_) {
		auto *banner = new QLabel("Plugin outputs are active. Shared encoding settings are locked until every "
					  "plugin destination has stopped.",
					  this);
		banner->setWordWrap(true);
		ui::set_theme_class(banner, "frame-notice text-warning");
		layout->addWidget(banner);
	}

	auto *intro =
		new QLabel("One plugin-owned H.264 encoder and one AAC encoder serve every plugin destination. "
			   "Each destination still uses its own upload bandwidth. Per-destination bitrates are not "
			   "possible while the encoder is shared.",
			   this);
	intro->setWordWrap(true);
	ui::set_theme_class(intro, "text-muted");
	layout->addWidget(intro);

	auto *form = new QFormLayout();
	auto *video = new QLabel(
		qstr(media.describeVideo() + ", " + video_format_name(media.format)) +
			"<br><span>Inherited from OBS Settings &gt; Video. The plugin never changes OBS settings.</span>",
		this);
	video->setTextFormat(Qt::RichText);
	video->setWordWrap(true);
	form->addRow("Program video", video);
	form->addRow("Program audio", new QLabel(qstr(media.describeAudio()), this));

	videoEncoder_ = new QComboBox(this);
	videoEncoder_->setObjectName("mfVideoEncoder");
	std::vector<EncoderChoice> videoChoices = list_h264_encoders();
	std::string automatic = choose_default_video_encoder(videoChoices);
	videoEncoder_->addItem(
		QString("Automatic (currently %1)").arg(automatic.empty() ? QString("none available") : qstr(automatic)),
		QString());
	for (const EncoderChoice &choice : videoChoices) {
		QString label = qstr(choice.displayName) + (choice.gpuTexture ? "  [hardware]" : "");
		if (!choice.usable())
			label += "  (no bitrate setting: cannot be shared)";
		videoEncoder_->addItem(label, qstr(choice.id));
		if (!choice.usable())
			disable_item(videoEncoder_, videoEncoder_->count() - 1);
	}
	int videoIndex = videoEncoder_->findData(qstr(profile.videoEncoderId));
	videoEncoder_->setCurrentIndex(videoIndex < 0 ? 0 : videoIndex);
	form->addRow("Video encoder (H.264)", videoEncoder_);

	videoBitrate_ = new QSpinBox(this);
	videoBitrate_->setObjectName("mfVideoBitrate");
	videoBitrate_->setRange(kMinVideoBitrateKbps, kMaxVideoBitrateKbps);
	videoBitrate_->setSingleStep(250);
	videoBitrate_->setSuffix(" kbps");
	videoBitrate_->setValue(profile.videoBitrateKbps);
	form->addRow("Video bitrate", videoBitrate_);
	form->addRow("Rate control", new QLabel("CBR", this));

	keyframe_ = new QSpinBox(this);
	keyframe_->setObjectName("mfKeyint");
	keyframe_->setRange(kMinKeyframeSec, kMaxKeyframeSec);
	keyframe_->setSuffix(" s");
	keyframe_->setValue(profile.keyframeIntervalSec);
	form->addRow("Keyframe interval", keyframe_);

	audioEncoder_ = new QComboBox(this);
	audioEncoder_->setObjectName("mfAudioEncoder");
	for (const EncoderChoice &choice : list_aac_encoders())
		audioEncoder_->addItem(qstr(choice.displayName), qstr(choice.id));
	int audioIndex = audioEncoder_->findData(qstr(profile.audioEncoderId));
	if (audioIndex < 0 && !profile.audioEncoderId.empty()) {
		audioEncoder_->addItem(qstr(profile.audioEncoderId) + " (not available)", qstr(profile.audioEncoderId));
		audioIndex = audioEncoder_->count() - 1;
	}
	audioEncoder_->setCurrentIndex(audioIndex < 0 ? 0 : audioIndex);
	form->addRow("Audio encoder (AAC)", audioEncoder_);

	audioBitrate_ = new QComboBox(this);
	audioBitrate_->setObjectName("mfAudioBitrate");
	for (int bitrate : supported_audio_bitrates())
		audioBitrate_->addItem(QString("%1 kbps").arg(bitrate), bitrate);
	audioBitrate_->setCurrentIndex(audioBitrate_->findData(profile.audioBitrateKbps));
	form->addRow("Audio bitrate", audioBitrate_);

	audioTrack_ = new QComboBox(this);
	audioTrack_->setObjectName("mfAudioTrack");
	for (int track = 1; track <= kAudioTrackCount; track++)
		audioTrack_->addItem(QString("Track %1").arg(track), track);
	audioTrack_->setCurrentIndex(profile.audioTrack - 1);
	form->addRow("OBS audio track", audioTrack_);
	auto *trackHint =
		new QLabel("Uses the selected track from OBS's Advanced Audio Properties. Monitoring audio is "
			   "only included if it is routed to this track.",
			   this);
	trackHint->setWordWrap(true);
	ui::set_theme_class(trackHint, "text-muted");
	form->addRow("", trackHint);
	layout->addLayout(form);

	issues_ = new QLabel(this);
	issues_->setWordWrap(true);
	layout->addWidget(issues_);

	auto *buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, this);
	save_ = buttons->button(QDialogButtonBox::Save);
	save_->setObjectName("mfEncodingSave");
	layout->addWidget(buttons);
	connect(buttons, &QDialogButtonBox::accepted, this, &EncoderSettingsDialog::onSave);
	connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

	for (QWidget *w : std::initializer_list<QWidget *>{videoEncoder_, videoBitrate_, keyframe_, audioEncoder_,
							   audioBitrate_, audioTrack_})
		w->setEnabled(!locked_);
	connect(videoBitrate_, &QSpinBox::valueChanged, this, &EncoderSettingsDialog::validate);
	connect(keyframe_, &QSpinBox::valueChanged, this, &EncoderSettingsDialog::validate);
	connect(audioBitrate_, &QComboBox::currentIndexChanged, this, &EncoderSettingsDialog::validate);
	connect(videoEncoder_, &QComboBox::currentIndexChanged, this, &EncoderSettingsDialog::validate);
	validate();
}

EncoderProfile EncoderSettingsDialog::collect() const
{
	EncoderProfile profile = context_.manager().config().encoder;
	profile.videoEncoderId = videoEncoder_->currentData().toString().toStdString();
	profile.videoBitrateKbps = videoBitrate_->value();
	profile.keyframeIntervalSec = keyframe_->value();
	profile.audioEncoderId = audioEncoder_->currentData().toString().toStdString();
	profile.audioBitrateKbps = audioBitrate_->currentData().toInt();
	profile.audioTrack = audioTrack_->currentData().toInt();
	return profile;
}

void EncoderSettingsDialog::validate()
{
	EncoderProfile profile = collect();
	Issues issues = validate_encoder_profile(profile);
	append(issues, validate_media(context_.manager().mediaInfo()));
	MediaInfo media = context_.manager().mediaInfo();
	for (const DestinationConfig &dest : context_.manager().config().destinations) {
		if (dest.enabled)
			append(issues,
			       check_platform_constraints(dest.platform, query_platform_constraints(dest.platform),
							  profile, media));
	}
	/* One warning per distinct message (several destinations may share a platform). */
	Issues unique;
	for (const Issue &issue : issues) {
		bool seen = false;
		for (const Issue &u : unique)
			seen = seen || u.message == issue.message;
		if (!seen)
			unique.push_back(issue);
	}
	ui::show_issues(issues_, unique);
	save_->setEnabled(!locked_ && !has_errors(validate_encoder_profile(profile)));
}

void EncoderSettingsDialog::onSave()
{
	std::string error;
	if (!context_.setEncoderProfile(collect(), error)) {
		QMessageBox::warning(this, "Cannot apply encoding settings", qstr(error));
		return;
	}
	accept();
}

} // namespace mf
