/* nd-controller.c
 *
 * Copyright 2024 GNOME Network Displays contributors
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
#include <gst/base/base.h>
#include <gst/gst.h>
#include <libportal-gtk4/portal-gtk4.h>
#include "gnome-network-displays-config.h"
#include "nd-controller.h"
#include "nd-dummy-provider.h"
#include "nd-meta-provider.h"
#include "nd-nm-device-registry.h"
#include "nd-pulseaudio.h"
#include "nd-sink-list-model.h"

#ifdef HAVE_AVAHI
#include <avahi-gobject/ga-client.h>
#include <avahi-gobject/ga-service-browser.h>
#include "nd-cc-provider.h"
#include "nd-wfd-mice-provider.h"
#endif

struct _NdController
{
  GObject              parent_instance;

#ifdef HAVE_AVAHI
  GaClient            *avahi_client;
#endif
  NdMetaProvider       *meta_provider;
  NdNMDeviceRegistry   *nm_device_registry;

  XdpPortal            *portal;
  XdpSession           *session;
  XdpSession           *new_session;       /* pending session during hot-swap */
  NdScreenCastSourceType screencast_type;
  gboolean              use_x11;

  NdPulseaudio         *pulse;

  GCancellable         *cancellable;

  NdSink               *stream_sink;
  NdSinkState           current_state;

  GstElement           *capture_pipeline;  /* standalone: pipewiresrc → intervideosink */

  NdSinkListModel      *sink_list_model;

  gchar                *screen_name;

  gboolean              audio_muted;
  GstElement           *audio_source;  /* live pulsesrc, for mute toggling */
};

enum {
  SIGNAL_SINKS_CHANGED,
  SIGNAL_STATE_CHANGED,
  SIGNAL_SCREEN_CHANGED,
  N_SIGNALS,
};

static guint signals[N_SIGNALS];

G_DEFINE_TYPE (NdController, nd_controller, G_TYPE_OBJECT)

/* Forward declarations */
static void start_streaming_to_sink (NdController *self, NdSink *sink);
static void session_closed_cb (NdController *self);

static GstElement *
nd_controller_screencast_get_source (NdController *self, XdpSession *session)
{
  g_autoptr(GVariant) stream_properties = NULL;
  GstElement *src = NULL;
  GVariant *streams = NULL;
  GVariantIter iter;
  guint32 node_id;
  guint32 screencast_type;

  if (!session)
    g_error ("XDP session not found!");

  streams = xdp_session_get_streams (session);
  if (streams == NULL)
    g_error ("XDP session streams not found!");

  g_variant_iter_init (&iter, streams);
  g_variant_iter_loop (&iter, "(u@a{sv})", &node_id, &stream_properties);
  g_variant_lookup (stream_properties, "source_type", "u", &screencast_type);

  g_debug ("Got a stream with node ID: %d", node_id);
  g_debug ("Got a stream of type: %d", screencast_type);

  switch (screencast_type)
    {
    case ND_SCREEN_CAST_SOURCE_TYPE_MONITOR:
    case ND_SCREEN_CAST_SOURCE_TYPE_WINDOW:
    case ND_SCREEN_CAST_SOURCE_TYPE_VIRTUAL:
      self->screencast_type = screencast_type;
      break;

    default:
      g_assert_not_reached ();
    }

  src = gst_element_factory_make ("pipewiresrc", "portal-pipewire-source");
  if (src == NULL)
    g_error ("GStreamer element \"pipewiresrc\" could not be created!");

  g_autofree gchar *path_str = g_strdup_printf ("%u", node_id);
  g_object_set (src,
                "fd", xdp_session_open_pipewire_remote (session),
                "path", path_str,
                "do-timestamp", TRUE,
                NULL);

  gst_base_src_set_live (GST_BASE_SRC (src), TRUE);

  return g_steal_pointer (&src);
}

static gchar *
nd_controller_extract_screen_name (XdpSession *session)
{
  GVariant *streams = xdp_session_get_streams (session);
  if (streams)
    {
      g_autoptr(GVariant) stream_properties = NULL;
      GVariantIter iter;
      guint32 node_id;
      guint32 source_type = 0;

      g_variant_iter_init (&iter, streams);
      if (g_variant_iter_loop (&iter, "(u@a{sv})", &node_id, &stream_properties))
        {
          g_variant_lookup (stream_properties, "source_type", "u", &source_type);
          switch (source_type)
            {
            case ND_SCREEN_CAST_SOURCE_TYPE_MONITOR:
              return g_strdup ("Monitor");
            case ND_SCREEN_CAST_SOURCE_TYPE_WINDOW:
              return g_strdup ("Window");
            case ND_SCREEN_CAST_SOURCE_TYPE_VIRTUAL:
              return g_strdup ("Virtual Display");
            default:
              return g_strdup ("Screen");
            }
        }
    }
  return g_strdup ("Screen");
}

static GstElement *
nd_controller_build_capture_pipeline (NdController *self, XdpSession *session)
{
  g_autoptr(GstCaps) caps = NULL;
  GstElement *pipeline, *src, *dst, *filter;

  pipeline = gst_pipeline_new ("capture-pipeline");

  if (self->use_x11)
    src = gst_element_factory_make ("ximagesrc", "X11 screencast source");
  else
    src = nd_controller_screencast_get_source (self, session);

  if (!src)
    {
      g_warning ("Error creating video source element for capture pipeline!");
      gst_object_unref (pipeline);
      return NULL;
    }

  dst = gst_element_factory_make ("intervideosink", "inter video sink");
  if (!dst)
    {
      g_warning ("Error creating intervideosink!");
      gst_object_unref (src);
      gst_object_unref (pipeline);
      return NULL;
    }
  g_object_set (dst,
                "channel", "nd-inter-video",
                "max-lateness", (gint64) - 1,
                "sync", FALSE,
                NULL);

  gst_bin_add_many (GST_BIN (pipeline), src, dst, NULL);

  if (self->screencast_type == ND_SCREEN_CAST_SOURCE_TYPE_VIRTUAL)
    {
      caps = gst_caps_new_simple ("video/x-raw",
                                  "framerate", GST_TYPE_FRACTION, 30, 1,
                                  "width", G_TYPE_INT, 1920,
                                  "height", G_TYPE_INT, 1080,
                                  NULL);
      filter = gst_element_factory_make ("capsfilter", "srcfilter");
      gst_bin_add (GST_BIN (pipeline), filter);
      g_object_set (filter, "caps", caps, NULL);
      g_clear_pointer (&caps, gst_caps_unref);

      gst_element_link_many (src, filter, dst, NULL);
    }
  else
    {
      gst_element_link_many (src, dst, NULL);
    }

  gst_element_set_state (pipeline, GST_STATE_PLAYING);

  return pipeline;
}

static GstElement *
sink_create_source_cb (NdController *self, NdSink *sink)
{
  GstBin *bin;
  GstElement *res;

  /* The capture pipeline (pipewiresrc → intervideosink) is already running
   * as a standalone pipeline. We only need to return an intervideosrc that
   * reads from the same "nd-inter-video" channel. */
  bin = GST_BIN (gst_bin_new ("screencast source bin"));

  res = gst_element_factory_make ("intervideosrc", "screencastsrc");
  g_object_set (res,
                "do-timestamp", FALSE,
                "timeout", (guint64) G_MAXUINT64,
                "channel", "nd-inter-video",
                NULL);

  gst_bin_add (bin, res);

  gst_element_add_pad (GST_ELEMENT (bin),
                       gst_ghost_pad_new ("src",
                                          gst_element_get_static_pad (res,
                                                                      "src")));

  g_object_ref_sink (bin);
  return GST_ELEMENT (bin);
}

static GstElement *
sink_create_audio_source_cb (NdController *self, NdSink *sink)
{
  GstElement *res;

  if (!self->pulse)
    return NULL;

  res = nd_pulseaudio_get_source (self->pulse);

  /* Store reference for live mute toggling */
  g_clear_object (&self->audio_source);
  self->audio_source = g_object_ref (res);

  /* Apply current mute state */
  g_object_set (res, "mute", self->audio_muted, NULL);
  g_debug ("NdController: Audio source created (muted=%d)", self->audio_muted);

  return g_object_ref_sink (res);
}

static void
sink_notify_state_cb (NdController *self, GParamSpec *pspec, NdSink *sink)
{
  NdSinkState state;

  g_object_get (sink, "state", &state, NULL);
  g_debug ("NdController: Got state change to %s",
           g_enum_to_string (ND_TYPE_SINK_STATE, state));

  self->current_state = state;

  if (state == ND_SINK_STATE_DISCONNECTED)
    {
      g_object_set (self->meta_provider, "discover", TRUE, NULL);
      g_signal_handlers_disconnect_by_data (self->stream_sink, self);
      g_clear_object (&self->stream_sink);

      /* Release audio source reference */
      g_clear_object (&self->audio_source);

      /* Stop capture pipeline */
      if (self->capture_pipeline)
        {
          gst_element_set_state (self->capture_pipeline, GST_STATE_NULL);
          g_clear_pointer (&self->capture_pipeline, gst_object_unref);
        }

      /* Close portal sessions — user must re-select screen */
      if (self->new_session)
        {
          g_signal_handlers_disconnect_by_func (self->new_session,
                                                session_closed_cb, self);
          xdp_session_close (self->new_session);
        }
      g_clear_object (&self->new_session);
      if (self->session)
        {
          g_signal_handlers_disconnect_by_func (self->session,
                                                session_closed_cb, self);
          xdp_session_close (self->session);
        }
      g_clear_object (&self->session);
      g_clear_pointer (&self->screen_name, g_free);
      g_signal_emit (self, signals[SIGNAL_SCREEN_CHANGED], 0);
    }

  g_signal_emit (self, signals[SIGNAL_STATE_CHANGED], 0, (guint) state);
}

/* --- Virtual display repositioning via Mutter DisplayConfig D-Bus --- */

static void
on_apply_monitors_config_done (GObject      *source,
                               GAsyncResult *result,
                               gpointer      user_data)
{
  g_autoptr(GError) error = NULL;
  g_autoptr(GVariant) reply = NULL;

  reply = g_dbus_connection_call_finish (G_DBUS_CONNECTION (source), result, &error);
  if (error)
    g_debug ("NdController: ApplyMonitorsConfig failed: %s", error->message);
  else
    g_debug ("NdController: Virtual display repositioned above other monitors");
}

static void
on_display_state_received (GObject      *source,
                           GAsyncResult *result,
                           gpointer      user_data)
{
  NdController *self = user_data;
  g_autoptr(GError) error = NULL;
  g_autoptr(GVariant) reply = NULL;
  GVariantBuilder lm_builder, props_builder;
  gint virtual_idx = -1;
  gint min_y = G_MAXINT, min_x = G_MAXINT, max_x = 0;
  gint virtual_height = 1080, virtual_width = 1920;

  reply = g_dbus_connection_call_finish (G_DBUS_CONNECTION (source), result, &error);
  if (!reply)
    {
      g_debug ("NdController: Could not get display config: %s", error->message);
      return;
    }

  guint32 serial;
  g_autoptr(GVariant) serial_v = g_variant_get_child_value (reply, 0);
  g_autoptr(GVariant) monitors_v = g_variant_get_child_value (reply, 1);
  g_autoptr(GVariant) logical_monitors_v = g_variant_get_child_value (reply, 2);

  serial = g_variant_get_uint32 (serial_v);

  gsize n_monitors = g_variant_n_children (monitors_v);
  gsize n_logical = g_variant_n_children (logical_monitors_v);

  if (n_logical == 0)
    return;

  /* Build connector -> (current_mode_id, width, height) mapping */
  GHashTable *connector_mode = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  GHashTable *connector_width = g_hash_table_new (g_str_hash, g_str_equal);
  GHashTable *connector_height = g_hash_table_new (g_str_hash, g_str_equal);

  /* We store mode_id strings in a GPtrArray for memory management */
  g_autoptr(GPtrArray) mode_ids = g_ptr_array_new_with_free_func (g_free);

  for (gsize i = 0; i < n_monitors; i++)
    {
      g_autoptr(GVariant) monitor = g_variant_get_child_value (monitors_v, i);
      g_autoptr(GVariant) conn_info = g_variant_get_child_value (monitor, 0);
      g_autoptr(GVariant) modes_v = g_variant_get_child_value (monitor, 1);
      g_autoptr(GVariant) conn_name_v = g_variant_get_child_value (conn_info, 0);
      const gchar *connector = g_variant_get_string (conn_name_v, NULL);

      gsize n_modes = g_variant_n_children (modes_v);
      for (gsize m = 0; m < n_modes; m++)
        {
          g_autoptr(GVariant) mode = g_variant_get_child_value (modes_v, m);
          gsize n_fields = g_variant_n_children (mode);

          g_autoptr(GVariant) id_v = g_variant_get_child_value (mode, 0);
          g_autoptr(GVariant) w_v = g_variant_get_child_value (mode, 1);
          g_autoptr(GVariant) h_v = g_variant_get_child_value (mode, 2);

          const gchar *mode_id = g_variant_get_string (id_v, NULL);
          gint w = g_variant_get_int32 (w_v);
          gint h = g_variant_get_int32 (h_v);

          /* Check if this mode is current (properties dict is 7th field, index 6) */
          gboolean is_current = FALSE;
          if (n_fields >= 7)
            {
              g_autoptr(GVariant) mode_props = g_variant_get_child_value (mode, 6);
              g_autoptr(GVariant) cur_v = g_variant_lookup_value (mode_props,
                                                                   "is-current",
                                                                   G_VARIANT_TYPE_BOOLEAN);
              if (cur_v)
                is_current = g_variant_get_boolean (cur_v);
            }

          if (is_current)
            {
              gchar *mid = g_strdup (mode_id);
              g_ptr_array_add (mode_ids, mid);
              g_hash_table_insert (connector_mode, g_strdup (connector), mid);
              g_hash_table_insert (connector_width, (gchar *) connector,
                                   GINT_TO_POINTER (w));
              g_hash_table_insert (connector_height, (gchar *) connector,
                                   GINT_TO_POINTER (h));
              break;
            }
        }
    }

  /* Parse logical monitors and find virtual display + bounding box */
  for (gsize i = 0; i < n_logical; i++)
    {
      g_autoptr(GVariant) lmon = g_variant_get_child_value (logical_monitors_v, i);
      g_autoptr(GVariant) assigned = g_variant_get_child_value (lmon, 5);

      if (g_variant_n_children (assigned) == 0)
        continue;

      g_autoptr(GVariant) first_mon = g_variant_get_child_value (assigned, 0);
      g_autoptr(GVariant) cname_v = g_variant_get_child_value (first_mon, 0);
      const gchar *connector = g_variant_get_string (cname_v, NULL);

      g_autoptr(GVariant) x_v = g_variant_get_child_value (lmon, 0);
      g_autoptr(GVariant) y_v = g_variant_get_child_value (lmon, 1);
      gint x = g_variant_get_int32 (x_v);
      gint y = g_variant_get_int32 (y_v);

      if (g_str_has_prefix (connector, "Virtual"))
        {
          virtual_idx = (gint) i;
          gpointer w_p = g_hash_table_lookup (connector_width, connector);
          gpointer h_p = g_hash_table_lookup (connector_height, connector);
          if (w_p) virtual_width = GPOINTER_TO_INT (w_p);
          if (h_p) virtual_height = GPOINTER_TO_INT (h_p);
        }
      else
        {
          gpointer w_p = g_hash_table_lookup (connector_width, connector);
          gint w = w_p ? GPOINTER_TO_INT (w_p) : 1920;

          if (x < min_x) min_x = x;
          if (y < min_y) min_y = y;
          if (x + w > max_x) max_x = x + w;
        }
    }

  if (virtual_idx < 0)
    {
      g_debug ("NdController: No virtual display found in display config");
      g_hash_table_unref (connector_mode);
      g_hash_table_unref (connector_width);
      g_hash_table_unref (connector_height);
      return;
    }

  if (min_y == G_MAXINT)
    min_y = 0;
  if (min_x == G_MAXINT)
    min_x = 0;

  /* Calculate virtual display position: centered above all physical monitors */
  gint total_physical_width = max_x - min_x;
  gint virt_x = min_x + (total_physical_width - virtual_width) / 2;
  gint virt_y = min_y - virtual_height;

  g_debug ("NdController: Repositioning virtual display to (%d, %d) [%dx%d above physical monitors]",
           virt_x, virt_y, virtual_width, virtual_height);

  /* Build ApplyMonitorsConfig logical_monitors: a(iiduba(ssa{sv})) */
  g_variant_builder_init (&lm_builder, G_VARIANT_TYPE ("a(iiduba(ssa{sv}))"));

  for (gsize i = 0; i < n_logical; i++)
    {
      g_autoptr(GVariant) lmon = g_variant_get_child_value (logical_monitors_v, i);
      g_autoptr(GVariant) assigned = g_variant_get_child_value (lmon, 5);

      if (g_variant_n_children (assigned) == 0)
        continue;

      g_autoptr(GVariant) ox_v = g_variant_get_child_value (lmon, 0);
      g_autoptr(GVariant) oy_v = g_variant_get_child_value (lmon, 1);
      g_autoptr(GVariant) scale_v = g_variant_get_child_value (lmon, 2);
      g_autoptr(GVariant) transform_v = g_variant_get_child_value (lmon, 3);
      g_autoptr(GVariant) primary_v = g_variant_get_child_value (lmon, 4);

      gint ox = g_variant_get_int32 (ox_v);
      gint oy = g_variant_get_int32 (oy_v);
      gdouble scale = g_variant_get_double (scale_v);
      guint32 transform = g_variant_get_uint32 (transform_v);
      gboolean is_primary = g_variant_get_boolean (primary_v);

      /* Override position for virtual display */
      if ((gint) i == virtual_idx)
        {
          ox = virt_x;
          oy = virt_y;
        }

      /* Build monitors sub-array for this logical monitor */
      GVariantBuilder mons_builder;
      g_variant_builder_init (&mons_builder, G_VARIANT_TYPE ("a(ssa{sv})"));

      gsize n_assigned = g_variant_n_children (assigned);
      for (gsize j = 0; j < n_assigned; j++)
        {
          g_autoptr(GVariant) asgn_mon = g_variant_get_child_value (assigned, j);
          g_autoptr(GVariant) asgn_conn_v = g_variant_get_child_value (asgn_mon, 0);
          const gchar *asgn_conn = g_variant_get_string (asgn_conn_v, NULL);

          const gchar *mode_id = g_hash_table_lookup (connector_mode, asgn_conn);
          if (!mode_id)
            {
              g_debug ("NdController: No current mode for connector %s, skipping", asgn_conn);
              continue;
            }

          GVariantBuilder mon_props;
          g_variant_builder_init (&mon_props, G_VARIANT_TYPE ("a{sv}"));

          g_variant_builder_add (&mons_builder, "(ssa{sv})",
                                 asgn_conn, mode_id, &mon_props);
        }

      g_variant_builder_add (&lm_builder, "(iiduba(ssa{sv}))",
                             ox, oy, scale, transform, is_primary,
                             &mons_builder);
    }

  g_variant_builder_init (&props_builder, G_VARIANT_TYPE ("a{sv}"));

  /* Apply with method=1 (temporary, reverts after 20s if not confirmed) */
  g_dbus_connection_call (G_DBUS_CONNECTION (source),
                          "org.gnome.Mutter.DisplayConfig",
                          "/org/gnome/Mutter/DisplayConfig",
                          "org.gnome.Mutter.DisplayConfig",
                          "ApplyMonitorsConfig",
                          g_variant_new ("(uua(iiduba(ssa{sv}))a{sv})",
                                         serial,
                                         (guint32) 1,
                                         &lm_builder,
                                         &props_builder),
                          NULL,
                          G_DBUS_CALL_FLAGS_NONE,
                          -1,
                          self->cancellable,
                          on_apply_monitors_config_done,
                          self);

  g_hash_table_unref (connector_mode);
  g_hash_table_unref (connector_width);
  g_hash_table_unref (connector_height);
}

static gboolean
reposition_virtual_display_timeout (gpointer user_data)
{
  NdController *self = user_data;
  g_autoptr(GError) error = NULL;
  GDBusConnection *conn;

  conn = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, &error);
  if (!conn)
    {
      g_debug ("NdController: Cannot get session bus for display config: %s",
               error->message);
      return G_SOURCE_REMOVE;
    }

  g_dbus_connection_call (conn,
                          "org.gnome.Mutter.DisplayConfig",
                          "/org/gnome/Mutter/DisplayConfig",
                          "org.gnome.Mutter.DisplayConfig",
                          "GetCurrentState",
                          NULL,
                          NULL,
                          G_DBUS_CALL_FLAGS_NONE,
                          -1,
                          self->cancellable,
                          on_display_state_received,
                          self);

  g_object_unref (conn);
  return G_SOURCE_REMOVE;
}

static void
nd_screencast_started_cb (GObject      *source_object,
                          GAsyncResult *result,
                          gpointer      user_data)
{
  g_autoptr(GError) error = NULL;
  XdpSession *session = XDP_SESSION (source_object);
  NdController *self = ND_CONTROLLER (user_data);
  gboolean is_swap = (session == self->new_session);

  if (!xdp_session_start_finish (session, result, &error))
    {
      if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        {
          g_warning ("Error initializing screencast portal: %s", error->message);

          if (g_error_matches (error, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_METHOD))
            g_warning ("Screencasting portal is unavailable!");

          if (!is_swap)
            {
              g_warning ("Falling back to X11!");
              self->use_x11 = TRUE;
            }
        }

      g_warning ("Failed to start screencast session: %s", error->message);

      if (is_swap)
        {
          /* Swap failed — discard new session, keep old capture running */
          xdp_session_close (self->new_session);
          g_clear_object (&self->new_session);
          g_debug ("NdController: Screen swap cancelled/failed, keeping current screen");
        }
      else
        {
          g_clear_object (&self->session);
          g_signal_emit (self, signals[SIGNAL_SCREEN_CHANGED], 0);
        }
      return;
    }

  g_debug ("NdController: Screencast session started — screen selected (swap=%d)", is_swap);

  if (is_swap)
    {
      /* Hot-swap: build new capture pipeline, stop old, swap sessions */
      GstElement *new_capture = nd_controller_build_capture_pipeline (self, self->new_session);
      if (!new_capture)
        {
          g_warning ("NdController: Failed to build new capture pipeline, keeping old");
          xdp_session_close (self->new_session);
          g_clear_object (&self->new_session);
          return;
        }

      /* Stop old capture pipeline */
      if (self->capture_pipeline)
        {
          gst_element_set_state (self->capture_pipeline, GST_STATE_NULL);
          g_clear_pointer (&self->capture_pipeline, gst_object_unref);
        }

      /* Disconnect "closed" signal from old session before closing it,
       * otherwise session_closed_cb will tear down the RTSP stream. */
      if (self->session)
        {
          g_signal_handlers_disconnect_by_func (self->session,
                                                session_closed_cb, self);
          xdp_session_close (self->session);
        }
      g_clear_object (&self->session);

      self->session = g_steal_pointer (&self->new_session);
      self->capture_pipeline = new_capture;

      /* Update screen name */
      g_clear_pointer (&self->screen_name, g_free);
      self->screen_name = nd_controller_extract_screen_name (self->session);

      /* Reposition virtual display above physical monitors */
      if (self->screencast_type == ND_SCREEN_CAST_SOURCE_TYPE_VIRTUAL)
        g_timeout_add (500, reposition_virtual_display_timeout, self);

      g_signal_emit (self, signals[SIGNAL_SCREEN_CHANGED], 0);
    }
  else
    {
      /* Initial screen selection — build and start capture pipeline */
      g_clear_pointer (&self->screen_name, g_free);
      self->screen_name = nd_controller_extract_screen_name (self->session);

      /* Build capture pipeline */
      self->capture_pipeline = nd_controller_build_capture_pipeline (self, self->session);
      if (!self->capture_pipeline)
        g_warning ("NdController: Failed to build capture pipeline!");

      /* Reposition virtual display above physical monitors */
      if (self->screencast_type == ND_SCREEN_CAST_SOURCE_TYPE_VIRTUAL)
        g_timeout_add (500, reposition_virtual_display_timeout, self);

      g_signal_emit (self, signals[SIGNAL_SCREEN_CHANGED], 0);
    }
}

static void
session_closed_cb (NdController *self)
{
  g_debug ("NdController: Session closed");

  /* If the capture pipeline is still running (e.g. virtual display sessions
   * close immediately after PipeWire FD is obtained), keep the screen
   * selection state intact so the user can still connect to sinks. */
  if (self->capture_pipeline)
    {
      g_debug ("NdController: Capture pipeline still active, keeping screen state");
      g_clear_object (&self->session);
      return;
    }

  if (self->stream_sink)
    nd_sink_stop_stream (self->stream_sink);

  g_clear_object (&self->session);
  g_clear_pointer (&self->screen_name, g_free);
  g_signal_emit (self, signals[SIGNAL_SCREEN_CHANGED], 0);
}

static void
nd_screencast_init_cb (GObject      *source_object,
                       GAsyncResult *result,
                       gpointer      user_data)
{
  g_autoptr(GError) error = NULL;
  NdController *self = ND_CONTROLLER (user_data);
  XdpSession *new_session;
  gboolean is_swap;

  new_session = xdp_portal_create_screencast_session_finish (self->portal, result, &error);
  if (new_session == NULL)
    {
      g_warning ("Failed to create screencast session: %s", error->message);
      /* If this was a swap attempt, just keep old session running */
      if (self->stream_sink)
        {
          g_debug ("NdController: Swap session creation failed, keeping current screen");
          return;
        }
      self->use_x11 = TRUE;
      return;
    }

  /* Determine if this is a hot-swap (streaming) or initial selection */
  is_swap = (self->stream_sink != NULL);

  if (is_swap)
    self->new_session = new_session;
  else
    self->session = new_session;

  g_signal_connect_object (new_session,
                           "closed",
                           (GCallback) session_closed_cb,
                           self,
                           G_CONNECT_SWAPPED);

  /* Start session — NULL parent shows screen picker as floating dialog */
  xdp_session_start (new_session, NULL, self->cancellable,
                     nd_screencast_started_cb, self);
}

static void
start_streaming_to_sink (NdController *self, NdSink *sink)
{
  self->stream_sink = nd_sink_start_stream (sink);

  if (!self->stream_sink)
    {
      g_warning ("NdController: Could not start streaming!");
      return;
    }

  g_signal_connect_object (self->stream_sink,
                           "create-source",
                           (GCallback) sink_create_source_cb,
                           self,
                           G_CONNECT_SWAPPED);

  g_signal_connect_object (self->stream_sink,
                           "create-audio-source",
                           (GCallback) sink_create_audio_source_cb,
                           self,
                           G_CONNECT_SWAPPED);

  g_signal_connect_object (self->stream_sink,
                           "notify::state",
                           (GCallback) sink_notify_state_cb,
                           self,
                           G_CONNECT_SWAPPED);

  /* Check state immediately in case we already moved to error */
  sink_notify_state_cb (self, NULL, self->stream_sink);

  g_object_set (self->meta_provider, "discover", FALSE, NULL);
}

static void
sink_list_items_changed_cb (GListModel   *model,
                            guint         position,
                            guint         removed,
                            guint         added,
                            NdController *self)
{
  g_signal_emit (self, signals[SIGNAL_SINKS_CHANGED], 0);
}

static void
nd_pulseaudio_init_async_cb (GObject      *source_object,
                             GAsyncResult *res,
                             gpointer      user_data)
{
  NdController *self;
  g_autoptr(GError) error = NULL;

  if (!g_async_initable_init_finish (G_ASYNC_INITABLE (source_object), res, &error))
    {
      if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        g_warning ("Error initializing pulse audio sink: %s", error->message);

      g_object_unref (source_object);
      return;
    }

  self = ND_CONTROLLER (user_data);
  self->pulse = ND_PULSEAUDIO (source_object);
}

static void
nd_controller_constructed (GObject *obj)
{
  NdController *self = ND_CONTROLLER (obj);
  g_autoptr(GError) error = NULL;
  NdPulseaudio *pulse;

  G_OBJECT_CLASS (nd_controller_parent_class)->constructed (obj);

  g_debug ("desktopCast v%s started (tray-only mode)", PACKAGE_VERSION);

  self->cancellable = g_cancellable_new ();

  self->meta_provider = nd_meta_provider_new ();
  self->nm_device_registry = nd_nm_device_registry_new (self->meta_provider);

  if (g_strcmp0 (g_getenv ("NETWORK_DISPLAYS_DUMMY"), "1") == 0)
    {
      g_autoptr(NdDummyProvider) dummy_provider = NULL;

      g_debug ("Adding dummy provider");
      dummy_provider = nd_dummy_provider_new ();
      nd_meta_provider_add_provider (self->meta_provider, ND_PROVIDER (dummy_provider));
    }

#ifdef HAVE_AVAHI
  {
    g_autoptr(NdWFDMiceProvider) mice_provider = NULL;
    g_autoptr(NdCCProvider) cc_provider = NULL;

    self->avahi_client = ga_client_new (GA_CLIENT_FLAG_NO_FLAGS);
    if (!ga_client_start (self->avahi_client, &error))
      {
        g_warning ("NdController: Failed to start Avahi Client");
        if (error != NULL)
          g_warning ("NdController: Error: %s", error->message);
        return;
      }

    g_debug ("NdController: Got avahi client");

    mice_provider = nd_wfd_mice_provider_new (self->avahi_client);
    cc_provider = nd_cc_provider_new (self->avahi_client);

    if (!nd_wfd_mice_provider_browse (mice_provider, error) || !nd_cc_provider_browse (cc_provider, error))
      {
        g_warning ("NdController: Avahi client failed to browse: %s", error->message);
        return;
      }

    g_debug ("NdController: Got avahi browser");
    nd_meta_provider_add_provider (self->meta_provider, ND_PROVIDER (mice_provider));
    nd_meta_provider_add_provider (self->meta_provider, ND_PROVIDER (cc_provider));

    g_debug ("NdController: Added avahi providers");
  }
#endif

  self->sink_list_model = nd_sink_list_model_new (ND_PROVIDER (self->meta_provider));

  g_signal_connect (G_LIST_MODEL (self->sink_list_model),
                    "items-changed",
                    G_CALLBACK (sink_list_items_changed_cb),
                    self);

  /* Initialize portal (session created on-demand via nd_controller_select_screen) */
  self->portal = xdp_portal_initable_new (&error);
  if (error)
    {
      g_warning ("Failed to create screencast portal: %s", error->message);
      self->use_x11 = TRUE;
      g_clear_object (&self->portal);
    }

  /* Initialize pulseaudio */
  pulse = nd_pulseaudio_new ("GNOME Network Displays", "gnome-network-displays");
  g_async_initable_init_async (G_ASYNC_INITABLE (pulse),
                               G_PRIORITY_LOW,
                               self->cancellable,
                               nd_pulseaudio_init_async_cb,
                               self);
}

static void
nd_controller_finalize (GObject *obj)
{
  NdController *self = ND_CONTROLLER (obj);

  g_cancellable_cancel (self->cancellable);
  g_clear_object (&self->cancellable);

  if (self->pulse)
    nd_pulseaudio_unload (self->pulse);
  g_clear_object (&self->pulse);

  g_clear_object (&self->audio_source);
  g_clear_object (&self->stream_sink);
  g_clear_pointer (&self->screen_name, g_free);

  if (self->capture_pipeline)
    {
      gst_element_set_state (self->capture_pipeline, GST_STATE_NULL);
      g_clear_pointer (&self->capture_pipeline, gst_object_unref);
    }

  if (self->new_session)
    {
      g_signal_handlers_disconnect_by_func (self->new_session,
                                            session_closed_cb, self);
      xdp_session_close (self->new_session);
    }
  g_clear_object (&self->new_session);
  if (self->session)
    {
      g_signal_handlers_disconnect_by_func (self->session,
                                            session_closed_cb, self);
      xdp_session_close (self->session);
    }
  g_clear_object (&self->session);
  g_clear_object (&self->portal);

  g_clear_object (&self->sink_list_model);
  g_clear_object (&self->meta_provider);
  g_clear_object (&self->nm_device_registry);

#ifdef HAVE_AVAHI
  if (self->avahi_client)
    g_object_run_dispose (G_OBJECT (self->avahi_client));
  g_clear_object (&self->avahi_client);
#endif

  G_OBJECT_CLASS (nd_controller_parent_class)->finalize (obj);
}

static void
nd_controller_class_init (NdControllerClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->constructed = nd_controller_constructed;
  object_class->finalize = nd_controller_finalize;

  signals[SIGNAL_SINKS_CHANGED] =
    g_signal_new ("sinks-changed",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 0);

  signals[SIGNAL_STATE_CHANGED] =
    g_signal_new ("state-changed",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 1, G_TYPE_UINT);

  signals[SIGNAL_SCREEN_CHANGED] =
    g_signal_new ("screen-changed",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 0);
}

static void
nd_controller_init (NdController *self)
{
  self->current_state = ND_SINK_STATE_DISCONNECTED;
}

NdController *
nd_controller_new (void)
{
  return g_object_new (ND_TYPE_CONTROLLER, NULL);
}

guint
nd_controller_get_n_sinks (NdController *self)
{
  g_return_val_if_fail (ND_IS_CONTROLLER (self), 0);

  return g_list_model_get_n_items (G_LIST_MODEL (self->sink_list_model));
}

NdSink *
nd_controller_get_sink (NdController *self, guint index)
{
  g_return_val_if_fail (ND_IS_CONTROLLER (self), NULL);

  return g_list_model_get_item (G_LIST_MODEL (self->sink_list_model), index);
}

void
nd_controller_connect_sink (NdController *self, NdSink *sink)
{
  g_return_if_fail (ND_IS_CONTROLLER (self));
  g_return_if_fail (ND_IS_SINK (sink));

  if (self->stream_sink)
    {
      g_warning ("NdController: Already streaming, disconnect first");
      return;
    }

  if (!self->use_x11 && !self->session && !self->capture_pipeline)
    {
      g_warning ("NdController: No screen selected — call select_screen first");
      return;
    }

  start_streaming_to_sink (self, sink);
}

void
nd_controller_disconnect (NdController *self)
{
  g_return_if_fail (ND_IS_CONTROLLER (self));

  if (self->stream_sink)
    nd_sink_stop_stream (self->stream_sink);
}

gboolean
nd_controller_is_streaming (NdController *self)
{
  g_return_val_if_fail (ND_IS_CONTROLLER (self), FALSE);

  return self->current_state == ND_SINK_STATE_STREAMING;
}

NdSinkState
nd_controller_get_state (NdController *self)
{
  g_return_val_if_fail (ND_IS_CONTROLLER (self), ND_SINK_STATE_DISCONNECTED);

  return self->current_state;
}

NdSink *
nd_controller_get_stream_sink (NdController *self)
{
  g_return_val_if_fail (ND_IS_CONTROLLER (self), NULL);

  return self->stream_sink;
}

void
nd_controller_select_screen (NdController *self)
{
  g_return_if_fail (ND_IS_CONTROLLER (self));

  if (!self->portal)
    {
      g_warning ("NdController: No portal available for screen selection");
      return;
    }

  if (self->stream_sink)
    {
      /* Streaming — hot-swap: keep old session + capture alive,
       * create a new session that will swap in on success */
      g_debug ("NdController: Hot-swap screen selection while streaming");
      /* Cancel any pending swap */
      if (self->new_session)
        {
          g_signal_handlers_disconnect_by_func (self->new_session,
                                                session_closed_cb, self);
          xdp_session_close (self->new_session);
          g_clear_object (&self->new_session);
        }
    }
  else
    {
      /* Not streaming — close existing session and capture pipeline */
      if (self->capture_pipeline)
        {
          gst_element_set_state (self->capture_pipeline, GST_STATE_NULL);
          g_clear_pointer (&self->capture_pipeline, gst_object_unref);
        }
      if (self->session)
        {
          g_signal_handlers_disconnect_by_func (self->session,
                                                session_closed_cb, self);
          xdp_session_close (self->session);
          g_clear_object (&self->session);
          g_clear_pointer (&self->screen_name, g_free);
        }
    }

  xdp_portal_create_screencast_session (self->portal,
                                        XDP_OUTPUT_MONITOR | XDP_OUTPUT_WINDOW | XDP_OUTPUT_VIRTUAL,
                                        XDP_SCREENCAST_FLAG_NONE,
                                        XDP_CURSOR_MODE_EMBEDDED,
                                        XDP_PERSIST_MODE_NONE,
                                        NULL,
                                        self->cancellable,
                                        nd_screencast_init_cb,
                                        self);
}

gboolean
nd_controller_has_screen (NdController *self)
{
  g_return_val_if_fail (ND_IS_CONTROLLER (self), FALSE);

  return self->session != NULL || self->capture_pipeline != NULL;
}

const gchar *
nd_controller_get_screen_name (NdController *self)
{
  g_return_val_if_fail (ND_IS_CONTROLLER (self), NULL);

  return self->screen_name;
}

void
nd_controller_set_muted (NdController *self, gboolean muted)
{
  g_return_if_fail (ND_IS_CONTROLLER (self));

  self->audio_muted = muted;

  /* Apply to live audio source if streaming */
  if (self->audio_source)
    g_object_set (self->audio_source, "mute", muted, NULL);

  g_debug ("NdController: Audio %s (live=%s)",
           muted ? "muted" : "unmuted",
           self->audio_source ? "yes" : "no");
}

gboolean
nd_controller_get_muted (NdController *self)
{
  g_return_val_if_fail (ND_IS_CONTROLLER (self), FALSE);

  return self->audio_muted;
}

