// SPDX-License-Identifier: GPL-2.0-or-later
// MatchFrame Multistream - Copyright (C) 2026 MatchFrame contributors

#include "plugin/plugin-context.hpp"
#include "ui/multistream-dock.hpp"

#include <obs-frontend-api.h>
#include <obs-module.h>
#include <plugin-support.h>

#include <QMainWindow>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE(PLUGIN_NAME, "en-US")

MODULE_EXPORT const char *obs_module_description(void)
{
	return "Sends the OBS Program feed to several RTMP/RTMPS destinations at once from one shared encoder.";
}

MODULE_EXPORT const char *obs_module_name(void)
{
	return "MatchFrame Multistream";
}

namespace {

constexpr const char *kDockId = "matchframe-multistream";

} // namespace

bool obs_module_load(void)
{
	mf::PluginContext &context = mf::PluginContext::instance();
	context.moduleLoad();

	/* Added during module load so OBS restores the dock's saved position and visibility.
	 * OBS owns the widget from here on. */
	auto *mainWindow = static_cast<QMainWindow *>(obs_frontend_get_main_window());
	auto *dock = new mf::MultistreamDock(context, mainWindow);
	if (!obs_frontend_add_dock_by_id(kDockId, obs_module_text("DockTitle"), dock)) {
		obs_log(LOG_ERROR, "could not add the dock (id already in use?)");
		delete dock;
	}

	obs_log(LOG_INFO, "plugin loaded successfully (version %s)", PLUGIN_VERSION);
	return true;
}

void obs_module_unload(void)
{
	mf::PluginContext::instance().moduleUnload();
	obs_log(LOG_INFO, "plugin unloaded");
}
