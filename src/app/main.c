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

#include <adwaita.h>
#include <glib/gi18n.h>
#include <gst/gst.h>
#include "gnome-network-displays-config.h"
#include "nd-window.h"
#include "nd-tray.h"

static NdTray *tray = NULL;

static gboolean
on_close_request (GtkWindow *window,
                  gpointer   user_data)
{
  NdWindow *nd_window = ND_WINDOW (window);

  if (nd_window_is_streaming (nd_window))
    {
      gtk_widget_set_visible (GTK_WIDGET (window), FALSE);
      return TRUE;
    }

  return FALSE;
}

static void
on_activate (AdwApplication *app)
{
  GtkWindow *window;

  g_assert (GTK_IS_APPLICATION (app));

  /* If a window already exists, just show and present it */
  window = gtk_application_get_active_window (GTK_APPLICATION (app));
  if (window)
    {
      gtk_widget_set_visible (GTK_WIDGET (window), TRUE);
      gtk_window_present (window);
      return;
    }

  window = g_object_new (ND_TYPE_WINDOW,
                         "application", app,
                         NULL);

  g_signal_connect (window, "close-request",
                    G_CALLBACK (on_close_request), NULL);

  gtk_window_present (window);
}

static void
on_disconnect_activated (GSimpleAction *action,
                         GVariant      *parameter,
                         gpointer       user_data)
{
  GApplication *app = G_APPLICATION (user_data);
  GtkWindow *window;

  window = gtk_application_get_active_window (GTK_APPLICATION (app));
  if (window)
    {
      NdSink *sink = nd_window_get_stream_sink (ND_WINDOW (window));
      if (sink)
        nd_sink_stop_stream (sink);
    }
}

static void
on_streaming_state_changed (GSimpleAction *action,
                            GVariant      *value,
                            gpointer       user_data)
{
  gboolean streaming = g_variant_get_boolean (value);

  g_simple_action_set_state (action, value);
  nd_tray_set_streaming (tray, streaming);
}

static void
on_startup (GApplication *app)
{
  GSimpleAction *disconnect_action;
  GSimpleAction *streaming_action;

  /* Keep the application alive when the window is hidden */
  g_application_hold (app);

  /* Create the tray icon */
  tray = nd_tray_new (app);

  /* Register the disconnect action */
  disconnect_action = g_simple_action_new ("disconnect", NULL);
  g_signal_connect (disconnect_action, "activate",
                    G_CALLBACK (on_disconnect_activated), app);
  g_action_map_add_action (G_ACTION_MAP (app), G_ACTION (disconnect_action));
  g_object_unref (disconnect_action);

  /* Register a stateful boolean action for streaming state bridge */
  streaming_action = g_simple_action_new_stateful ("streaming-state",
                                                    NULL,
                                                    g_variant_new_boolean (FALSE));
  g_signal_connect (streaming_action, "change-state",
                    G_CALLBACK (on_streaming_state_changed), NULL);
  g_action_map_add_action (G_ACTION_MAP (app), G_ACTION (streaming_action));
  g_object_unref (streaming_action);
}

static void
on_shutdown (GApplication *app)
{
  nd_tray_destroy (tray);
  tray = NULL;
}

int
main (int   argc,
      char *argv[])
{
  g_autoptr(AdwApplication) app = NULL;

  /* Set up gettext translations */
  bindtextdomain (GETTEXT_PACKAGE, LOCALEDIR);
  bind_textdomain_codeset (GETTEXT_PACKAGE, "UTF-8");
  textdomain (GETTEXT_PACKAGE);

  gst_init (&argc, &argv);

#if GLIB_CHECK_VERSION (2, 74, 0)
  app = adw_application_new ("org.gnome.NetworkDisplays", G_APPLICATION_DEFAULT_FLAGS);
#else
  app = adw_application_new ("org.gnome.NetworkDisplays", G_APPLICATION_FLAGS_NONE);
#endif

  g_set_application_name (_("GNOME Network Displays"));

  g_signal_connect (app, "startup", G_CALLBACK (on_startup), NULL);
  g_signal_connect (app, "activate", G_CALLBACK (on_activate), NULL);
  g_signal_connect (app, "shutdown", G_CALLBACK (on_shutdown), NULL);

  return g_application_run (G_APPLICATION (app), argc, argv);
}
