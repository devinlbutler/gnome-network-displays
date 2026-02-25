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
  NdScreenCastSourceType screencast_type;
  gboolean              use_x11;

  NdPulseaudio         *pulse;

  GCancellable         *cancellable;

  NdSink               *stream_sink;
  NdSinkState           current_state;
  NdSink               *pending_sink;

  NdSinkListModel      *sink_list_model;
};

enum {
  SIGNAL_SINKS_CHANGED,
  SIGNAL_STATE_CHANGED,
  N_SIGNALS,
};

static guint signals[N_SIGNALS];

G_DEFINE_TYPE (NdController, nd_controller, G_TYPE_OBJECT)

/* Forward declarations */
static void start_streaming_to_sink (NdController *self, NdSink *sink);

static GstElement *
nd_controller_screencast_get_source (NdController *self)
{
  g_autoptr(GVariant) stream_properties = NULL;
  GstElement *src = NULL;
  GVariant *streams = NULL;
  GVariantIter iter;
  guint32 node_id;
  guint32 screencast_type;

  if (!self->session)
    g_error ("XDP session not found!");

  streams = xdp_session_get_streams (self->session);
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
                "fd", xdp_session_open_pipewire_remote (self->session),
                "path", path_str,
                "do-timestamp", TRUE,
                NULL);

  gst_base_src_set_live (GST_BASE_SRC (src), TRUE);

  return g_steal_pointer (&src);
}

static GstElement *
sink_create_source_cb (NdController *self, NdSink *sink)
{
  g_autoptr(GstCaps) caps = NULL;
  GstBin *bin;
  GstElement *src, *filter, *dst, *res;

  bin = GST_BIN (gst_bin_new ("screencast source bin"));
  g_debug ("use x11: %d", self->use_x11);
  if (self->use_x11)
    src = gst_element_factory_make ("ximagesrc", "X11 screencast source");
  else
    src = nd_controller_screencast_get_source (self);

  if (!src)
    g_error ("Error creating video source element, likely a missing dependency!");

  gst_bin_add (bin, src);

  dst = gst_element_factory_make ("intervideosink", "inter video sink");
  if (!dst)
    g_error ("Error creating intervideosink, missing dependency!");
  g_object_set (dst,
                "channel", "nd-inter-video",
                "max-lateness", (gint64) - 1,
                "sync", FALSE,
                NULL);
  gst_bin_add (bin, dst);

  if (self->screencast_type == ND_SCREEN_CAST_SOURCE_TYPE_VIRTUAL)
    {
      caps = gst_caps_new_simple ("video/x-raw",
                                  "max-framerate", GST_TYPE_FRACTION, 30, 1,
                                  "width", G_TYPE_INT, 1920,
                                  "height", G_TYPE_INT, 1080,
                                  NULL);
      filter = gst_element_factory_make ("capsfilter", "srcfilter");
      gst_bin_add (bin, filter);
      g_object_set (filter,
                    "caps", caps,
                    NULL);
      g_clear_pointer (&caps, gst_caps_unref);

      gst_element_link_many (src, filter, dst, NULL);
    }
  else
    gst_element_link_many (src, dst, NULL);

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
    }

  g_signal_emit (self, signals[SIGNAL_STATE_CHANGED], 0, (guint) state);
}

static void
nd_screencast_started_cb (GObject      *source_object,
                          GAsyncResult *result,
                          gpointer      user_data)
{
  g_autoptr(GError) error = NULL;
  XdpSession *session = XDP_SESSION (source_object);
  NdController *self = ND_CONTROLLER (user_data);

  if (!xdp_session_start_finish (session, result, &error))
    {
      if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        {
          g_warning ("Error initializing screencast portal: %s", error->message);

          if (g_error_matches (error, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_METHOD))
            g_warning ("Screencasting portal is unavailable!");

          g_warning ("Falling back to X11!");
          self->use_x11 = TRUE;
        }

      g_warning ("Failed to start screencast session: %s", error->message);
      g_clear_object (&self->pending_sink);
      return;
    }

  g_debug ("NdController: Screencast session started");

  /* If we have a pending sink, start streaming to it now */
  if (self->pending_sink)
    {
      NdSink *sink = g_steal_pointer (&self->pending_sink);
      start_streaming_to_sink (self, sink);
    }
}

static void
session_closed_cb (NdController *self)
{
  g_debug ("NdController: Session closed");
  if (self->stream_sink)
    nd_sink_stop_stream (self->stream_sink);

  g_clear_object (&self->session);
}

static void
nd_screencast_init_cb (GObject      *source_object,
                       GAsyncResult *result,
                       gpointer      user_data)
{
  g_autoptr(GError) error = NULL;
  NdController *self = ND_CONTROLLER (user_data);

  self->session = xdp_portal_create_screencast_session_finish (self->portal, result, &error);
  if (self->session == NULL)
    {
      g_warning ("Failed to create screencast session: %s", error->message);
      self->use_x11 = TRUE;
      g_clear_object (&self->pending_sink);
      return;
    }

  g_signal_connect_object (self->session,
                           "closed",
                           (GCallback) session_closed_cb,
                           self,
                           G_CONNECT_SWAPPED);

  /* Start session — NULL parent shows screen picker as floating dialog */
  xdp_session_start (self->session, NULL, self->cancellable,
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

  /* Initialize portal and eagerly create screencast session */
  self->portal = xdp_portal_initable_new (&error);
  if (error)
    {
      g_warning ("Failed to create screencast portal: %s", error->message);
      self->use_x11 = TRUE;
      g_clear_object (&self->portal);
    }

  if (self->portal)
    {
      xdp_portal_create_screencast_session (self->portal,
                                            XDP_OUTPUT_MONITOR | XDP_OUTPUT_WINDOW | XDP_OUTPUT_VIRTUAL,
                                            XDP_SCREENCAST_FLAG_NONE,
                                            XDP_CURSOR_MODE_EMBEDDED,
                                            XDP_PERSIST_MODE_NONE,
                                            NULL,
                                            self->cancellable,
                                            nd_screencast_init_cb,
                                            self);
      g_debug ("NdController: Creating portal session eagerly");
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

  g_clear_object (&self->pending_sink);
  g_clear_object (&self->stream_sink);

  if (self->session)
    xdp_session_close (self->session);
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

  /* If already streaming, disconnect first */
  if (self->stream_sink)
    {
      g_warning ("NdController: Already streaming, disconnect first");
      return;
    }

  /* If we need a portal session and don't have one, create it (deferred) */
  if (!self->use_x11 && !self->portal)
    {
      g_warning ("NdController: Cannot start streaming — no portal available!");
      return;
    }

  if (!self->use_x11 && self->portal && !self->session)
    {
      g_debug ("NdController: Creating portal session (deferred connect)");
      self->pending_sink = g_object_ref (sink);
      xdp_portal_create_screencast_session (self->portal,
                                            XDP_OUTPUT_MONITOR | XDP_OUTPUT_WINDOW | XDP_OUTPUT_VIRTUAL,
                                            XDP_SCREENCAST_FLAG_NONE,
                                            XDP_CURSOR_MODE_EMBEDDED,
                                            XDP_PERSIST_MODE_NONE,
                                            NULL,
                                            self->cancellable,
                                            nd_screencast_init_cb,
                                            self);
      return;
    }

  start_streaming_to_sink (self, sink);
}

void
nd_controller_disconnect (NdController *self)
{
  g_return_if_fail (ND_IS_CONTROLLER (self));

  g_clear_object (&self->pending_sink);

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
