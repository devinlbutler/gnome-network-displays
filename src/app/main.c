/* main.c
 *
 * Copyright 2018 Benjamin Berg
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <glib/gi18n.h>
#include <gst/gst.h>
#include <gtk/gtk.h>
#include "gnome-network-displays-config.h"
#include "nd-controller.h"
#include "nd-debug-log.h"
#include "nd-tray.h"

static NdTray       *tray = NULL;
static NdController *controller = NULL;

static void
on_activate (GtkApplication *app)
{
  g_debug ("Activate: tray-only mode, no window to show");
}

static void
on_startup (GApplication *app)
{
  /* Keep the application alive (no window) */
  g_application_hold (app);

  /* Create headless controller (owns providers, portal, pulse) */
  controller = nd_controller_new ();

  /* Create tray icon driven by controller */
  tray = nd_tray_new (app, controller);
}

static void
on_shutdown (GApplication *app)
{
  /* Stop any active stream */
  if (controller)
    nd_controller_disconnect (controller);

  nd_tray_destroy (tray);
  tray = NULL;

  g_clear_object (&controller);

  nd_debug_log_shutdown ();
}

int
main (int   argc,
      char *argv[])
{
  g_autoptr(GtkApplication) app = NULL;

  /* Set up gettext translations */
  bindtextdomain (GETTEXT_PACKAGE, LOCALEDIR);
  bind_textdomain_codeset (GETTEXT_PACKAGE, "UTF-8");
  textdomain (GETTEXT_PACKAGE);

  gst_init (&argc, &argv);

  /* Initialize debug log capture (before anything else logs) */
  nd_debug_log_init ();

#if GLIB_CHECK_VERSION (2, 74, 0)
  app = gtk_application_new ("com.desktopcast.DesktopCast", G_APPLICATION_DEFAULT_FLAGS);
#else
  app = gtk_application_new ("com.desktopcast.DesktopCast", G_APPLICATION_FLAGS_NONE);
#endif

  g_set_application_name (_("desktopCast"));

  g_signal_connect (app, "startup", G_CALLBACK (on_startup), NULL);
  g_signal_connect (app, "activate", G_CALLBACK (on_activate), NULL);
  g_signal_connect (app, "shutdown", G_CALLBACK (on_shutdown), NULL);

  return g_application_run (G_APPLICATION (app), argc, argv);
}
