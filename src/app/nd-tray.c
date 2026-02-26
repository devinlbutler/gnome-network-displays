/* nd-tray.c
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

#include "nd-tray.h"
#include "nd-controller.h"
#include "nd-debug-log.h"
#include <gio/gio.h>

/* Menu item IDs */
#define MENU_ID_DISCONNECT    2
#define MENU_ID_QUIT          3
#define MENU_ID_CANCEL        4
#define MENU_ID_SELECT_SCREEN 5
#define MENU_ID_DEBUG_TOGGLE  6
#define MENU_ID_SAVE_LOG      7
#define MENU_ID_MUTE_TOGGLE   8
#define MENU_ID_SEPARATOR_1   90
#define MENU_ID_SEPARATOR_2   91
#define MENU_ID_SINK_BASE     100

/* StatusNotifierItem D-Bus interface */
static const gchar sni_introspection_xml[] =
  "<node>"
  "  <interface name='org.kde.StatusNotifierItem'>"
  "    <method name='Activate'>"
  "      <arg type='i' name='x' direction='in'/>"
  "      <arg type='i' name='y' direction='in'/>"
  "    </method>"
  "    <method name='SecondaryActivate'>"
  "      <arg type='i' name='x' direction='in'/>"
  "      <arg type='i' name='y' direction='in'/>"
  "    </method>"
  "    <method name='ContextMenu'>"
  "      <arg type='i' name='x' direction='in'/>"
  "      <arg type='i' name='y' direction='in'/>"
  "    </method>"
  "    <signal name='NewStatus'>"
  "      <arg type='s' name='status'/>"
  "    </signal>"
  "    <property name='Category' type='s' access='read'/>"
  "    <property name='Id' type='s' access='read'/>"
  "    <property name='Title' type='s' access='read'/>"
  "    <property name='Status' type='s' access='read'/>"
  "    <property name='IconName' type='s' access='read'/>"
  "    <property name='Menu' type='o' access='read'/>"
  "    <property name='ItemIsMenu' type='b' access='read'/>"
  "  </interface>"
  "</node>";

/* com.canonical.dbusmenu D-Bus interface */
static const gchar dbusmenu_introspection_xml[] =
  "<node>"
  "  <interface name='com.canonical.dbusmenu'>"
  "    <method name='GetLayout'>"
  "      <arg type='i' name='parentId' direction='in'/>"
  "      <arg type='i' name='recursionDepth' direction='in'/>"
  "      <arg type='as' name='propertyNames' direction='in'/>"
  "      <arg type='u' name='revision' direction='out'/>"
  "      <arg type='(ia{sv}av)' name='layout' direction='out'/>"
  "    </method>"
  "    <method name='Event'>"
  "      <arg type='i' name='id' direction='in'/>"
  "      <arg type='s' name='eventId' direction='in'/>"
  "      <arg type='v' name='data' direction='in'/>"
  "      <arg type='u' name='timestamp' direction='in'/>"
  "    </method>"
  "    <method name='GetGroupProperties'>"
  "      <arg type='ai' name='ids' direction='in'/>"
  "      <arg type='as' name='propertyNames' direction='in'/>"
  "      <arg type='a(ia{sv})' name='properties' direction='out'/>"
  "    </method>"
  "    <method name='AboutToShow'>"
  "      <arg type='i' name='id' direction='in'/>"
  "      <arg type='b' name='needUpdate' direction='out'/>"
  "    </method>"
  "    <signal name='LayoutUpdated'>"
  "      <arg type='u' name='revision'/>"
  "      <arg type='i' name='parent'/>"
  "    </signal>"
  "    <property name='Version' type='u' access='read'/>"
  "    <property name='Status' type='s' access='read'/>"
  "  </interface>"
  "</node>";

struct _NdTray
{
  GApplication    *app;
  NdController    *controller;

  guint            sni_bus_name_id;
  guint            sni_registration_id;
  guint            dbusmenu_registration_id;
  GDBusConnection *connection;

  GDBusNodeInfo   *sni_node_info;
  GDBusNodeInfo   *dbusmenu_node_info;

  guint            layout_revision;

  /* Maps menu item id (gint, GINT_TO_POINTER) -> NdSink* (borrowed) */
  GHashTable      *id_to_sink;
  gint             next_sink_id;
};

static void nd_tray_register_with_watcher (NdTray *self);
static void notify_layout_updated (NdTray *self);

/* --- Helper: add a menu item to children builder --- */

static void
add_menu_item (GVariantBuilder *children,
               gint             id,
               const gchar     *label,
               gboolean         enabled)
{
  GVariantBuilder item_props;
  GVariantBuilder no_children;

  g_variant_builder_init (&item_props, G_VARIANT_TYPE ("a{sv}"));
  g_variant_builder_add (&item_props, "{sv}", "label",
                         g_variant_new_string (label));
  g_variant_builder_add (&item_props, "{sv}", "enabled",
                         g_variant_new_boolean (enabled));

  g_variant_builder_init (&no_children, G_VARIANT_TYPE ("av"));

  g_variant_builder_add (children, "v",
                         g_variant_new ("(ia{sv}av)", id,
                                        &item_props, &no_children));
}

static void
add_separator (GVariantBuilder *children, gint id)
{
  GVariantBuilder item_props;
  GVariantBuilder no_children;

  g_variant_builder_init (&item_props, G_VARIANT_TYPE ("a{sv}"));
  g_variant_builder_add (&item_props, "{sv}", "type",
                         g_variant_new_string ("separator"));

  g_variant_builder_init (&no_children, G_VARIANT_TYPE ("av"));

  g_variant_builder_add (children, "v",
                         g_variant_new ("(ia{sv}av)", id,
                                        &item_props, &no_children));
}

/* --- SNI interface --- */

static void
sni_method_call (GDBusConnection       *connection,
                 const gchar           *sender,
                 const gchar           *object_path,
                 const gchar           *interface_name,
                 const gchar           *method_name,
                 GVariant              *parameters,
                 GDBusMethodInvocation *invocation,
                 gpointer               user_data)
{
  /* Left-click and right-click both just acknowledge — menu is shown via dbusmenu */
  g_dbus_method_invocation_return_value (invocation, NULL);
}

static GVariant *
sni_get_property (GDBusConnection *connection,
                  const gchar     *sender,
                  const gchar     *object_path,
                  const gchar     *interface_name,
                  const gchar     *property_name,
                  GError         **error,
                  gpointer         user_data)
{
  if (g_strcmp0 (property_name, "Category") == 0)
    return g_variant_new_string ("ApplicationStatus");
  if (g_strcmp0 (property_name, "Id") == 0)
    return g_variant_new_string ("desktopcast");
  if (g_strcmp0 (property_name, "Title") == 0)
    return g_variant_new_string ("desktopCast");
  if (g_strcmp0 (property_name, "Status") == 0)
    return g_variant_new_string ("Active");
  if (g_strcmp0 (property_name, "IconName") == 0)
    return g_variant_new_string ("screen-shared-symbolic");
  if (g_strcmp0 (property_name, "Menu") == 0)
    return g_variant_new_object_path ("/MenuBar");
  if (g_strcmp0 (property_name, "ItemIsMenu") == 0)
    return g_variant_new_boolean (TRUE);

  g_set_error (error, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_PROPERTY,
               "Unknown property: %s", property_name);
  return NULL;
}

static const GDBusInterfaceVTable sni_vtable = {
  sni_method_call,
  sni_get_property,
  NULL,
};

/* --- dbusmenu interface --- */

static GVariant *
build_menu_layout (NdTray *self)
{
  GVariantBuilder root_props;
  GVariantBuilder children;
  NdSinkState state;

  g_variant_builder_init (&root_props, G_VARIANT_TYPE ("a{sv}"));
  g_variant_builder_add (&root_props, "{sv}", "children-display",
                         g_variant_new_string ("submenu"));

  g_variant_builder_init (&children, G_VARIANT_TYPE ("av"));

  /* Clear the id-to-sink mapping and rebuild */
  g_hash_table_remove_all (self->id_to_sink);
  self->next_sink_id = MENU_ID_SINK_BASE;

  state = nd_controller_get_state (self->controller);

  if (state == ND_SINK_STATE_DISCONNECTED)
    {
      if (!nd_controller_has_screen (self->controller))
        {
          /* No screen selected — show "Select Screen to Share" */
          add_menu_item (&children, MENU_ID_SELECT_SCREEN,
                         "Select Screen to Share", TRUE);
        }
      else
        {
          /* Screen selected, idle — show screen name + TV list */
          const gchar *screen_name = nd_controller_get_screen_name (self->controller);
          g_autofree gchar *sharing_label = g_strdup_printf ("Sharing: %s",
                                                              screen_name ? screen_name : "Screen");
          add_menu_item (&children, 10, sharing_label, FALSE);
          add_menu_item (&children, MENU_ID_SELECT_SCREEN, "Change Screen", TRUE);

          add_separator (&children, MENU_ID_SEPARATOR_1);

          guint n_sinks = nd_controller_get_n_sinks (self->controller);
          if (n_sinks == 0)
            {
              add_menu_item (&children, 11, "Searching for displays...", FALSE);
            }
          else
            {
              for (guint i = 0; i < n_sinks; i++)
                {
                  NdSink *sink = nd_controller_get_sink (self->controller, i);
                  if (!sink)
                    continue;

                  gint id = self->next_sink_id++;
                  g_autofree gchar *display_name = NULL;
                  g_object_get (sink, "display-name", &display_name, NULL);

                  const gchar *label = display_name ? display_name : "Unknown Display";
                  add_menu_item (&children, id, label, TRUE);

                  g_hash_table_insert (self->id_to_sink,
                                      GINT_TO_POINTER (id), sink);
                }
            }
        }
    }
  else if (state == ND_SINK_STATE_ERROR)
    {
      add_menu_item (&children, 10, "Connection error", FALSE);
      add_menu_item (&children, MENU_ID_CANCEL, "Dismiss", TRUE);
    }
  else if (state == ND_SINK_STATE_STREAMING)
    {
      NdSink *stream_sink = nd_controller_get_stream_sink (self->controller);
      g_autofree gchar *display_name = NULL;
      g_autofree gchar *label = NULL;

      if (stream_sink)
        g_object_get (stream_sink, "display-name", &display_name, NULL);

      label = g_strdup_printf ("Streaming to %s",
                               display_name ? display_name : "display");
      add_menu_item (&children, 10, label, FALSE);
      add_menu_item (&children, MENU_ID_SELECT_SCREEN, "Change Screen", TRUE);
      add_menu_item (&children, MENU_ID_DISCONNECT, "Disconnect", TRUE);
    }
  else
    {
      /* Connecting states */
      NdSink *stream_sink = nd_controller_get_stream_sink (self->controller);
      g_autofree gchar *display_name = NULL;
      g_autofree gchar *label = NULL;

      if (stream_sink)
        g_object_get (stream_sink, "display-name", &display_name, NULL);

      label = g_strdup_printf ("Connecting to %s...",
                               display_name ? display_name : "display");
      add_menu_item (&children, 10, label, FALSE);
      add_menu_item (&children, MENU_ID_CANCEL, "Cancel", TRUE);
    }

  /* Mute toggle */
  if (nd_controller_get_muted (self->controller))
    add_menu_item (&children, MENU_ID_MUTE_TOGGLE, "Unmute Audio", TRUE);
  else
    add_menu_item (&children, MENU_ID_MUTE_TOGGLE, "Mute Audio", TRUE);

  /* Separator + Debug + Quit */
  add_separator (&children, MENU_ID_SEPARATOR_2);

  if (nd_debug_log_get_verbose ())
    add_menu_item (&children, MENU_ID_DEBUG_TOGGLE, "Disable Debug Mode", TRUE);
  else
    add_menu_item (&children, MENU_ID_DEBUG_TOGGLE, "Enable Debug Mode", TRUE);

  add_menu_item (&children, MENU_ID_SAVE_LOG, "Save Debug Log", TRUE);
  add_menu_item (&children, MENU_ID_QUIT, "Quit", TRUE);

  return g_variant_new ("(u(ia{sv}av))",
                        self->layout_revision,
                        0, &root_props, &children);
}

static void
dbusmenu_method_call (GDBusConnection       *connection,
                      const gchar           *sender,
                      const gchar           *object_path,
                      const gchar           *interface_name,
                      const gchar           *method_name,
                      GVariant              *parameters,
                      GDBusMethodInvocation *invocation,
                      gpointer               user_data)
{
  NdTray *self = user_data;

  if (g_strcmp0 (method_name, "GetLayout") == 0)
    {
      g_dbus_method_invocation_return_value (invocation,
                                             build_menu_layout (self));
    }
  else if (g_strcmp0 (method_name, "Event") == 0)
    {
      gint32 id;
      const gchar *event_id;
      g_autoptr(GVariant) data = NULL;
      guint32 timestamp;

      g_variant_get (parameters, "(is@vu)", &id, &event_id, &data, &timestamp);

      if (g_strcmp0 (event_id, "clicked") == 0)
        {
          if (id >= MENU_ID_SINK_BASE)
            {
              /* Sink clicked — look up from hash table */
              NdSink *sink = g_hash_table_lookup (self->id_to_sink,
                                                  GINT_TO_POINTER (id));
              if (sink)
                nd_controller_connect_sink (self->controller, sink);
              else
                g_warning ("NdTray: Unknown sink id %d", id);
            }
          else
            {
              switch (id)
                {
                case MENU_ID_DISCONNECT:
                  nd_controller_disconnect (self->controller);
                  break;

                case MENU_ID_QUIT:
                  nd_controller_disconnect (self->controller);
                  g_application_quit (self->app);
                  break;

                case MENU_ID_CANCEL:
                  nd_controller_disconnect (self->controller);
                  break;

                case MENU_ID_SELECT_SCREEN:
                  /* Hot-swap: select_screen handles both streaming and idle cases */
                  nd_controller_select_screen (self->controller);
                  break;

                case MENU_ID_MUTE_TOGGLE:
                  nd_controller_set_muted (self->controller,
                                           !nd_controller_get_muted (self->controller));
                  notify_layout_updated (self);
                  break;

                case MENU_ID_DEBUG_TOGGLE:
                  nd_debug_log_set_verbose (!nd_debug_log_get_verbose ());
                  notify_layout_updated (self);
                  break;

                case MENU_ID_SAVE_LOG:
                  {
                    g_autofree gchar *path = nd_debug_log_save ();
                    if (path)
                      g_message ("Debug log saved: %s", path);
                  }
                  break;
                }
            }
        }

      g_dbus_method_invocation_return_value (invocation, NULL);
    }
  else if (g_strcmp0 (method_name, "GetGroupProperties") == 0)
    {
      GVariantBuilder builder;
      g_variant_builder_init (&builder, G_VARIANT_TYPE ("a(ia{sv})"));
      g_dbus_method_invocation_return_value (invocation,
                                             g_variant_new ("(a(ia{sv}))",
                                                            &builder));
    }
  else if (g_strcmp0 (method_name, "AboutToShow") == 0)
    {
      g_dbus_method_invocation_return_value (invocation,
                                             g_variant_new ("(b)", FALSE));
    }
  else
    {
      g_dbus_method_invocation_return_error (invocation,
                                             G_DBUS_ERROR,
                                             G_DBUS_ERROR_UNKNOWN_METHOD,
                                             "Unknown method: %s",
                                             method_name);
    }
}

static GVariant *
dbusmenu_get_property (GDBusConnection *connection,
                       const gchar     *sender,
                       const gchar     *object_path,
                       const gchar     *interface_name,
                       const gchar     *property_name,
                       GError         **error,
                       gpointer         user_data)
{
  if (g_strcmp0 (property_name, "Version") == 0)
    return g_variant_new_uint32 (3);
  if (g_strcmp0 (property_name, "Status") == 0)
    return g_variant_new_string ("normal");

  g_set_error (error, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_PROPERTY,
               "Unknown property: %s", property_name);
  return NULL;
}

static const GDBusInterfaceVTable dbusmenu_vtable = {
  dbusmenu_method_call,
  dbusmenu_get_property,
  NULL,
};

/* --- Bus name callbacks --- */

static void
on_bus_name_acquired (GDBusConnection *connection,
                      const gchar     *name,
                      gpointer         user_data)
{
  NdTray *self = user_data;

  g_debug ("NdTray: Acquired bus name %s", name);
  self->connection = connection;

  /* Register SNI object */
  g_autoptr(GError) error = NULL;
  self->sni_registration_id =
    g_dbus_connection_register_object (connection,
                                       "/StatusNotifierItem",
                                       self->sni_node_info->interfaces[0],
                                       &sni_vtable,
                                       self,
                                       NULL,
                                       &error);
  if (error)
    {
      g_warning ("NdTray: Failed to register SNI object: %s", error->message);
      return;
    }

  /* Register dbusmenu object */
  self->dbusmenu_registration_id =
    g_dbus_connection_register_object (connection,
                                       "/MenuBar",
                                       self->dbusmenu_node_info->interfaces[0],
                                       &dbusmenu_vtable,
                                       self,
                                       NULL,
                                       &error);
  if (error)
    {
      g_warning ("NdTray: Failed to register dbusmenu object: %s", error->message);
      return;
    }

  nd_tray_register_with_watcher (self);
}

static void
on_bus_name_lost (GDBusConnection *connection,
                  const gchar     *name,
                  gpointer         user_data)
{
  g_debug ("NdTray: Lost bus name %s", name);
}

/* --- Register with StatusNotifierWatcher --- */

static void
register_cb (GObject      *source_object,
             GAsyncResult *result,
             gpointer      user_data)
{
  g_autoptr(GVariant) ret = NULL;
  g_autoptr(GError) error = NULL;

  ret = g_dbus_connection_call_finish (G_DBUS_CONNECTION (source_object),
                                       result, &error);
  if (error)
    g_debug ("NdTray: Failed to register with StatusNotifierWatcher: %s",
             error->message);
  else
    g_debug ("NdTray: Registered with StatusNotifierWatcher");
}

static void
nd_tray_register_with_watcher (NdTray *self)
{
  g_autofree gchar *bus_name = NULL;

  bus_name = g_strdup_printf ("org.kde.StatusNotifierItem-%d-1", getpid ());

  g_dbus_connection_call (self->connection,
                          "org.kde.StatusNotifierWatcher",
                          "/StatusNotifierWatcher",
                          "org.kde.StatusNotifierWatcher",
                          "RegisterStatusNotifierItem",
                          g_variant_new ("(s)", bus_name),
                          NULL,
                          G_DBUS_CALL_FLAGS_NONE,
                          -1,
                          NULL,
                          register_cb,
                          self);
}

/* --- Signal handlers from controller --- */

static void
notify_layout_updated (NdTray *self)
{
  self->layout_revision++;

  if (!self->connection)
    return;

  g_dbus_connection_emit_signal (self->connection,
                                 NULL,
                                 "/MenuBar",
                                 "com.canonical.dbusmenu",
                                 "LayoutUpdated",
                                 g_variant_new ("(ui)",
                                                self->layout_revision, 0),
                                 NULL);
}

static void
on_sinks_changed (NdController *controller, NdTray *self)
{
  notify_layout_updated (self);
}

static void
on_state_changed (NdController *controller, guint state, NdTray *self)
{
  notify_layout_updated (self);

  if (!self->connection)
    return;

  /* Emit NewStatus on SNI */
  g_dbus_connection_emit_signal (self->connection,
                                 NULL,
                                 "/StatusNotifierItem",
                                 "org.kde.StatusNotifierItem",
                                 "NewStatus",
                                 g_variant_new ("(s)", "Active"),
                                 NULL);
}

/* --- Public API --- */

NdTray *
nd_tray_new (GApplication *app, NdController *controller)
{
  NdTray *self;
  g_autofree gchar *bus_name = NULL;

  g_return_val_if_fail (G_IS_APPLICATION (app), NULL);
  g_return_val_if_fail (ND_IS_CONTROLLER (controller), NULL);

  self = g_new0 (NdTray, 1);
  self->app = app;
  self->controller = controller;
  self->layout_revision = 1;
  self->next_sink_id = MENU_ID_SINK_BASE;
  self->id_to_sink = g_hash_table_new (g_direct_hash, g_direct_equal);

  self->sni_node_info = g_dbus_node_info_new_for_xml (sni_introspection_xml, NULL);
  self->dbusmenu_node_info = g_dbus_node_info_new_for_xml (dbusmenu_introspection_xml, NULL);

  if (!self->sni_node_info || !self->dbusmenu_node_info)
    {
      g_warning ("NdTray: Failed to parse introspection XML");
      g_hash_table_unref (self->id_to_sink);
      g_free (self);
      return NULL;
    }

  /* Connect to controller signals */
  g_signal_connect (controller, "sinks-changed",
                    G_CALLBACK (on_sinks_changed), self);
  g_signal_connect (controller, "state-changed",
                    G_CALLBACK (on_state_changed), self);
  g_signal_connect (controller, "screen-changed",
                    G_CALLBACK (on_sinks_changed), self);

  bus_name = g_strdup_printf ("org.kde.StatusNotifierItem-%d-1", getpid ());

  self->sni_bus_name_id =
    g_bus_own_name (G_BUS_TYPE_SESSION,
                    bus_name,
                    G_BUS_NAME_OWNER_FLAGS_NONE,
                    on_bus_name_acquired,
                    NULL,
                    on_bus_name_lost,
                    self,
                    NULL);

  return self;
}

void
nd_tray_destroy (NdTray *self)
{
  if (self == NULL)
    return;

  /* Disconnect controller signals */
  if (self->controller)
    g_signal_handlers_disconnect_by_data (self->controller, self);

  if (self->connection)
    {
      if (self->sni_registration_id > 0)
        g_dbus_connection_unregister_object (self->connection,
                                             self->sni_registration_id);
      if (self->dbusmenu_registration_id > 0)
        g_dbus_connection_unregister_object (self->connection,
                                             self->dbusmenu_registration_id);
    }

  if (self->sni_bus_name_id > 0)
    g_bus_unown_name (self->sni_bus_name_id);

  g_clear_pointer (&self->sni_node_info, g_dbus_node_info_unref);
  g_clear_pointer (&self->dbusmenu_node_info, g_dbus_node_info_unref);
  g_clear_pointer (&self->id_to_sink, g_hash_table_unref);

  g_free (self);
}
