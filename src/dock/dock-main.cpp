/* Registers the Kagee dock (tabbed control panel) with the OBS frontend. */
#include "kagee-dock.hpp"

#include <QMainWindow>
#include <QPointer>

static QPointer<KageeDock> dock_widget;

static void frontend_event(enum obs_frontend_event event, void *)
{
	if (dock_widget)
		dock_widget->frontendEvent(event);
}

extern "C" void kagee_dock_load(void)
{
	auto *main = static_cast<QMainWindow *>(obs_frontend_get_main_window());
	if (!main)
		return; /* no UI (e.g. headless use of libobs) */
	dock_widget = new KageeDock(main);
	obs_frontend_add_dock_by_id("kagee_dock", obs_module_text("Dock.Title"), dock_widget);
	obs_frontend_add_event_callback(frontend_event, nullptr);
}

extern "C" void kagee_dock_unload(void)
{
	obs_frontend_remove_event_callback(frontend_event, nullptr);
}
